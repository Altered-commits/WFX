// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_HTTP_USER_REQUEST_HPP
#define WFX_INC_HTTP_USER_REQUEST_HPP

#include "core/core.hpp"
#include "shared/apis/http_api.hpp"
#include "shared/utils/memory.hpp"
#include <array>
#include <cstdint>
#include <string_view>
#include <cstring>

namespace WFX::Http {

namespace Detail {

// One parameter as offsets into the query it came from, rather than two pointers and two lengths.
// Six bytes against thirty two, which is what lets a whole set sit inline instead of on the heap
struct QuerySlot {
    std::uint16_t start = 0; // first character of the key
    std::uint16_t eq = 0;    // the '=', or end when the key came with no value
    std::uint16_t end = 0;   // one past the last character of the value
};

// Three 16 bit offsets and no padding, which is the whole reason a set of these sits inline: a
// scan of sixteen touches two cache lines where sixteen string view pairs would touch eight
static_assert(sizeof(QuerySlot) == 6, "'QuerySlot' must stay three 16 bit offsets");
static_assert(alignof(QuerySlot) == 2, "'QuerySlot' must not be padded up");

// Those offsets are sixteen bits, so a query longer than this cannot be addressed by them. The
// server caps a request line far below it, so reaching this means something else already went wrong
inline constexpr std::size_t QUERY_MAX_LEN = 0xFFFF;

// Walks "key=value&key=value", handing each pair to fill until it returns false, which is how a
// caller that has run out of room ends the walk. True when the whole query was walked
template <typename Fn> bool ParseQuery(std::string_view query, Fn&& fill) noexcept
{
    if(query.size() > QUERY_MAX_LEN)
        return false;

    for(std::size_t pos = 0; pos <= query.size();) {
        const auto amp = query.find('&', pos);
        const std::size_t end = (amp == std::string_view::npos) ? query.size() : amp;

        if(end > pos) {
            // Searched inside the pair, not from it to the end: a search running past this pair is
            // thrown away and the next one rescans the same tail, which is quadratic on "a&a&a&a=1".
            // The '&' search above cannot do that, since where it lands becomes the next start
            const auto eq = query.substr(pos, end - pos).find('=');
            const std::size_t at = (eq == std::string_view::npos) ? end : pos + eq;

            if(!fill(QuerySlot{static_cast<std::uint16_t>(pos), static_cast<std::uint16_t>(at),
                               static_cast<std::uint16_t>(end)}))
                return false;
        }

        if(amp == std::string_view::npos)
            break;

        pos = amp + 1;
    }

    return true;
}

// What both forms answer with, given the query they were parsed from
inline bool SlotGet(std::string_view query, const QuerySlot* slots, std::size_t count, std::string_view key,
                    std::string_view& out) noexcept
{
    for(std::size_t i = 0; i < count; ++i) {
        const auto& s = slots[i];
        if(query.substr(s.start, s.eq - s.start) != key)
            continue;

        out = (s.eq == s.end) ? std::string_view{} : query.substr(s.eq + 1, s.end - s.eq - 1);
        return true;
    }

    return false;
}

} // namespace Detail

// Parses "key=value&key=value" once into offsets into that same buffer, so nothing is copied. Get()
// is a flat scan rather than a hash map, which wins at this size on every axis that matters, and a
// query carrying more than maxParams is parsed up to it with Truncated() saying so
class QueryParams {
public:
    // What a route realistically reads, not what a request line can hold: eight kilobytes of it would
    // otherwise become four thousand slots. A route that genuinely wants more says so through the
    // maxParams argument, or holds them inline with GetQueryParams<N>()
    static constexpr std::size_t DEFAULT_MAX_PARAMS = 32;

public:
    explicit QueryParams(std::string_view query, std::size_t maxParams = DEFAULT_MAX_PARAMS) noexcept : query_(query)
    {
        complete_ = Detail::ParseQuery(query, [&](Detail::QuerySlot slot) {
            if(slots_.size() == maxParams)
                return false;

            slots_.push_back(slot);
            return true;
        });
    }

public:
    // Raw value, still percent-encoded if the client sent it that way. WFX doesn't decode this
    // for you, same as GetHeader doesn't decode header values, decode it yourself if you need to
    bool Get(std::string_view key, std::string_view& out) const noexcept
    {
        return Detail::SlotGet(query_, slots_.data(), slots_.size(), key, out);
    }

    std::size_t Count() const noexcept
    {
        return slots_.size();
    }

    // Whether the query carried more than was parsed, so a key Get() did not find might still have
    // been sent. A route that cares answers rather than reading a partial set as the whole
    bool Truncated() const noexcept
    {
        return !complete_;
    }

private:
    std::string_view query_;
    Shared::Vector<Detail::QuerySlot> slots_;
    bool complete_ = true;
};

// The same, sized by the caller and held inline: parsing allocates nothing, so a handler that knows
// it reads four parameters pays four slots and no heap at all. Anything past N is not parsed and
// Truncated() says so, which is the caller's own cap rather than a surprise
template <std::size_t N> class FixedQueryParams {
public:
    explicit FixedQueryParams(std::string_view query) noexcept : query_(query)
    {
        complete_ = Detail::ParseQuery(query, [&](Detail::QuerySlot slot) {
            if(count_ == N)
                return false;

            slots_[count_++] = slot;
            return true;
        });
    }

public:
    bool Get(std::string_view key, std::string_view& out) const noexcept
    {
        return Detail::SlotGet(query_, slots_.data(), count_, key, out);
    }

    std::size_t Count() const noexcept
    {
        return count_;
    }

    bool Truncated() const noexcept
    {
        return !complete_;
    }

private:
    std::string_view query_;
    std::array<Detail::QuerySlot, N> slots_{};
    std::size_t count_ = 0;
    bool complete_ = true;
};

/* User side implementation of 'Request'. 'CoreEngine' passes the API */
class Request {
public:
    Request(void* backend) : backend_(backend)
    {}

public:
    Shared::HttpMethod Method() const
    {
        return Core::HttpApiExt1()->getMethod(backend_);
    }

