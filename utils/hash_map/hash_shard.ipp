// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_UTILS_HASH_SHARD_IPP
#define WFX_UTILS_HASH_SHARD_IPP

#include "utils/diagnostics/logger.hpp"
#include "shared/utils/memory.hpp"
#include <cstring>
#include <limits>
#include <algorithm>

#if defined(_MSC_VER)
#include <intrin.h>
#define PREFETCH_T3(ptr) _mm_prefetch(reinterpret_cast<const char*>(ptr), _MM_HINT_T0)
#elif defined(__GNUC__) || defined(__clang__)
#define PREFETCH_T3(ptr) __builtin_prefetch((ptr), 0, 3)
#else
#define PREFETCH_T3(ptr) ((void)0)
#endif

#undef max

namespace WFX::Utils {

template <typename K, typename V> void HashShard<K, V>::DestroySlots(Entry* slots, std::size_t count) noexcept
{
    for(std::size_t i = 0; i < count; ++i)
        slots[i].~Entry();
}

template <typename K, typename V> std::size_t HashShard<K, V>::SafeByteSize(std::size_t count)
{
    constexpr std::size_t MAX_COUNT = std::numeric_limits<std::size_t>::max() / sizeof(Entry);

    if(count > MAX_COUNT)
        GetLogger().Fatal("[HashShard]: Requested capacity of ", count, " entries overflows allocation size");

    return count * sizeof(Entry);
}

template <typename K, typename V> HashShard<K, V>::~HashShard()
{
    if(!entries_)
        return;

    DestroySlots(entries_, capacity_);
    Shared::Free(entries_);
}

template <typename K, typename V>
HashShard<K, V>::HashShard(HashShard&& other) noexcept
    : entries_(other.entries_), capacity_(other.capacity_), initialBucketCapacity_(other.initialBucketCapacity_),
      size_(other.size_)
{
    other.entries_ = nullptr;
    other.capacity_ = 0;
    other.initialBucketCapacity_ = 0;
    other.size_ = 0;
}

template <typename K, typename V> HashShard<K, V>& HashShard<K, V>::operator=(HashShard&& other) noexcept
{
    if(this == &other)
        return *this;

    if(entries_) {
        DestroySlots(entries_, capacity_);
        Shared::Free(entries_);
    }

    entries_ = other.entries_;
    capacity_ = other.capacity_;
    initialBucketCapacity_ = other.initialBucketCapacity_;
    size_ = other.size_;

    other.entries_ = nullptr;
    other.capacity_ = 0;
    other.initialBucketCapacity_ = 0;
    other.size_ = 0;

    return *this;
}

template <typename K, typename V> void HashShard<K, V>::Init(std::size_t cap)
{
    // One-time init only
    if(entries_)
        GetLogger().Fatal("[HashShard]: Init() called twice");

    // Masking (capacity_ - 1) everywhere below requires a power-of-2 capacity. bit_ceil(0) == 1,-
    // -so a 0 request still yields a valid (if degenerate) 1-slot table rather than UB
    cap = std::bit_ceil(cap == 0 ? std::size_t{1} : cap);

    initialBucketCapacity_ = cap;
    capacity_ = cap;
    entries_ = reinterpret_cast<Entry*>(Shared::Alloc(SafeByteSize(cap)));

    if(!entries_)
        GetLogger().Fatal("[HashShard]: Failed to get memory for entries");

    for(std::size_t i = 0; i < cap; ++i)
        new (&entries_[i]) Entry{};
}

template <typename K, typename V> inline bool HashShard<K, V>::KeysEqual(const K& a, const K& b) const
{
    // 'has_unique_object_representations_v' (unlike a raw sizeof/is_trivially_copyable check) also-
    // -guarantees no padding bytes, so there's no risk of two logically-equal keys comparing unequal-
    // -because of uninitialized padding garbage
    if constexpr(std::has_unique_object_representations_v<K> && (sizeof(K) == 4 || sizeof(K) == 8))
        return std::memcmp(&a, &b, sizeof(K)) == 0;
    else
        return a == b;
}

template <typename K, typename V> bool HashShard<K, V>::Resize(std::size_t newCapacity)
{
    if(!entries_) {
        GetLogger().Error("[HashShard]: Resize() called before Init()");
        return false;
    }

    if(newCapacity == 0) {
        constexpr std::size_t MAX_CAPACITY = std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
        if(capacity_ > MAX_CAPACITY) {
            GetLogger().Error("[HashShard]: Capacity doubling overflow guard triggered at ", capacity_, " entries");
            return false;
        }

        newCapacity = capacity_ * 2;
    }

    // No-op, current table already satisfies the (shrink) request
    if(newCapacity < initialBucketCapacity_)
        return true;

    // Never shrink via the growth path
    if(newCapacity < capacity_)
        newCapacity = capacity_;

    newCapacity = std::bit_ceil(newCapacity);

    // No-op, already at the requested size
    if(newCapacity == capacity_)
        return true;

    // Allocation is opportunistic: back off toward 'capacity_' a few times before giving up-
    // -entirely, mirroring BufferPool's expansion-retry approach rather than crashing the-
    // -process over what may just be transient memory pressure
    Entry* newEntries = nullptr;
    std::size_t triedCapacity = newCapacity;

    for(std::size_t attempt = 0; attempt < MAX_RESIZE_BACKOFF_ATTEMPTS; ++attempt) {
        newEntries = reinterpret_cast<Entry*>(Shared::Alloc(SafeByteSize(triedCapacity)));
        if(newEntries)
            break;

        GetLogger().Warn("[HashShard]: Resize allocation of ", triedCapacity, " entries failed, backing off");

        std::size_t halfway = capacity_ + (triedCapacity - capacity_) / 2;
        halfway = std::bit_ceil(std::max(halfway, capacity_ + 1));

        // No more room to back off
        if(halfway >= triedCapacity)
            break;

        triedCapacity = halfway;
    }

    if(!newEntries) {
        GetLogger().Error("[HashShard]: All resize attempts failed, staying at capacity=", capacity_);
        return false;
    }

    newCapacity = triedCapacity;

    for(std::size_t i = 0; i < newCapacity; ++i)
        new (&newEntries[i]) Entry{};

    for(std::size_t i = 0; i < capacity_; ++i) {
        Entry& currentEntry = entries_[i];
        if(!currentEntry.occupied)
            continue;

        const std::size_t hash = WFXHash(currentEntry.key);
        const std::size_t idx = hash & (newCapacity - 1);
        std::size_t probe = 0;

        while(probe < MAX_PROBE_LIMIT) {
            const std::size_t pos = (idx + probe) & (newCapacity - 1);
            Entry& target = newEntries[pos];
            PREFETCH_T3(reinterpret_cast<const char*>(&newEntries[(pos + 1) & (newCapacity - 1)]));

            if(!target.occupied) {
                currentEntry.probeLength = static_cast<std::uint8_t>(probe);
                target = std::move(currentEntry);
                break;
            }

            if(target.probeLength < probe) {
                std::swap(target, currentEntry);
                currentEntry.probeLength = static_cast<std::uint8_t>(probe);
                probe = target.probeLength;
            }

            ++probe;
        }
    }

    // Every old slot (moved-from occupied ones and never-occupied default-constructed ones alike)-
    // -is still a live object and must be destroyed before the backing buffer is freed
    DestroySlots(entries_, capacity_);
    Shared::Free(entries_);
    entries_ = newEntries;
    capacity_ = newCapacity;

    return true;
}

template <typename K, typename V> void HashShard<K, V>::BackwardShiftErase(std::size_t pos)
{
    const std::size_t mask = capacity_ - 1;
    std::size_t j = pos;
    std::size_t next = (j + 1) & mask;

    // Deletion by backward shifting of values
    while(entries_[next].occupied && entries_[next].probeLength > 0) {
        entries_[j] = std::move(entries_[next]);

        entries_[next].occupied = false;
        entries_[next].probeLength = 0;

        entries_[j].probeLength--;

        j = next;
        next = (j + 1) & mask;
    }

    // Reset the vacated slot back to a fresh, default-constructed entry
    entries_[j].~Entry();
    new (&entries_[j]) Entry{};

    --size_;
}

template <typename K, typename V> bool HashShard<K, V>::Emplace(const K& key, V&& value)
{
    if(!entries_) {
        GetLogger().Error("[HashShard]: Emplace() called before Init()");
        return false;
    }

    Entry newEntry{key, std::move(value), 0, true};

    for(std::size_t growthAttempt = 0; growthAttempt < MAX_GROWTH_RETRIES; ++growthAttempt) {
        if(static_cast<float>(size_) / capacity_ >= KLOAD_FACTOR_GROW)
            (void)Resize();

        std::size_t mask = capacity_ - 1;
        std::size_t hash = WFXHash(key);
        const std::size_t idx = hash & mask;
        std::size_t probe = 0;

        while(probe < MAX_PROBE_LIMIT) {
            const std::size_t pos = (idx + probe) & mask;
            Entry& entry = entries_[pos];

            PREFETCH_T3(reinterpret_cast<const char*>(&entries_[(pos + 1) & mask]));

            if(!entry.occupied) {
                newEntry.probeLength = static_cast<std::uint8_t>(probe);
                entry = std::move(newEntry);
                size_++;
                return true;
            }

            if(KeysEqual(entry.key, key)) {
                entry.value = std::move(newEntry.value); // overwrite
                return true;
            }

            if(entry.probeLength < probe) {
                std::swap(entry, newEntry);
                newEntry.probeLength = static_cast<std::uint8_t>(probe);
                probe = entry.probeLength;
            }

            ++probe;
        }

        // Probe limit exceeded. 'newEntry' at this point may hold a robin-hood-displaced entry-
        // -that was already present in the table (not necessarily the caller's key), so we can't-
        // -just return false here without silently dropping it. Force growth and retry the-
        // -insertion of whatever 'newEntry' currently holds. If growth itself fails, give up-
        // -gracefully after a bounded number of attempts rather than looping forever
        if(!Resize())
            break;
    }

    return false;
}

template <typename K, typename V> bool HashShard<K, V>::Insert(const K& key, const V& value)
{
    if(!entries_) {
        GetLogger().Error("[HashShard]: Insert() called before Init()");
        return false;
    }

    Entry newEntry{key, value, 0, true};

    for(std::size_t growthAttempt = 0; growthAttempt < MAX_GROWTH_RETRIES; ++growthAttempt) {
        if(static_cast<float>(size_) / capacity_ >= KLOAD_FACTOR_GROW)
            (void)Resize();

        const std::size_t mask = capacity_ - 1;
        const std::size_t hash = WFXHash(key);
        const std::size_t idx = hash & mask;
        std::size_t probe = 0;

        while(probe < MAX_PROBE_LIMIT) {
            const std::size_t pos = (idx + probe) & mask;
            Entry& entry = entries_[pos];

            PREFETCH_T3(reinterpret_cast<const char*>(&entries_[(pos + 1) & mask]));

            if(!entry.occupied) {
                newEntry.probeLength = static_cast<std::uint8_t>(probe);
                entry = std::move(newEntry);
                size_++;
                return true;
            }

            if(KeysEqual(entry.key, key)) {
                entry.value = value;
                return true;
            }

            if(entry.probeLength < probe) {
                std::swap(entry, newEntry);
                newEntry.probeLength = static_cast<std::uint8_t>(probe);
                probe = entry.probeLength;
            }

            ++probe;
        }

        // See Emplace() for why this can't just return false
        if(!Resize())
            break;
    }

    return false;
}

template <typename K, typename V> V* HashShard<K, V>::Get(const K& key) const
{
    if(!entries_)
        return nullptr;

    const std::size_t mask = capacity_ - 1;
    const std::size_t hash = WFXHash(key);
    const std::size_t idx = hash & mask;

    for(std::size_t i = 0; i < capacity_; ++i) {
        const std::size_t pos = (idx + i) & mask;
        Entry& entry = entries_[pos];

        if(!entry.occupied)
            return nullptr;

        if(KeysEqual(entry.key, key))
            return &entry.value;

        if(entry.probeLength < i)
            return nullptr;
    }

    return nullptr;
}

template <typename K, typename V> V* HashShard<K, V>::GetOrInsert(const K& inputKey, const V& defaultValue)
{
    if(!entries_) {
        GetLogger().Error("[HashShard]: GetOrInsert() called before Init()");
        return nullptr;
    }

    Entry newEntry{inputKey, defaultValue, 0, true};

    for(std::size_t growthAttempt = 0; growthAttempt < MAX_GROWTH_RETRIES; ++growthAttempt) {
        if(static_cast<float>(size_) / capacity_ >= KLOAD_FACTOR_GROW)
            (void)Resize();

        const std::size_t mask = capacity_ - 1;
        const std::size_t hash = WFXHash(inputKey);
        const std::size_t idx = hash & mask;
        std::size_t probe = 0;

        while(probe < MAX_PROBE_LIMIT) {
            const std::size_t pos = (idx + probe) & mask;
            Entry& entry = entries_[pos];

            PREFETCH_T3(reinterpret_cast<const char*>(&entries_[(pos + 1) & mask]));

            if(!entry.occupied) {
                newEntry.probeLength = static_cast<std::uint8_t>(probe);
                entry = std::move(newEntry);

                size_++;
                return &entry.value;
            }

            if(KeysEqual(entry.key, newEntry.key))
                return &entry.value;

            if(entry.probeLength < probe) {
                std::swap(entry, newEntry);
                newEntry.probeLength = static_cast<std::uint8_t>(probe);
                probe = entry.probeLength;
            }

            ++probe;
        }

        // See Emplace() for why this can't just return nullptr
        if(!Resize())
            break;
    }

    return nullptr;
}

template <typename K, typename V> bool HashShard<K, V>::Erase(const K& key)
{
    if(!entries_)
        return false;

    const std::size_t mask = capacity_ - 1;
    const std::size_t hash = WFXHash(key);
    const std::size_t idx = hash & mask;

    for(std::size_t i = 0; i < capacity_; ++i) {
        const std::size_t pos = (idx + i) & mask;
        const Entry& entry = entries_[pos];

        if(!entry.occupied && entry.probeLength == 0)
            return false;

        if(entry.occupied && KeysEqual(entry.key, key)) {
            BackwardShiftErase(pos);
            return true;
        }

        if(entry.probeLength < i)
            return false;
    }
    return false;
}

} // namespace WFX::Utils

#endif // WFX_UTILS_HASH_SHARD_IPP
