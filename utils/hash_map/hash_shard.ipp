#ifndef WFX_UTILS_HASH_SHARD_IPP
#define WFX_UTILS_HASH_SHARD_IPP

#include "utils/logger/logger.hpp"
#include <cstring>

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

template <typename K, typename V>
HashShard<K, V>::HashShard(BufferPool& pool) : pool_(pool) {}

template <typename K, typename V>
HashShard<K, V>::~HashShard()
{
    if(entries_)
        pool_.Release(entries_);
}

template <typename K, typename V>
void HashShard<K, V>::Init(std::size_t cap)
{
    initialBucketCapacity_ = cap;
    capacity_              = cap;
    entries_               = reinterpret_cast<Entry*>(pool_.Lease(cap * sizeof(Entry)));
    
    if(!entries_)
        Logger::GetInstance().Fatal("[HashShard]: Failed to get memory for entries");

    for(std::size_t i = 0; i < cap; ++i)
        new (&entries_[i]) Entry{};
}

template <typename K, typename V>
inline bool HashShard<K, V>::KeysEqual(const K& a, const K& b) const
{
    if constexpr(std::is_trivially_copyable_v<K> && (sizeof(K) == 8 || sizeof(K) == 4))
        return std::memcmp(&a, &b, sizeof(K)) == 0;
    else
        return a == b;
}

