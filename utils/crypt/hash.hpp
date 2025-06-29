#ifndef WFX_UTILS_HASHERS_HPP
#define WFX_UTILS_HASHERS_HPP

#include "utils/logger/logger.hpp"
#include "./string.hpp"

#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>
#include <mutex>
#include <atomic>

// Some OS level tools for randomization
#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
    #include <bcrypt.h>
    #pragma comment(lib, "bcrypt.lib")
#else
    #include <fcntl.h>
    #include <unistd.h>
#endif

// Helper macros
#define ROTL64(x, b) ((x << b) | (x >> (64 - b)))

namespace WFX::Utils {

// vvv HASHERS vvv
// SipHash
std::size_t SipHash24(const std::uint8_t* data, std::size_t len, const std::uint8_t key[16]);
std::size_t SipHash24(std::string_view data, const std::uint8_t key[16]);

// FNV-1A
std::size_t Fnv1aCaseInsensitive(const std::uint8_t* data, std::size_t len);
std::size_t Fnv1aCaseInsensitive(std::string_view data);

// vvv TRUE RANDOMIZER vvv
class RandomPool {
    static constexpr std::size_t BUFFER_SIZE = 1024 * 1024; // Stores 1MB worth of random bytes
    static constexpr std::size_t MAX_RETRIES = 32; // Retries for GetBytes function

public:
    static RandomPool& GetInstance();

    bool GetBytes(std::uint8_t* out, std::size_t len);
    
private:
    RandomPool();

    // No copying or moving
    RandomPool(const RandomPool&) = delete;
    RandomPool& operator=(const RandomPool&) = delete;
    RandomPool(RandomPool&&) = delete;
    RandomPool& operator=(RandomPool&&) = delete;

private:
    bool RefillBytes(); // Actual shit

private:
    alignas(64) std::uint8_t randomPool_[BUFFER_SIZE];
    std::atomic<std::size_t> cursor_{0};
    std::mutex refillMutex_;

    Logger& logger_ = Logger::GetInstance();
};

} // namespace WFX::Utils


#endif // WFX_UTILS_HASHERS_HPP