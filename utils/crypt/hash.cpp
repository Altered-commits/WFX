#include "hash.hpp"

#include "utils/logger/logger.hpp"
#include <cstring>
#include <limits>

// Some OS level tools for randomization
#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
    #include <bcrypt.h>
    #pragma comment(lib, "bcrypt.lib")
#else
    #include <fcntl.h>
    #include <unistd.h>
    #include <sys/random.h>
    #include <errno.h>
#endif

namespace WFX::Utils {

// vvv HASH UTILS vvv
std::uint64_t HashUtils::Rotl(std::uint64_t n, unsigned int i) noexcept
{
    constexpr std::size_t m = std::numeric_limits<std::size_t>::digits - 1;
    const std::size_t c = i & m;
    return (n << c) | (n >> ((std::size_t(0) - c) & m));
}

std::uint64_t HashUtils::Rotr(std::uint64_t n, unsigned int i) noexcept
{
    constexpr std::size_t m = std::numeric_limits<std::size_t>::digits - 1;
    const std::size_t c = i & m;
    return (n >> c) | (n << ((std::size_t(0) - c) & m));
}

std::uint64_t HashUtils::Distribute(std::uint64_t n) noexcept
{
    if constexpr(sizeof(std::size_t) == 4) {
        const std::uint32_t p = 0x55555555ul;         // Alternating bit pattern
        const std::uint32_t c = 3423571495ul;         // Odd constant for mixing
        std::uint32_t x = static_cast<std::uint32_t>(n);
        x ^= x >> 16;
        x *= p;
        x ^= x >> 16;
        return static_cast<std::size_t>(c * x);
    }
    else {
        const std::uint64_t p = 0x5555555555555555ull;
        const std::uint64_t c = 17316035218449499591ull;
        std::uint64_t x = static_cast<std::uint64_t>(n);
        x ^= x >> 32;
        x *= p;
        x ^= x >> 32;
        return static_cast<std::size_t>(c * x);
    }
}

// vvv HASHERS vvv
std::uint64_t Hasher::SipHash24(const std::uint8_t* data, std::uint64_t len, const std::uint8_t key[16])
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
            v0 += v1; v1 = HashUtils::Rotl(v1, 13); v1 ^= v0; v0 = HashUtils::Rotl(v0, 32);
            v2 += v3; v3 = HashUtils::Rotl(v3, 16); v3 ^= v2;
            v0 += v3; v3 = HashUtils::Rotl(v3, 21); v3 ^= v0;
            v2 += v1; v1 = HashUtils::Rotl(v1, 17); v1 ^= v2; v2 = HashUtils::Rotl(v2, 32);
        }
        v0 ^= m;
    }

    std::uint64_t last = static_cast<std::uint64_t>(len) << 56;
    std::uint64_t rem = len & 7;
    for(std::uint64_t i = 0; i < rem; ++i)
        last |= static_cast<std::uint64_t>(ptr[i]) << (i * 8);

    v3 ^= last;
    for(int i = 0; i < 2; ++i) {
        v0 += v1; v1 = HashUtils::Rotl(v1, 13); v1 ^= v0; v0 = HashUtils::Rotl(v0, 32);
        v2 += v3; v3 = HashUtils::Rotl(v3, 16); v3 ^= v2;
        v0 += v3; v3 = HashUtils::Rotl(v3, 21); v3 ^= v0;
        v2 += v1; v1 = HashUtils::Rotl(v1, 17); v1 ^= v2; v2 = HashUtils::Rotl(v2, 32);
    }
    v0 ^= last;

    v2 ^= 0xff;
    for(int i = 0; i < 4; ++i) {
        v0 += v1; v1 = HashUtils::Rotl(v1, 13); v1 ^= v0; v0 = HashUtils::Rotl(v0, 32);
        v2 += v3; v3 = HashUtils::Rotl(v3, 16); v3 ^= v2;
        v0 += v3; v3 = HashUtils::Rotl(v3, 21); v3 ^= v0;
        v2 += v1; v1 = HashUtils::Rotl(v1, 17); v1 ^= v2; v2 = HashUtils::Rotl(v2, 32);
    }

    return v0 ^ v1 ^ v2 ^ v3;
}

std::uint64_t Hasher::SipHash24(std::string_view str, const std::uint8_t key[16])
{
    return SipHash24(reinterpret_cast<const std::uint8_t*>(str.data()), str.size(), key);
}

std::uint64_t Hasher::Fnv1aCaseInsensitive(const std::uint8_t* data, std::uint64_t len)
{
    constexpr std::uint64_t fnvPrime       = 1099511628211ULL;
    constexpr std::uint64_t fnvOffsetBasis = 14695981039346656037ULL;

    std::uint64_t hash = fnvOffsetBasis;

    const std::uint8_t* end = data + len;
    while(data < end) {
        hash ^= static_cast<std::uint8_t>(StringSanitizer::ToLowerAscii(static_cast<unsigned char>(*data++)));
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

    // If not enough space in pool, refill
    if(cursor_ + len > BUFFER_SIZE) {
        if(!RefillBytes())
            return false;

        // If still can't fit, fail (should never happen)
        if(len > BUFFER_SIZE)
            return false;
    }

    std::memcpy(out, randomPool_ + cursor_, len);
    cursor_ += len;
    return true;
}

// Main shit
bool RandomPool::RefillBytes()
{
#if defined(_WIN32)
    if(BCryptGenRandom(nullptr, randomPool_, static_cast<ULONG>(BUFFER_SIZE), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return false;
#else
    ssize_t totalRead = 0;

    while(totalRead < BUFFER_SIZE) {
        ssize_t n = getrandom(randomPool_ + totalRead, BUFFER_SIZE - totalRead, 0);
        if(n < 0) {
            if(errno == ENOSYS) {
                // Fallback to /dev/urandom
                int fd = open("/dev/urandom", O_RDONLY);
                if(fd < 0)
                    return false;

                ssize_t r;
                ssize_t readTotal = 0;
                while(readTotal < BUFFER_SIZE) {
                    r = read(fd, randomPool_ + readTotal, BUFFER_SIZE - readTotal);
                    if(r <= 0) {
                        close(fd);
                        return false;
                    }
                    readTotal += r;
                }
                close(fd);
                break;
            }
            // Interrupted syscall
            else if(errno == EINTR)
                continue;
            
            else
                return false;
        }
        else
            totalRead += n;
    }
#endif
    cursor_ = 0;
    return true;
}

} // WFX::Utils