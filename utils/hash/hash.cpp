// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "hash.hpp"
#include "shared/utils/hash.hpp"
#include "utils/diagnostics/logger.hpp"
#include <cstring>
#include <bit>

// Some OS level tools for randomization
#include <fcntl.h>
#include <unistd.h>
#include <sys/random.h>
#include <errno.h>

namespace WFX::Utils {

using namespace WFX::Shared; // For 'Rotl', 'Rotr', etc.

// vvv HASHERS vvv
std::uint64_t Hasher::SipHash24(const std::uint8_t* data, std::uint64_t len, const std::uint8_t key[16]) noexcept
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
            v0 += v1;
            v1 = std::rotl(v1, 13);
            v1 ^= v0;
            v0 = std::rotl(v0, 32);
            v2 += v3;
            v3 = std::rotl(v3, 16);
            v3 ^= v2;
            v0 += v3;
            v3 = std::rotl(v3, 21);
            v3 ^= v0;
            v2 += v1;
            v1 = std::rotl(v1, 17);
            v1 ^= v2;
            v2 = std::rotl(v2, 32);
        }
        v0 ^= m;
    }

    std::uint64_t last = static_cast<std::uint64_t>(len) << 56;
    std::uint64_t rem = len & 7;
    for(std::uint64_t i = 0; i < rem; ++i)
        last |= static_cast<std::uint64_t>(ptr[i]) << (i * 8);

    v3 ^= last;
    for(int i = 0; i < 2; ++i) {
        v0 += v1;
        v1 = std::rotl(v1, 13);
        v1 ^= v0;
        v0 = std::rotl(v0, 32);
        v2 += v3;
        v3 = std::rotl(v3, 16);
        v3 ^= v2;
        v0 += v3;
        v3 = std::rotl(v3, 21);
        v3 ^= v0;
        v2 += v1;
        v1 = std::rotl(v1, 17);
        v1 ^= v2;
        v2 = std::rotl(v2, 32);
    }
    v0 ^= last;

    v2 ^= 0xff;
    for(int i = 0; i < 4; ++i) {
        v0 += v1;
        v1 = std::rotl(v1, 13);
        v1 ^= v0;
        v0 = std::rotl(v0, 32);
        v2 += v3;
        v3 = std::rotl(v3, 16);
        v3 ^= v2;
        v0 += v3;
        v3 = std::rotl(v3, 21);
        v3 ^= v0;
        v2 += v1;
        v1 = std::rotl(v1, 17);
        v1 ^= v2;
        v2 = std::rotl(v2, 32);
    }

    return v0 ^ v1 ^ v2 ^ v3;
}

std::uint64_t Hasher::SipHash24(std::string_view str, const std::uint8_t key[16]) noexcept
{
    return SipHash24(reinterpret_cast<const std::uint8_t*>(str.data()), str.size(), key);
}

// vvv RANDOM POOL vvv
// Global pool instance
static RandomPool __GlobalRandomPool;

RandomPool& GetRandomPool() noexcept
{
    return __GlobalRandomPool;
}

RandomPool::RandomPool()
{
    if(!RefillBytes())
        GetLogger().Fatal("[RandomPool]: Failed to construct randomized byte pool.");
}

// vvv Public Functions vvv
bool RandomPool::GenerateSSLKey()
{
    return GetBytes(sslKey_.data(), SSL_KEY_SIZE);
}

RandomPool::SSLKey& RandomPool::GetSSLKey()
{
    return sslKey_;
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
    cursor_ = 0;
    return true;
}

} // namespace WFX::Utils