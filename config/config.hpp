#ifndef WFX_CONFIG_HPP
#define WFX_CONFIG_HPP

#include <cstdint>
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

    std::uint32_t maxHeaderTotalSize    = 8 * 1024;
    std::uint32_t maxBodyTotalSize      = 8 * 1024;
    std::uint16_t maxHeaderTotalCount   = 64;
    std::uint16_t headerReserveHintSize = 512;

    std::uint16_t headerTimeout = 15;
    std::uint16_t bodyTimeout   = 20;
    std::uint16_t idleTimeout   = 60;

    std::uint32_t maxConnections      = 2000;
    std::uint32_t maxConnectionsPerIp = 20;
    std::uint32_t maxRequestBurstSize = 10;
    std::uint32_t maxTokensPerSecond  = 5;
};

struct ENVConfig {
    std::string envPath;
};

struct SSLConfig {
    std::string certPath;
    std::string keyPath;

    std::string tls13Ciphers = 
        "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256";

    std::string tls12Ciphers = 
        "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
        "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384";

    std::string curves = "X25519:P-256"; // Elliptic curves preference

    bool         enableSessionCache = true;
    bool         enableKTLS         = false;
    std::uint8_t minProtoVersion    = 2;        // 1-> TLSv1.1; 2-> TLSv1.2; 3 -> TLSv1.3
    int          securityLevel      = 2;        // OpenSSL security level (0-5)
    std::size_t  sessionCacheSize   = 32 * 1024;
};

struct OSSpecificConfig {
#ifdef _WIN32
    std::uint32_t maxAcceptSlots      = 1024;
    std::uint16_t workerThreadCount   = 2;
    std::uint16_t callbackThreadCount = 4;
#else
    std::uint32_t workerProcesses = 4;
    std::uint32_t backlog         = 1024;
    
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

struct MiscConfig {
    std::uint16_t fileCacheSize     = 20;
    std::uint16_t cacheChunkSize    = 2 * 1024;
    std::uint32_t templateChunkSize = 16 * 1024;
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
    void LoadToolchainSettings(std::string_view path, bool isDebug);

public: // Main storage space for configurations
    ProjectConfig    projectConfig;
    NetworkConfig    networkConfig;
    ENVConfig        envConfig;
    SSLConfig        sslConfig;
    OSSpecificConfig osSpecificConfig;
    MiscConfig       miscConfig;
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