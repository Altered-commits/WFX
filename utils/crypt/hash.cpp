#include "hash.hpp"
#include "utils/logger/logger.hpp"

namespace WFX::Utils {

// vvv HASHERS vvv
std::size_t Hasher::SipHash24(const std::uint8_t* data, std::size_t len, const std::uint8_t key[16])
{
    std::uint64_t k0, k1;
    memcpy(&k0, key, 8);
    memcpy(&k1, key + 8, 8);

    std::uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
    std::uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
    std::uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
    std::uint64_t v3 = 0x7465646279746573ULL ^ k1;

    const std::uint8_t* ptr = data;
    const std::uint8_t* end = data + (len & ~7ULL);

    while(ptr != end) {
        std::uint64_t m;
        memcpy(&m, ptr, 8);
        ptr += 8;

        v3 ^= m;
        for(int i = 0; i < 2; ++i) {
            v0 += v1; v1 = ROTL64(v1, 13); v1 ^= v0; v0 = ROTL64(v0, 32);
            v2 += v3; v3 = ROTL64(v3, 16); v3 ^= v2;
            v0 += v3; v3 = ROTL64(v3, 21); v3 ^= v0;
            v2 += v1; v1 = ROTL64(v1, 17); v1 ^= v2; v2 = ROTL64(v2, 32);
        }
        v0 ^= m;
    }

    std::uint64_t last = static_cast<std::uint64_t>(len) << 56;
    std::size_t rem = len & 7;
    for(std::size_t i = 0; i < rem; ++i)
        last |= static_cast<std::uint64_t>(ptr[i]) << (i * 8);

    v3 ^= last;
    for(int i = 0; i < 2; ++i) {
        v0 += v1; v1 = ROTL64(v1, 13); v1 ^= v0; v0 = ROTL64(v0, 32);
        v2 += v3; v3 = ROTL64(v3, 16); v3 ^= v2;
        v0 += v3; v3 = ROTL64(v3, 21); v3 ^= v0;
        v2 += v1; v1 = ROTL64(v1, 17); v1 ^= v2; v2 = ROTL64(v2, 32);
    }
    v0 ^= last;

    v2 ^= 0xff;
    for(int i = 0; i < 4; ++i) {
        v0 += v1; v1 = ROTL64(v1, 13); v1 ^= v0; v0 = ROTL64(v0, 32);
        v2 += v3; v3 = ROTL64(v3, 16); v3 ^= v2;
        v0 += v3; v3 = ROTL64(v3, 21); v3 ^= v0;
        v2 += v1; v1 = ROTL64(v1, 17); v1 ^= v2; v2 = ROTL64(v2, 32);
    }

    return v0 ^ v1 ^ v2 ^ v3;
}

std::size_t Hasher::SipHash24(std::string_view str, const std::uint8_t key[16])
{
    return SipHash24(reinterpret_cast<const std::uint8_t*>(str.data()), str.size(), key);
}

std::size_t Hasher::Fnv1aCaseInsensitive(const std::uint8_t* data, std::size_t len)
{
    constexpr std::uint64_t fnvPrime       = 1099511628211ULL;
    constexpr std::uint64_t fnvOffsetBasis = 14695981039346656037ULL;

    std::uint64_t hash = fnvOffsetBasis;

    const std::uint8_t* end = data + len;
    while(data < end) {
        hash ^= static_cast<std::uint8_t>(StringGuard::ToLowerAscii(static_cast<unsigned char>(*data++)));
        hash *= fnvPrime;
    }

    return hash;
}

std::uint64_t Hasher::Fnv1aCaseInsensitive(std::string_view str)
{
    return Fnv1aCaseInsensitive(reinterpret_cast<const std::uint8_t*>(str.data()), str.size());
}

// vvv TRUE RANDOMIZER vvv
RandomPool::RandomPool()
{
    if(!RefillBytes())
        Logger::GetInstance().Fatal("[RandomPool]: Failed to construct randomized byte pool.");
}

RandomPool& RandomPool::GetInstance()
{
    static RandomPool pool;
    return pool;
}

bool RandomPool::GetBytes(std::uint8_t* out, std::size_t len)
{
    if(!out || len == 0 || len > BUFFER_SIZE)
        return false;

    for(int attempt = 0; attempt < MAX_RETRIES; ++attempt)
    {
        std::size_t oldPos = cursor_.fetch_add(len, std::memory_order_acq_rel);
        if(oldPos + len <= BUFFER_SIZE)
        {
            memcpy(out, randomPool_ + oldPos, len);
            return true;
        }

        // Another thread might've refilled already, double-check
        if(cursor_.load(std::memory_order_acquire) >= BUFFER_SIZE)
        {
            if(!RefillBytes())
                return false;
        }
        
        // Let other threads move forward slightly, reduces lock contention
        std::this_thread::yield();
    }

    return false;
}

// Main shit
bool RandomPool::RefillBytes()
{
    std::lock_guard<std::mutex> lock(refillMutex_);
#if defined(_WIN32)
    if(BCryptGenRandom(nullptr, randomPool_, static_cast<ULONG>(BUFFER_SIZE), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return false;
#else
    ssize_t readBytes = 0;
#if defined(SYS_getrandom)
    readBytes = getrandom(randomPool_, BUFFER_SIZE, 0);
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if(fd < 0) return false;
    readBytes = read(fd, randomPool_, BUFFER_SIZE);
    close(fd);
#endif // defined(SYS_getrandom)
    if(readBytes != static_cast<ssize_t>(BUFFER_SIZE))
        return false;
#endif
    cursor_.store(0, std::memory_order_release);
    return true;
}

} // WFX::Utils