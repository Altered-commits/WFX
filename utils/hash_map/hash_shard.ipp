// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_UTILS_HASH_SHARD_IPP
#define WFX_UTILS_HASH_SHARD_IPP

#include "utils/diagnostics/logger.hpp"
#include "shared/utils/memory.hpp"
#include "shared/utils/compiler_macro.hpp"
#include <cstring>
#include <limits>
#include <utility>

namespace WFX::Utils {

template <typename K, typename V>
void HashShard<K, V>::DestroySlots(Entry* slots, const std::uint8_t* meta, std::size_t count) noexcept
{
    for(std::size_t i = 0; i < count; ++i)
        if(IsFull(meta[i]))
            slots[i].~Entry();
}

template <typename K, typename V> std::size_t HashShard<K, V>::SafeByteSize(std::size_t count, std::size_t elemSize)
{
    const std::size_t maxCount = std::numeric_limits<std::size_t>::max() / elemSize;

    if(count > maxCount)
        GetLogger().Fatal("[HashShard]: Requested capacity of ", count, " entries overflows allocation size");

    return count * elemSize;
}

template <typename K, typename V> HashShard<K, V>::~HashShard()
{
    if(!entries_)
        return;

    DestroySlots(entries_, meta_, capacity_);
    Shared::Free(entries_);
    Shared::Free(meta_);
}

template <typename K, typename V>
HashShard<K, V>::HashShard(HashShard&& other) noexcept
    : entries_(other.entries_), meta_(other.meta_), capacity_(other.capacity_),
      initialBucketCapacity_(other.initialBucketCapacity_), size_(other.size_), deletedCount_(other.deletedCount_)
{
    other.entries_ = nullptr;
    other.meta_ = nullptr;
    other.capacity_ = 0;
    other.initialBucketCapacity_ = 0;
    other.size_ = 0;
    other.deletedCount_ = 0;
}

template <typename K, typename V> HashShard<K, V>& HashShard<K, V>::operator=(HashShard&& other) noexcept
{
    if(this == &other)
        return *this;

    if(entries_) {
        DestroySlots(entries_, meta_, capacity_);
        Shared::Free(entries_);
        Shared::Free(meta_);
    }

    entries_ = other.entries_;
    meta_ = other.meta_;
    capacity_ = other.capacity_;
    initialBucketCapacity_ = other.initialBucketCapacity_;
    size_ = other.size_;
    deletedCount_ = other.deletedCount_;

    other.entries_ = nullptr;
    other.meta_ = nullptr;
    other.capacity_ = 0;
    other.initialBucketCapacity_ = 0;
    other.size_ = 0;
    other.deletedCount_ = 0;

    return *this;
}

template <typename K, typename V> void HashShard<K, V>::Init(std::size_t cap, bool forLiveCount)
{
    // One-time init only
    if(entries_)
        GetLogger().Fatal("[HashShard]: Init() called twice");

    // 'forLiveCount' treats 'cap' as a live-entry ceiling rather than a raw slot count: a
    // caller who knows their real ceiling up front (e.g. a pool-bounded cache) can pre-size to
    // hold that many entries without ever resizing, regardless of whether 'cap' itself happens
    // to sit right on a power of 2 (which would otherwise leave zero load-factor headroom)
    if(forLiveCount)
        cap = static_cast<std::size_t>(static_cast<double>(cap) / KCOMFORTABLE_LOAD_FACTOR) + 1;

    // Always at least one full group, and a power of 2 so group-index masking works
    cap = std::bit_ceil(cap < GROUP_WIDTH ? GROUP_WIDTH : cap);

    initialBucketCapacity_ = cap;
    capacity_ = cap;

    entries_ = reinterpret_cast<Entry*>(Shared::Alloc(SafeByteSize(cap, sizeof(Entry))));
    if(!entries_)
        GetLogger().Fatal("[HashShard]: Failed to get memory for entries");

    // +GROUP_WIDTH: see SetMetaByte's comment. entries_ needs no such padding, it's only ever
    // read at exact slot indices, never via a group-wide unaligned load
    meta_ = reinterpret_cast<std::uint8_t*>(Shared::Alloc(SafeByteSize(cap + GROUP_WIDTH, sizeof(std::uint8_t))));
    if(!meta_) {
        Shared::Free(entries_);
        entries_ = nullptr;
        GetLogger().Fatal("[HashShard]: Failed to get memory for metadata");
    }

    // TAG_EMPTY in every byte, mirror included. Entries themselves are left uninitialized: a
    // slot only becomes a live object when something is actually placed into it
    std::memset(meta_, TAG_EMPTY, cap + GROUP_WIDTH);
}

template <typename K, typename V> inline bool HashShard<K, V>::KeysEqual(const K& a, const K& b)
{
    // 'has_unique_object_representations_v' (unlike a raw sizeof/is_trivially_copyable check)
    // also guarantees no padding bytes, so there's no risk of two logically-equal keys
    // comparing unequal because of uninitialized padding garbage
    if constexpr(std::has_unique_object_representations_v<K> && (sizeof(K) == 4 || sizeof(K) == 8))
        return std::memcmp(&a, &b, sizeof(K)) == 0;
    else
        return a == b;
}

template <typename K, typename V>
typename HashShard<K, V>::SlotResult HashShard<K, V>::FindSlot(const Entry* ents, const std::uint8_t* meta,
                                                               std::size_t capacity, const K& key)
{
    const std::size_t hash = WFXHash(key);
    const auto h2 = static_cast<std::uint8_t>(hash >> 57); // top 7 bits of the hash, bit 7 always 0
    const std::size_t groupMask = (capacity / GROUP_WIDTH) - 1;

    // Low bits pick the starting group, independent of h2's top-7-bit slice
    std::size_t groupIdx = hash & groupMask;
    std::size_t stride = 0;

    constexpr std::size_t NO_INSERT_POS = static_cast<std::size_t>(-1);
    std::size_t insertPos = NO_INSERT_POS;

    // Triangular-number probing over groups is guaranteed (power-of-2 group count) to visit
    // every group exactly once, so this loop bound is a hard upper limit, never a soft cap
    for(std::size_t groupsVisited = 0; groupsVisited <= groupMask; ++groupsVisited) {
        const std::size_t base = groupIdx * GROUP_WIDTH;

        std::uint64_t word;
        std::memcpy(&word, &meta[base], sizeof(word));

        for(std::uint64_t matches = MatchByte(word, h2); matches != 0; matches &= matches - 1) {
            const std::size_t pos = base + (static_cast<std::size_t>(std::countr_zero(matches)) >> 3);
            if(KeysEqual(ents[pos].key, key))
                return {pos, h2, true};
        }

        if(insertPos == NO_INSERT_POS) {
            if(const std::uint64_t avail = MatchEmptyOrDeleted(word); avail != 0)
                insertPos = base + (static_cast<std::size_t>(std::countr_zero(avail)) >> 3);
        }

        // A group with a genuinely empty slot proves the key can't be any further along the
        // probe sequence: insertion would already have stopped here or earlier
        if(MatchEmpty(word) != 0)
            return {insertPos, h2, false};

        ++stride;
        groupIdx = (groupIdx + stride) & groupMask;
    }

    // Every group visited with no empty slot anywhere: the table is completely full. The
    // load-factor discipline in InsertOrGet/Resize is supposed to make this impossible
    GetLogger().Fatal("[HashShard]: FindSlot exhausted every group, table is completely full");
    WFX_UNREACHABLE;
}

template <typename K, typename V>
typename HashShard<K, V>::AvailSlot HashShard<K, V>::FindAvailableSlot(const std::uint8_t* meta, std::size_t capacity,
                                                                       const K& key)
{
    const std::size_t hash = WFXHash(key);
    const auto h2 = static_cast<std::uint8_t>(hash >> 57);
    const std::size_t groupMask = (capacity / GROUP_WIDTH) - 1;

    std::size_t groupIdx = hash & groupMask;
    std::size_t stride = 0;

    for(std::size_t groupsVisited = 0; groupsVisited <= groupMask; ++groupsVisited) {
        const std::size_t base = groupIdx * GROUP_WIDTH;

        std::uint64_t word;
        std::memcpy(&word, &meta[base], sizeof(word));

        if(const std::uint64_t avail = MatchEmptyOrDeleted(word); avail != 0) {
            const std::size_t pos = base + (static_cast<std::size_t>(std::countr_zero(avail)) >> 3);
            return {pos, h2};
        }

        ++stride;
        groupIdx = (groupIdx + stride) & groupMask;
    }

    GetLogger().Fatal("[HashShard]: FindAvailableSlot exhausted every group during in-place compaction");
    WFX_UNREACHABLE;
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

    newCapacity = std::bit_ceil(newCapacity < GROUP_WIDTH ? GROUP_WIDTH : newCapacity);

    // Only ever called to grow bigger now (see InsertOrGet): purging tombstones without
    // growing goes through CompactInPlace instead, which needs no second allocation at all
    Entry* newEntries = reinterpret_cast<Entry*>(Shared::Alloc(SafeByteSize(newCapacity, sizeof(Entry))));
    if(!newEntries) {
        GetLogger().Error("[HashShard]: Resize allocation of ", newCapacity, " entries failed");
        return false;
    }

    auto* newMeta =
        reinterpret_cast<std::uint8_t*>(Shared::Alloc(SafeByteSize(newCapacity + GROUP_WIDTH, sizeof(std::uint8_t))));
    if(!newMeta) {
        Shared::Free(newEntries);
        GetLogger().Error("[HashShard]: Resize allocation of ", newCapacity, " metadata bytes failed");
        return false;
    }
    std::memset(newMeta, TAG_EMPTY, newCapacity + GROUP_WIDTH);

    // Group probing always finds room as long as the destination isn't already full, and it
    // never is here (sized for exactly the entries being moved in), so unlike the old
    // robin-hood placement this can't fail partway and need a bigger retry
    for(std::size_t i = 0; i < capacity_; ++i) {
        if(!IsFull(meta_[i]))
            continue;

        const auto slot = FindSlot(newEntries, newMeta, newCapacity, entries_[i].key);
        new (&newEntries[slot.pos]) Entry{std::move(entries_[i].key), std::move(entries_[i].value)};
        SetMetaByte(newMeta, newCapacity, slot.pos, slot.h2);
    }

    DestroySlots(entries_, meta_, capacity_);
    Shared::Free(entries_);
    Shared::Free(meta_);
    entries_ = newEntries;
    meta_ = newMeta;
    capacity_ = newCapacity;
    deletedCount_ = 0; // every tombstone was dropped by the rehash above

    return true;
}

template <typename K, typename V> void HashShard<K, V>::CompactInPlace()
{
    // Pass 1: FULL -> DELETED ("needs re-placement"), DELETED -> EMPTY (already reclaimed),
    // EMPTY stays EMPTY. From here on, a DELETED byte means "still holds an unsettled entry"
    for(std::size_t i = 0; i < capacity_; ++i) {
        if(IsFull(meta_[i]))
            SetMetaByte(meta_, capacity_, i, TAG_DELETED);
        else if(meta_[i] == TAG_DELETED)
            SetMetaByte(meta_, capacity_, i, TAG_EMPTY);
    }

    // Pass 2: cycle-follow every unsettled entry into its correct slot, all within this same
    // buffer. FindAvailableSlot only ever lands on a slot that's currently EMPTY (nothing left
    // to do but move in) or DELETED (still holds someone else's unsettled entry: swap and keep
    // resolving whatever just landed back in 'i', a standard in-place permutation cycle)
    for(std::size_t i = 0; i < capacity_; ++i) {
        while(meta_[i] == TAG_DELETED) {
            const auto slot = FindAvailableSlot(meta_, capacity_, entries_[i].key);

            if(slot.pos == i) {
                SetMetaByte(meta_, capacity_, i, slot.h2);
                break;
            }

            if(meta_[slot.pos] == TAG_EMPTY) {
                new (&entries_[slot.pos]) Entry{std::move(entries_[i].key), std::move(entries_[i].value)};
                entries_[i].~Entry();
                SetMetaByte(meta_, capacity_, slot.pos, slot.h2);
                SetMetaByte(meta_, capacity_, i, TAG_EMPTY);
                break;
            }

            std::swap(entries_[i], entries_[slot.pos]);
            SetMetaByte(meta_, capacity_, slot.pos, slot.h2);
        }
    }

    deletedCount_ = 0;
}

template <typename K, typename V> V* HashShard<K, V>::InsertOrGet(const K& key, V&& value, bool overwriteExisting)
{
    if(!entries_) {
        GetLogger().Error("[HashShard]: Insert called before Init()");
        return nullptr;
    }

    // Pre-emptive grow/compact past load factor, accounting for the slot this insert might
    // consume. If growth fails (e.g. transient allocation pressure), fall through and try the
    // walk on the current table anyway: there's still headroom below 100% full either way
    if(static_cast<float>(size_ + deletedCount_ + 1) / capacity_ > KLOAD_FACTOR_GROW) {
        // Live entries alone don't need more room, only tombstones do: compact in place
        // instead of growing, so insert/erase churn can't grow memory unboundedly
        const bool needsMoreRoom = static_cast<float>(size_ + 1) / capacity_ > KLOAD_FACTOR_GROW;
        if(needsMoreRoom) {
            if(!Resize(0))
                GetLogger().Warn("[HashShard]: Pre-emptive grow past load factor failed, continuing on current table");
        }
        else
            CompactInPlace();
    }

    const auto slot = FindSlot(entries_, meta_, capacity_, key);
    if(slot.found) {
        if(overwriteExisting)
            entries_[slot.pos].value = std::move(value);

        return &entries_[slot.pos].value;
    }

    if(meta_[slot.pos] == TAG_DELETED)
        --deletedCount_;

    new (&entries_[slot.pos]) Entry{key, std::move(value)};
    SetMetaByte(meta_, capacity_, slot.pos, slot.h2);
    ++size_;

    return &entries_[slot.pos].value;
}

template <typename K, typename V> bool HashShard<K, V>::Emplace(const K& key, V&& value)
{
    return InsertOrGet(key, std::move(value), true) != nullptr;
}

template <typename K, typename V> bool HashShard<K, V>::Insert(const K& key, const V& value)
{
    V copy = value;
    return InsertOrGet(key, std::move(copy), true) != nullptr;
}

template <typename K, typename V> bool HashShard<K, V>::Erase(const K& key)
{
    if(!entries_)
        return false;

    const auto slot = FindSlot(entries_, meta_, capacity_, key);
    if(!slot.found)
        return false;

    entries_[slot.pos].~Entry();

    // Decide EMPTY vs DELETED by checking one group's width on either side of the erased slot.
    // If there's a genuinely empty slot within reach in both directions, no other key's probe
    // walk could ever need to scan past this position anyway, so it's safe to reveal it as
    // EMPTY outright and hand the slot straight back to the load-factor budget. Otherwise it
    // must stay a tombstone, or some other key's early-termination check could go wrong
    const std::size_t mask = capacity_ - 1;
    const std::size_t indexBefore = (slot.pos + capacity_ - GROUP_WIDTH) & mask;

    std::uint64_t wordBefore, wordAfter;
    std::memcpy(&wordBefore, &meta_[indexBefore], sizeof(wordBefore));
    std::memcpy(&wordAfter, &meta_[slot.pos], sizeof(wordAfter));

    const auto beforeRun = static_cast<std::size_t>(std::countl_zero(MatchEmpty(wordBefore))) / 8;
    const auto afterRun = static_cast<std::size_t>(std::countr_zero(MatchEmpty(wordAfter))) / 8;

    if(beforeRun + afterRun >= GROUP_WIDTH) {
        SetMetaByte(meta_, capacity_, slot.pos, TAG_DELETED);
        ++deletedCount_;
    }
    else
        SetMetaByte(meta_, capacity_, slot.pos, TAG_EMPTY);

    --size_;

    return true;
}

template <typename K, typename V> V* HashShard<K, V>::Get(const K& key) const
{
    if(!entries_)
        return nullptr;

    const auto slot = FindSlot(entries_, meta_, capacity_, key);
    return slot.found ? &entries_[slot.pos].value : nullptr;
}

template <typename K, typename V> V* HashShard<K, V>::GetOrInsert(const K& key, const V& defaultValue)
{
    V copy = defaultValue;
    return InsertOrGet(key, std::move(copy), false);
}

} // namespace WFX::Utils

#endif // WFX_UTILS_HASH_SHARD_IPP
