// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_ENDPOINT_POSTGRES_STMT_CACHE_HPP
#define WFX_INC_WFX_ENDPOINT_POSTGRES_STMT_CACHE_HPP

// -----------------------------------------------------------------------
// Prepared statement cache, one table per connection.
//
// An unnamed statement lives only until the next one replaces it, so every
// request makes the server parse and plan SQL it has already seen. Naming the
// statement keeps the plan on the server, and a later request that matches
// sends Bind, Describe and Execute with no Parse at all.
//
// Prepared statements are session state, so a table belongs to one connection.
// The epoch is what is shared: when the server rejects a cached plan after a
// schema change, it moves once and every entry on every connection re-parses
// the next time it is used.
//
// SQL is only named once it has been seen minUses times. Below that it goes
// out unnamed, so one-off statements cost the server nothing and cannot take a
// name away from a statement that is actually hot.
//
// The table is open addressed with the name rendered at startup, which keeps a
// lookup to one hash and at most STMT_PROBE_LEN comparisons and allocates
// nothing once an entry exists.
// -----------------------------------------------------------------------

#include "wfx/memory.hpp"
#include "wfx/utils/hash.hpp"

#include <charconv>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace WFX::Postgres::Detail {

// The plan behind a prepared statement no longer matches the schema
inline constexpr std::string_view SQLSTATE_FEATURE_NOT_SUPPORTED = "0A000";

// The server has no statement under that name, which is what a session reset
// on the far side looks like from here
inline constexpr std::string_view SQLSTATE_INVALID_STATEMENT_NAME = "26000";

// 's' followed by the entry index, which the cache size bounds
inline constexpr std::size_t STMT_NAME_MAX = 8;

// How far a lookup walks past its home index before taking the name off
// whatever it lands on. Four keeps two statements that hash together from
// evicting each other on every request.
inline constexpr std::uint32_t STMT_PROBE_LEN = 4;

// A table is rounded up to a power of two, so a lookup masks instead of dividing
inline constexpr std::uint32_t STMT_CACHE_MAX = 4096;

inline constexpr std::uint32_t STMT_NONE = 0xFFFFFFFFu;

// Epoch of an entry whose SQL the server has never parsed
inline constexpr std::uint32_t STMT_EPOCH_UNPARSED = 0;

// -----------------------------------------------------------------------
// Shared by every connection to one endpoint. A rejected plan moves the epoch
// once rather than once per connection, since the schema change behind it
// applies to all of them.
// -----------------------------------------------------------------------
struct PgCacheControl {
    std::uint32_t epoch = 1;

    void Invalidate() noexcept
    {
        // Zero marks an unparsed entry, so the live epoch steps over it
        if(++epoch == STMT_EPOCH_UNPARSED)
            epoch = 1;
    }
};

// -----------------------------------------------------------------------
// One table slot. The name comes from the index and never changes, so it
// always refers to this entry whatever SQL currently occupies it.
// -----------------------------------------------------------------------
struct PgStatementEntry {
    WFX::String sql;

    // Points at the per-instantiation constant behind the request, which has
    // static storage duration and outlives the table
    const std::uint32_t* oids = nullptr;

    std::uint64_t hash = 0;

    // Epoch the SQL now in this entry was parsed at. Anything other than the
    // live epoch means it has to be parsed again before it can be bound.
    std::uint32_t epoch = STMT_EPOCH_UNPARSED;

    std::uint16_t paramCount = 0;

    // Executions seen, capped at the threshold, which is what decides whether
    // this SQL is worth a name
    std::uint16_t uses = 0;

    std::uint8_t nameLen = 0;

    // The server holds a statement under this name, whatever its SQL is now
    bool live = false;

    char name[STMT_NAME_MAX] = {};
};

// What one lookup tells the serializer to send. An empty name means the
// unnamed statement, which is where SQL below the threshold goes.
struct PgStatementLookup {
    std::string_view name;
    std::uint32_t index = STMT_NONE;
    bool parse = true;
    bool close = false;
};

