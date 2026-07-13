// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_UTILS_HASH_SHARD_HPP
#define WFX_UTILS_HASH_SHARD_HPP

#include <cstdint>
#include <bit>
#include <type_traits>

namespace WFX::Utils {

template <typename T> inline std::size_t WFXHash(const T& key)
{
    return std::hash<T>{}(key);
}

template <typename K, typename V> class HashShard {
    static_assert(std::is_default_constructible_v<K>, "HashShard<K, V> requires K to be default-constructible");
    static_assert(std::is_default_constructible_v<V>, "HashShard<K, V> requires V to be default-constructible");
    static_assert(std::is_move_constructible_v<V> && std::is_move_assignable_v<V>,
                  "HashShard<K, V> requires V to be movable (container values must move, not copy, on rehash)");

    // Robin-hood probe cap and load factor. Grow-only, never shrinks
    static constexpr std::size_t MAX_PROBE_LIMIT = 64;
    static constexpr float KLOAD_FACTOR_GROW = 0.7f;

    // How hard a hot-path insert retries growth before giving up gracefully
    static constexpr std::size_t MAX_RESIZE_BACKOFF_ATTEMPTS = 4;
    static constexpr std::size_t MAX_GROWTH_RETRIES = 8;

public:
    HashShard() = default;
    ~HashShard();

    // Owns a raw, manually-lifetime-managed 'entries_' buffer. A shallow (compiler-generated) copy-
    // -would double-free it, so copying is disabled outright rather than left as a footgun
    HashShard(const HashShard&) = delete;
    HashShard& operator=(const HashShard&) = delete;

    HashShard(HashShard&& other) noexcept;
    HashShard& operator=(HashShard&& other) noexcept;

public: // Main Functions
    // Initializing
    void Init(std::size_t cap);

    // Operations
    bool Emplace(const K& key, V&& value);
    bool Insert(const K& key, const V& value);
    [[nodiscard]] V* Get(const K& key) const;
    bool Erase(const K& key);
    [[nodiscard]] V* GetOrInsert(const K& key, const V& defaultValue = V{});

private: // Storage
    struct Entry {
        K key;
        V value;
        std::uint8_t probeLength;
        bool occupied;
    };

private: // Helper Functions
    bool KeysEqual(const K& a, const K& b) const;

    // Backs off to a smaller request a few times under memory pressure. False if every attempt failed
    [[nodiscard]] bool Resize(std::size_t newCapacity = 0);
    void BackwardShiftErase(std::size_t pos);

    // Every slot is a live, placement-constructed object, even unoccupied ones - must be-
    // -individually destroyed before the backing buffer is freed, or container V leaks
    static void DestroySlots(Entry* slots, std::size_t count) noexcept;

    // Overflow-safe 'count * sizeof(Entry)'
    static std::size_t SafeByteSize(std::size_t count);

private: // Storage
    Entry* entries_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t initialBucketCapacity_ = 0;
    std::size_t size_ = 0;
};

} // namespace WFX::Utils

#include "utils/hash_map/hash_shard.ipp"

#endif // WFX_UTILS_HASH_SHARD_HPP
