#ifndef WFX_UTILS_HASHERS_HPP
#define WFX_UTILS_HASHERS_HPP

#include <cstdint>
#include <string_view>
#include <array>

namespace WFX::Utils {

// vvv HASHERS vvv
namespace Hasher {
std::uint64_t SipHash24(const std::uint8_t* data, std::uint64_t len, const std::uint8_t key[16]) noexcept;
std::uint64_t SipHash24(std::string_view data, const std::uint8_t key[16]) noexcept;
} // namespace Hasher

// vvv RANDOM BYTES GENERATOR vvv
class RandomPool final {
    static constexpr std::size_t BUFFER_SIZE = 1024 * 1024; // Stores 1MB worth of random bytes
    static constexpr std::size_t SSL_KEY_SIZE = 80;         // In bytes

    using SSLKey = std::array<std::uint8_t, SSL_KEY_SIZE>;

public:
    RandomPool();

public:
    bool GenerateSSLKey(); // Call once in master before fork

    SSLKey& GetSSLKey();
    bool GetBytes(std::uint8_t* out, std::size_t len);

private:
    bool RefillBytes();

private:
    // Core
    std::uint8_t randomPool_[BUFFER_SIZE];
    std::size_t cursor_{0};

    // SSL
    SSLKey sslKey_ = {};
};

// Free function declaration (defined in 'hash.cpp')
RandomPool& GetRandomPool() noexcept;

} // namespace WFX::Utils

#endif // WFX_UTILS_HASHERS_HPP