// -----------------------------------------------------------------------
// PgStatementCache
// -----------------------------------------------------------------------
class PgStatementCache {
public: // Setup
    // Sizes the table and renders every name once, so nothing is formatted on
    // the request path. A size of zero leaves the cache off.
    void Init(std::uint32_t size, std::uint16_t minUses)
    {
        if(size == 0)
            return;

        std::uint32_t cap = 1;
        while(cap < size && cap < STMT_CACHE_MAX)
            cap <<= 1;

        entries_.resize(cap);
        mask_ = cap - 1;
        minUses_ = minUses == 0 ? 1 : minUses;

        for(std::uint32_t i = 0; i < cap; ++i) {
            auto& e = entries_[i];
            e.name[0] = 's';

            const auto [ptr, ec] = std::to_chars(e.name + 1, e.name + STMT_NAME_MAX, i);
            e.nameLen = static_cast<std::uint8_t>(ec == std::errc{} ? ptr - e.name : 1);
        }
    }

public: // Lookup
    // Finds the entry for one statement, claiming a name for it when it is not
    // already there. Every flag comes from stored state, so serializing the
    // same request twice asks for the same messages both times, which is what
    // makes the retry after BUFFER_TOO_SMALL safe.
    PgStatementLookup Acquire(std::string_view sql, const std::uint32_t* oids, std::uint16_t paramCount,
                              std::uint32_t epoch)
    {
        if(entries_.empty())
            return {};

        const std::uint64_t h = Hash(sql, oids, paramCount);
        const std::uint32_t home = static_cast<std::uint32_t>(h) & mask_;

        std::uint32_t victim = home;
        std::uint16_t victimUses = 0xFFFFu;

        for(std::uint32_t i = 0; i < STMT_PROBE_LEN; ++i) {
            const std::uint32_t idx = (home + i) & mask_;
            auto& e = entries_[idx];

            if(e.sql.empty()) {
                victim = idx;
                break;
            }

            if(e.hash == h && e.paramCount == paramCount && e.sql == sql && SameOids(e.oids, oids, paramCount))
                return Describe(e, idx, epoch);

            // Whichever entry has been asked for least gives up its name, so a
            // statement run once cannot displace one a route runs constantly
            if(e.uses < victimUses) {
                victim = idx;
                victimUses = e.uses;
            }
        }

        // Nothing matched, so the entry the walk ended on gives up its SQL. Its
        // name stays prepared server side until the Close that goes out with
        // the Parse, which is why live carries over untouched.
        auto& e = entries_[victim];
        e.sql.assign(sql.data(), sql.size());
        e.oids = oids;
        e.hash = h;
        e.paramCount = paramCount;
        e.epoch = STMT_EPOCH_UNPARSED;
        e.uses = 0;

        return Describe(e, victim, epoch);
    }

public: // Outcome
    // ParseComplete came back, so the name is bound to this SQL at this epoch
    void MarkParsed(std::uint32_t index, std::uint32_t epoch) noexcept
    {
        if(index >= entries_.size())
            return;

        auto& e = entries_[index];
        e.live = true;
        e.epoch = epoch;
    }

    // The Parse never completed. Whatever the name held was closed alongside
    // it, so the server has nothing under it now.
    void MarkFailed(std::uint32_t index) noexcept
    {
        if(index >= entries_.size())
            return;

        auto& e = entries_[index];
        e.live = false;
        e.epoch = STMT_EPOCH_UNPARSED;
    }

private:
    PgStatementLookup Describe(PgStatementEntry& e, std::uint32_t idx, std::uint32_t epoch) noexcept
    {
        if(e.uses < minUses_)
            ++e.uses;

        // Below the threshold this SQL has not earned a name, so it goes out
        // unnamed and leaves no plan behind on the server
        if(e.uses < minUses_)
            return {};

        PgStatementLookup out;
        out.name = {e.name, e.nameLen};
        out.index = idx;
        out.parse = e.epoch != epoch;
        out.close = out.parse && e.live;
        return out;
    }

    static std::uint64_t Hash(std::string_view sql, const std::uint32_t* oids, std::uint16_t paramCount) noexcept
    {
        const std::uint64_t h = Xxh3(sql);

        if(paramCount == 0)
            return h;

        // The same text with different parameter types is a different
        // statement, because Parse is what declares those types
        const std::string_view raw{reinterpret_cast<const char*>(oids), paramCount * sizeof(std::uint32_t)};
        return HashCombine(h, Xxh3(raw));
    }

    static bool SameOids(const std::uint32_t* a, const std::uint32_t* b, std::uint16_t count) noexcept
    {
        if(a == b || count == 0)
            return true;

        if(!a || !b)
            return false;

        return std::memcmp(a, b, count * sizeof(std::uint32_t)) == 0;
    }

private:
    WFX::Vector<PgStatementEntry> entries_;
    std::uint32_t mask_ = 0;
    std::uint16_t minUses_ = 1;
};

} // namespace WFX::Postgres::Detail

#endif // WFX_INC_WFX_ENDPOINT_POSTGRES_STMT_CACHE_HPP