    Shared::HttpVersion Version() const
    {
        return Core::HttpApiExt1()->getVersion(backend_);
    }

    std::string_view Path() const
    {
        auto sv = Core::HttpApiExt1()->getPath(backend_);
        return {sv.data, static_cast<std::size_t>(sv.length)};
    }

    std::string_view Body() const
    {
        auto sv = Core::HttpApiExt1()->getBody(backend_);
        return {sv.data, static_cast<std::size_t>(sv.length)};
    }

    // Everything after '?', empty when the path carries none. Exactly as the client sent it: WFX
    // decodes no part of a query string, the same way it decodes no header value
    std::string_view RawQuery() const
    {
        const auto path = Path();
        const auto qpos = path.find('?');

        return (qpos == std::string_view::npos) ? std::string_view{} : path.substr(qpos + 1);
    }

    // Path() keeps the raw '?...' suffix as-is. Parses it into a QueryParams once per call, so
    // if you're reading several keys, call this once and reuse the result rather than calling it
    // per key. Past maxParams the set is truncated and says so
    QueryParams GetQueryParams(std::size_t maxParams = QueryParams::DEFAULT_MAX_PARAMS) const
    {
        return QueryParams(RawQuery(), maxParams);
    }

    // The same held inline for N parameters, as req.GetQueryParams<4>(), so parsing allocates
    // nothing. Use it where the count is known up front, which is most handlers
    template <std::size_t N> FixedQueryParams<N> GetQueryParams() const
    {
        return FixedQueryParams<N>(RawQuery());
    }

public:
    bool GetHeader(std::string_view key, std::string_view& out) const
    {
        const Shared::StringView k = ToSV(key);
        Shared::StringView val{};

        const bool ok = Core::HttpApiExt1()->getHeader(backend_, k, &val);
        if(!ok)
            return false;

        out = {val.data, static_cast<std::size_t>(val.length)};
        return true;
    }

public:
    std::uint64_t SegmentCount() const
    {
        return Core::HttpApiExt1()->getSegmentCount(backend_);
    }

    Shared::SegmentVariant GetSegment(std::uint64_t index) const
    {
        return Core::HttpApiExt1()->getSegment(backend_, index);
    }

public:
    template <typename T> bool SetContext(std::string_view key, T&& value)
    {
        using U = std::decay_t<T>;

        auto k = ToSV(key);

        Shared::Any any{};
        any.typeID = Shared::Any::TypeIDOf<U>();

        if constexpr(sizeof(U) <= sizeof(void*) && alignof(U) <= alignof(void*) && std::is_trivially_copyable_v<U> &&
                     std::is_trivially_destructible_v<U>) {
            U tmp = std::forward<T>(value);
            std::memcpy(&any.data, &tmp, sizeof(U));
            any.destructor = nullptr;
        }
        else {
            U* obj = Shared::New<U>(std::forward<T>(value));
            if(!obj)
                return false;

            any.data = obj;
            any.destructor = [](void* p) { Shared::Delete(static_cast<U*>(p)); };
        }

        Core::HttpApiExt1()->setContext(backend_, k, any);
        return true;
    }

    template <typename T> auto GetContext(std::string_view key) const
    {
        using U = std::decay_t<T>;

        auto k = ToSV(key);
        Shared::Any any{};

        // For trivial types, return val + bool
        if constexpr(sizeof(U) <= sizeof(void*) && alignof(U) <= alignof(void*) && std::is_trivially_copyable_v<U> &&
                     std::is_trivially_destructible_v<U>) {
            if(!Core::HttpApiExt1()->getContext(backend_, k, &any) || any.typeID != Shared::Any::TypeIDOf<U>())
                return std::pair<U, bool>{{}, false};

            U out{};
            std::memcpy(&out, &any.data, sizeof(U));

            return std::pair<U, bool>{out, true};
        }
        // For non-trivial types, we return ptr + bool
        else {
            if(!Core::HttpApiExt1()->getContext(backend_, k, &any))
                return std::pair<U*, bool>{nullptr, false};

            return std::pair<U*, bool>{any.As<U>(), true};
        }
    }

    void EraseContext(std::string_view key)
    {
        auto k = ToSV(key);
        Core::HttpApiExt1()->eraseContext(backend_, k);
    }

private:
    static Shared::StringView ToSV(std::string_view s)
    {
        return {s.data(), static_cast<std::uint64_t>(s.size())};
    }

private:
    void* backend_;
};

} // namespace WFX::Http

#endif // WFX_INC_HTTP_USER_REQUEST_HPP