template <typename K, typename V>
void HashShard<K, V>::Resize(std::size_t newCapacity)
{
    if(newCapacity == 0) newCapacity = capacity_ * 2;
    if(newCapacity < initialBucketCapacity_) return;

    // Just to keep everything in check
    newCapacity = Math::RoundUpToPowerOfTwo(newCapacity);

    Entry* newEntries = reinterpret_cast<Entry*>(pool_.Lease(newCapacity * sizeof(Entry)));
    if(!newEntries)
        Logger::GetInstance().Fatal("[HashShard]: Failed to resize entries");
    
    for(std::size_t i = 0; i < newCapacity; ++i)
        new (&newEntries[i]) Entry{};

    for(std::size_t i = 0; i < capacity_; ++i) {
        Entry& currentEntry = entries_[i];
        if(!currentEntry.occupied)
            continue;

        std::size_t hash  = WFXHash(currentEntry.key);
        std::size_t idx   = hash & (newCapacity - 1);
        std::size_t probe = 0;

        while(probe < MAX_PROBE_LIMIT) {
            std::size_t pos    = (idx + probe) & (newCapacity - 1);
            Entry&      target = newEntries[pos];
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

    pool_.Release(entries_);
    entries_  = newEntries;
    capacity_ = newCapacity;
}

template <typename K, typename V>
bool HashShard<K, V>::BackwardShiftErase(std::size_t pos)
{
    std::size_t mask = capacity_ - 1;
    std::size_t j    = pos;
    std::size_t next = (j + 1) & mask;

    // Deletion by backward shifting of values
    while(entries_[next].occupied && entries_[next].probeLength > 0) {
        entries_[j] = std::move(entries_[next]);

        entries_[next].occupied    = false;
        entries_[next].probeLength = 0;
        
        entries_[j].probeLength--;
        
        j    = next;
        next = (j + 1) & mask;
    }

    // Explicitly destroy old key/value
    entries_[j].key.~K();
    entries_[j].value.~V();

    // Reconstruct in-place (default-constructed)
    new (&entries_[j].key) K{};
    new (&entries_[j].value) V{};

    entries_[j].occupied    = false;
    entries_[j].probeLength = 0;

    // True if it needs to be down sized, false if it doesn't
    --size_;
    float currentLoad = static_cast<float>(size_) / capacity_;
    return (currentLoad < KLOAD_FACTOR_SHRINK) && (capacity_ > initialBucketCapacity_);
}

template <typename K, typename V>
bool HashShard<K, V>::Emplace(const K& key, V&& value)
{
    if(static_cast<float>(size_) / capacity_ >= KLOAD_FACTOR_GROW)
        Resize();

    std::size_t mask  = capacity_ - 1;
    std::size_t hash  = WFXHash(key);
    std::size_t idx   = hash & mask;
    std::size_t probe = 0;

    Entry newEntry{key, std::move(value), 0, true};

    while(probe < MAX_PROBE_LIMIT) {
        std::size_t pos = (idx + probe) & mask;
        Entry& entry    = entries_[pos];

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

    return false;
}

template <typename K, typename V>
bool HashShard<K, V>::Insert(const K& key, const V& value)
{
    if(static_cast<float>(size_) / capacity_ >= KLOAD_FACTOR_GROW)
        Resize();

    std::size_t mask  = capacity_ - 1;
    std::size_t hash  = WFXHash(key);
    std::size_t idx   = hash & mask;
    std::size_t probe = 0;

    Entry newEntry{key, value, 0, true};

    while(probe < MAX_PROBE_LIMIT) {
        std::size_t pos   = (idx + probe) & mask;
        Entry&      entry = entries_[pos];

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

    return false;
}

template <typename K, typename V>
V* HashShard<K, V>::Get(const K& key) const
{
    std::size_t mask = capacity_ - 1;
    std::size_t hash = WFXHash(key);
    std::size_t idx  = hash & mask;

    for(std::size_t i = 0; i < capacity_; ++i) {
        std::size_t pos  = (idx + i) & mask;
        Entry&      entry = entries_[pos];

        if(!entry.occupied)
            return nullptr;

        if(KeysEqual(entry.key, key))
            return &entry.value;

        if(entry.probeLength < i)
            return nullptr;
    }

    return nullptr;
}

template <typename K, typename V>
V* HashShard<K, V>::GetOrInsert(const K& inputKey, const V& defaultValue)
{
    if(static_cast<float>(size_) / capacity_ >= KLOAD_FACTOR_GROW)
        Resize();

    Entry newEntry{inputKey, std::move(defaultValue), 0, true};

    std::size_t mask  = capacity_ - 1;
    std::size_t hash  = WFXHash(inputKey);
    std::size_t idx   = hash & mask;
    std::size_t probe = 0;

    while(probe < MAX_PROBE_LIMIT) {
        std::size_t pos   = (idx + probe) & mask;
        Entry&      entry = entries_[pos];

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

    return nullptr;
}

template <typename K, typename V>
bool HashShard<K, V>::Erase(const K& key)
{
    std::size_t mask = capacity_ - 1;
    std::size_t hash = WFXHash(key);
    std::size_t idx  = hash & mask;

    for(std::size_t i = 0; i < capacity_; ++i) {
        std::size_t pos   = (idx + i) & mask;
        Entry&      entry = entries_[pos];
        
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

// Looping
template<typename K, typename V>
template<typename Fn>
void HashShard<K, V>::ForEach(Fn&& cb) const
{
    if(!entries_ || capacity_ <= 0 || size_ == 0)
        return;

    std::size_t i = 0;

    while(i < capacity_) {
        Entry& entry = entries_[i];

        if(entry.occupied)
            cb(entry.key, entry.value);

        ++i;
    }
}

template<typename K, typename V>
template<typename Fn>
void HashShard<K, V>::ForEachEraseIf(Fn&& cb)
{
    if(!entries_ || capacity_ <= 0 || size_ == 0)
        return;

    std::size_t i = 0;

    // I tried using _mm_prefetch thinking it would increase performance
    // It didn't :(, anyways so it works perfectly fine without any such instruction
    // Cool
    while(i < capacity_) {
        Entry& entry = entries_[i];

        if(!entry.occupied) {
            ++i;
            continue;
        }

        // Right now i'm doing a naive erase, in future this can be changed
        // Hopefully this gets changed lmao
        if(cb(entry.key, entry.value)) {
            BackwardShiftErase(i);
            continue;
        }

        ++i;
    }
}

template <typename K, typename V>
std::unique_lock<std::shared_mutex> HashShard<K, V>::UniqueLock() const
{
    return std::unique_lock<std::shared_mutex>(mutex_);
}

template <typename K, typename V>
std::shared_lock<std::shared_mutex> HashShard<K, V>::SharedLock() const
{
    return std::shared_lock<std::shared_mutex>(mutex_);
}

template <typename K, typename V>
std::shared_mutex& HashShard<K, V>::Mutex() const
{
    return mutex_;
}

} // namespace WFX::Utils

#endif // WFX_UTILS_HASH_SHARD_IPP