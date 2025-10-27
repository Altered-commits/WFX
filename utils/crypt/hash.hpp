#ifndef WFX_UTILS_HASHERS_HPP
#define WFX_UTILS_HASHERS_HPP

#include "./string.hpp"

#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>
#include <mutex>
#include <atomic>

// Helper macros
#define ROTL64(x, b) ((x << b) | (x >> (64 - b)))

namespace WFX::Utils {

// vvv HASH UTILS vvv
class HashUtils final {
public:
    static std::uint64_t Rotl(std::uint64_t n, unsigned int i) noexcept;
    static std::uint64_t Rotr(std::uint64_t n, unsigned int i) noexcept;
    static std::uint64_t Distribute(std::uint64_t n)           noexcept;

private:
    HashUtils()  = delete;
    ~HashUtils() = delete;
};

// vvv HASHERS vvv
class Hasher final {
public:
    static std::uint64_t SipHash24(const std::uint8_t* data, std::uint64_t len, const std::uint8_t key[16]);
    static std::uint64_t SipHash24(std::string_view data, const std::uint8_t key[16]);

    static std::uint64_t Fnv1aCaseInsensitive(const std::uint8_t* data, std::uint64_t len);
    static std::uint64_t Fnv1aCaseInsensitive(std::string_view data);

private:
    Hasher()  = delete;
    ~Hasher() = delete;
};

// vvv TRUE RANDOMIZER vvv
class RandomPool final {
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
};

} // namespace WFX::Utils


#endif // WFX_UTILS_HASHERS_HPP