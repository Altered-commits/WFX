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
    std::uint32_t maxSendBufferSize  = 2 * 1024;
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

struct SSLConfig {
    std::string certPath;
    std::string keyPath;
};

struct OSSpecificConfig {
#ifdef _WIN32
    std::uint32_t maxAcceptSlots      = 1024;
    std::uint16_t workerThreadCount   = 2;
    std::uint16_t callbackThreadCount = 4;
#else
    std::uint32_t workerProcesses = 4;
    std::uint32_t backlog         = 1024;
    std::uint16_t fileCacheSize   = 20;
    
    #ifdef WFX_LINUX_USE_IO_URING
        std::uint16_t batchSize       = 64;
        std::uint16_t acceptSlots     = 64;
        std::uint32_t queueDepth      = 4096;
        std::uint32_t fileChunkSize   = 64 * 1024;
    #else
        std::uint16_t maxEvents       = 1 * 1024;
    #endif // WFX_LINUX_USE_IO_URING
#endif // _WIN32
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
public: // Access
    static Config& GetInstance();

public: // Load from TOML
    void LoadCoreSettings(std::string_view path);
    void LoadToolchainSettings(std::string_view path);

public: // Main storage space for configurations
    ProjectConfig    projectConfig;
    NetworkConfig    networkConfig;
    SSLConfig        sslConfig;
    OSSpecificConfig osSpecificConfig;
    ToolchainConfig  toolchainConfig;

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