#ifndef WFX_CONFIG_HPP
#define WFX_CONFIG_HPP

#include <string>
#include <vector>

namespace WFX::Core {

// Every struct represents a section of configuration
struct ProjectConfig {
    std::string projectName;
    std::string publicDir;
    std::string templateDir;
    std::vector<std::string> middlewareList;
};

struct NetworkConfig {
    std::uint32_t maxRecvBufferSize  = 16 * 1024;
    std::uint32_t bufferIncrSize     = 4 * 1024;

    std::uint16_t headerTimeout = 15;
    std::uint16_t bodyTimeout   = 20;
    std::uint16_t idleTimeout   = 60;

    std::uint16_t headerReserveHintSize = 512;
    std::uint32_t maxHeaderTotalSize    = 8 * 1024;
    std::uint32_t maxHeaderTotalCount   = 64;
    std::uint32_t maxBodyTotalSize      = 8 * 1024;

    std::uint64_t maxConnections      = 10000;
    std::int32_t  maxConnectionsPerIp = 20;
    std::int16_t  maxRequestBurstSize = 10;
    std::int16_t  maxTokensPerSecond  = 5;
};

struct OSSpecificConfig {
#ifdef _WIN32
    std::uint32_t maxAcceptSlots      = 1024;
    std::uint16_t workerThreadCount   = 2;
    std::uint16_t callbackThreadCount = 4;
#else
    std::uint32_t workerConnections = 4 * 1024;
#endif
};

struct ToolchainConfig {
    std::string ccmd;
    std::string lcmd;
    std::string cargs;
    std::string largs;
    std::string objFlag;
    std::string dllFlag;
};

// Main Config loader
// TODO: Add checks for maxRecvBufferSize >= maxHeaderTotalSize + maxBodyTotalSize
class Config final {
public:
    ProjectConfig    projectConfig;
    NetworkConfig    networkConfig;
    OSSpecificConfig osSpecificConfig;
    ToolchainConfig  toolchainConfig;

    // vvv Access vvv
    static Config& GetInstance();

    // vvv Load from TOML vvv
    void LoadCoreSettings(std::string_view path);
    void LoadToolchainSettings(std::string_view path);

private:
    Config()  = default;
    ~Config() = default;

    Config(const Config&)            = delete;
    Config& operator=(const Config&) = delete;
    Config(Config&&)                 = delete;
    Config& operator=(Config&&)      = delete;
};

} // namespace WFX::Core

#endif  // WFX_CONFIG_HPP