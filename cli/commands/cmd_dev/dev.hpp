#ifndef WFX_CLI_COMMANDS_DEV_HPP
#define WFX_CLI_COMMANDS_DEV_HPP

#include <string>
#include <cstdint>

namespace WFX::CLI {
    
// Just to keep stuff tidy
enum class ServerFlags : std::uint64_t {
    NO_BUILD_CACHE      = 1ull << 0,
    NO_TEMPLATE_CACHE   = 1ull << 1,
    USE_HTTPS           = 1ull << 2,
    OVERRIDE_HTTPS_PORT = 1ull << 3,
    USE_DEBUG           = 1ull << 4,
};

struct ServerConfig {
    std::string   host  = "127.0.0.1";
    int           port  = 8080;
    std::uint64_t flags = 0;

    inline bool GetFlag(ServerFlags f) const noexcept
    {
        return (flags & static_cast<std::uint64_t>(f)) != 0;
    }

    inline void SetFlag(ServerFlags f, bool enable = true) noexcept
    {
        const auto bit = static_cast<std::uint64_t>(f);
        flags = enable ? (flags | bit) : (flags & ~bit);
    }
};

int RunDevServer(const ServerConfig& cfg);

}  // namespace WFX::CLI

#endif  // WFX_CLI_COMMANDS_DEV_HPP