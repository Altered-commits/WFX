// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_UTILS_HASH_SHARD_HPP
#define WFX_UTILS_HASH_SHARD_HPP

#include <cstdint>
#include <bit>
#include <functional>
#include <type_traits>

namespace WFX::Utils {

template <typename T> inline std::size_t WFXHash(const T& key)
{
    // std::hash<T> is identity for integral T on libstdc++, so mix it ourselves rather than
    // let sequential/attacker-chosen keys cluster under probing
    std::size_t h = std::hash<T>{}(key);
    h *= 0x9E3779B97F4A7C15ULL;
    h ^= h >> 32;
    return h;
}

// SwissTable-style open-addressing hashmap. A dense control-byte array (one byte per slot,
// scanned 8 at a time via SWAR bit tricks) lives separate from the {key, value} entries, for
// cache-dense probing. Growth is purely load-factor-driven: quadratic probing over 8-slot
// groups is proven to visit every group exactly once, so a probe walk always terminates on its
// own, with no fixed probe-length cap and no possibility of a surprise emergency resize
// mid-insert. Erase marks a tombstone, O(1), never shifts other entries. Slots are never
// default-constructed, so K/V only need to be copy/move constructible.
template <typename K, typename V> class HashShard {
    static_assert(std::is_copy_constructible_v<K> && std::is_move_constructible_v<K> && std::is_move_assignable_v<K>,
                  "HashShard<K, V> requires K to be copy-constructible and movable");
    static_assert(std::is_copy_constructible_v<V> && std::is_move_constructible_v<V> && std::is_move_assignable_v<V>,
                  "HashShard<K, V> requires V to be copy-constructible and movable");

    // The h2 tag math below shifts a 64-bit hash by 57, so this assumes a genuine 64-bit hash
    static_assert(sizeof(std::size_t) == 8, "HashShard assumes a 64-bit std::size_t");

    // Control bytes are scanned 8 at a time as one uint64_t via SWAR tricks (see hash_shard.ipp)
    static constexpr std::size_t GROUP_WIDTH = 8;

    // SwissTable's own figure: 7/8 load factor before growing. Safe here because probing has
    // no fixed cap to blow past, unlike the robin-hood design this replaced
    static constexpr float KLOAD_FACTOR_GROW = 0.875f;

    // Deliberately lower than KLOAD_FACTOR_GROW: CapacityForCount pre-sizes for a known live-
    // entry ceiling, and sizing right at the reactive trigger's edge would mean routine
    // tombstone churn at that ceiling could still bump into a resize. This leaves real headroom
    static constexpr float KCOMFORTABLE_LOAD_FACTOR = 0.7f;

    // Control byte states. EMPTY/DELETED are chosen so bit 7 alone distinguishes "not full"
    // from "full", and bit 6 alone further distinguishes EMPTY from DELETED
    static constexpr std::uint8_t TAG_EMPTY = 0xFF;
    static constexpr std::uint8_t TAG_DELETED = 0x80;

public:
    HashShard() = default;
    ~HashShard();

    // Owns raw, manually-lifetime-managed 'entries_'/'meta_' buffers. A shallow (compiler-
    // generated) copy would double-free them, so copying is disabled outright rather than
    // left as a footgun.
    HashShard(const HashShard&) = delete;
    HashShard& operator=(const HashShard&) = delete;
    HashShard(HashShard&& other) noexcept;
    HashShard& operator=(HashShard&& other) noexcept;

public: // Main Functions
    void Init(std::size_t cap, bool forLiveCount = false);
    bool Emplace(const K& key, V&& value);
    bool Insert(const K& key, const V& value);
    bool Erase(const K& key);
    [[nodiscard]] V* Get(const K& key) const;
    [[nodiscard]] V* GetOrInsert(const K& key, const V& defaultValue);

private:
    struct Entry {
        K key;
        V value;
    };

    // Result of a probe walk: either an existing match ('found'), or (if absent) the first
    // empty-or-deleted slot seen along the way, ready to receive a new entry at 'pos'
    struct SlotResult {
        std::size_t pos;
        std::uint8_t h2;
        bool found;
    };

    // First empty-or-deleted slot for a key known not to collide with anything else in the
    // table (used only during in-place compaction, where every key is already unique)
    struct AvailSlot {
        std::size_t pos;
        std::uint8_t h2;
    };

private: // Helper Functions
    static bool KeysEqual(const K& a, const K& b);

    V* InsertOrGet(const K& key, V&& value, bool overwriteExisting);

    // The core probe walk, shared by Get/Erase/InsertOrGet/Resize's rehash. Static so Resize
    // can run it against a not-yet-installed buffer while rehashing
    static SlotResult FindSlot(const Entry* ents, const std::uint8_t* meta, std::size_t capacity, const K& key);

    // Same probe walk as FindSlot but skips the key-match scan entirely: only used by
    // CompactInPlace, where every key in the table is already known to be unique
    static AvailSlot FindAvailableSlot(const std::uint8_t* meta, std::size_t capacity, const K& key);

    [[nodiscard]] bool Resize(std::size_t newCapacity = 0);

    // Purges every tombstone without a second allocation: converts the table to FULL->DELETED,
    // DELETED->EMPTY, then cycle-follows each not-yet-settled entry into its correct slot
    void CompactInPlace();

    // Only full slots are live objects (per 'meta_'); empty/deleted ones are raw, uninitialized
    // storage and must not be destroyed
    static void DestroySlots(Entry* slots, const std::uint8_t* meta, std::size_t count) noexcept;

    // Overflow-safe 'count * elemSize'
    static std::size_t SafeByteSize(std::size_t count, std::size_t elemSize);

    static bool IsFull(std::uint8_t m) noexcept { return (m & 0x80) == 0; }

    // 'meta' is allocated with GROUP_WIDTH extra bytes past 'capacity', mirroring the first
    // GROUP_WIDTH bytes, so a group read starting anywhere in [0, capacity) can safely span
    // the wraparound with a single unaligned load. Every meta write goes through here to keep
    // that mirror in sync
    static void SetMetaByte(std::uint8_t* meta, std::size_t capacity, std::size_t pos, std::uint8_t tag) noexcept
    {
        meta[pos] = tag;
        if(pos < GROUP_WIDTH)
            meta[capacity + pos] = tag;
    }

    // SWAR group-matching helpers, operating on 8 control bytes packed into one uint64_t.
    // Each returns an 8-bit-per-lane bitmask (bit 7 of each byte lane set exactly where that
    // lane matched); std::countr_zero(mask) >> 3 decodes the matching lane, and
    // 'mask &= mask - 1' advances to the next one
    //
    // Classic "find a zero byte in a word" trick applied to 'word XOR broadcast(tag)', so a
    // byte matching 'tag' becomes zero. May rarely flag a non-matching byte too (only ever a
    // full byte, never EMPTY/DELETED), which is harmless since callers always verify the real
    // key afterward
    static std::uint64_t MatchByte(std::uint64_t group, std::uint8_t tag) noexcept
    {
        const std::uint64_t bcast = tag * 0x0101010101010101ULL;
        const std::uint64_t cmp = group ^ bcast;
        return (cmp - 0x0101010101010101ULL) & ~cmp & 0x8080808080808080ULL;
    }

    // EMPTY (0xFF) has bits 7 and 6 both set; DELETED (0x80) has bit 7 set but bit 6 clear; a
    // full byte (h2 tag) always has bit 7 clear. ANDing a byte with itself shifted left by 1
    // puts bit 6 into bit 7's place, so the result has bit 7 set only where both were already
    // set: EMPTY only
    static std::uint64_t MatchEmpty(std::uint64_t group) noexcept
    {
        return group & (group << 1) & 0x8080808080808080ULL;
    }

    // EMPTY or DELETED both have bit 7 set, a full byte never does
    static std::uint64_t MatchEmptyOrDeleted(std::uint64_t group) noexcept
    {
        return group & 0x8080808080808080ULL;
    }

private: // Storage
    Entry* entries_ = nullptr;
    std::uint8_t* meta_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t initialBucketCapacity_ = 0;
    std::size_t size_ = 0;

    // Erase() marks a tombstone instead of shifting, so this can overcount reclaimed slots
    // between resizes. Only used to decide when a compacting rehash is due, so that's fine
    std::size_t deletedCount_ = 0;
};

} // namespace WFX::Utils

#include "utils/hash_map/hash_shard.ipp"

#endif // WFX_UTILS_HASH_SHARD_HPP
