// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_CONFIG_HPP
#define WFX_CONFIG_HPP

#include "shared/utils/detection_macro.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace WFX::Core {

// Every struct represents a section of configuration (except three below)
struct ProjectConfig {
    std::string projectName; // --
    std::string projectPath; //  | These will be set from within the master process
    std::string publicDir;   //  | Note: 'projectPath' is absolute path while others are relative paths
    std::string templateDir; // --

    std::vector<std::string> middlewareList;
};

struct BuildConfig {
    std::string buildDir = "build"; // Will be converted to dir in master process
    std::string buildGenerator = "Unix Makefiles";
    std::string buildType = "Debug";
};

struct NetworkConfig {
    std::uint32_t maxSendBufferSize = 16 * 1024;
    std::uint32_t maxReadBufferSize = 16 * 1024;
    std::uint32_t readBufferIncSize = 4 * 1024;
    std::uint32_t sendBufferIncSize = 4 * 1024;

    std::uint32_t maxHeaderTotalSize = 8 * 1024;
    std::uint32_t maxBodyTotalSize = 8 * 1024;
    std::uint16_t maxHeaderTotalCount = 64;
    std::uint16_t headerReserveHintSize = 512;

    std::uint16_t headerTimeout = 15;
    std::uint16_t bodyTimeout = 20;
    std::uint16_t idleTimeout = 60;

    std::uint32_t maxConnections = 2000;
    std::uint32_t maxConnectionsPerIp = 20;
    std::uint32_t maxRequestBurstSize = 10;
    std::uint32_t maxTokensPerSecond = 5;
};

struct ENVConfig {
    std::string envPath;
};

struct SSLConfig {
    std::string certPath;
    std::string keyPath;
    std::string caCertPath;

    std::string tls13Ciphers = "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256";

    std::string tls12Ciphers = "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
                               "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
                               "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384";

    std::string curves = "X25519:P-256"; // Elliptic curves preference

    bool enableServerSessionCache = true;
    bool enableClientSessionCache = true;
    bool enableKTLS = false;
    std::uint8_t minProtoVersion = 2; // 1-> TLSv1.1; 2-> TLSv1.2; 3 -> TLSv1.3
    int securityLevel = 2;            // OpenSSL security level (0-5)
    std::size_t serverSessionCacheSize = 4 * 1024;
    std::size_t clientSessionCacheSize = 1 * 1024;
};

struct OSSpecificConfig {
#ifdef WFX_PLATFORM_LINUX
    std::uint32_t workerProcesses = 4;
    std::uint32_t backlog = 1024;
    std::uint16_t workerShutdownTimeout = 5; // In seconds
    std::uint16_t maxEvents = 1 * 1024;
#else
#error "Unsupported platform - add a WFX_PLATFORM_<X> branch in config with its respective fields"
#endif
};

struct LoggingConfig {
    std::uint8_t minLevel = 2; // 0=trace 1=debug 2=info 3=warn 4=error 5=fatal
    bool enableStdout = true;
    bool enableColors = true;
    bool enableTimestamps = true;
    bool enableFile = false;
    std::uint16_t maxRotations = 2;
    std::uint32_t maxFileSize = 16 * 1024 * 1024;
};

struct MiscConfig {
    std::uint16_t fileCacheSize = 20;
    std::uint16_t cacheChunkSize = 2 * 1024;
    std::uint32_t templateChunkSize = 16 * 1024;
    std::uint16_t masterPollInterval = 2; // In seconds
    std::uint16_t maxWorkerRestarts = 5;
    std::uint16_t workerBackoffBase = 1; // In seconds
    std::uint16_t workerBackoffMax = 16; // In seconds
};

// Main Config loader
// TODO: Add checks for maxReadBufferSize >= maxHeaderTotalSize + maxBodyTotalSize
class Config final {
public: // Load from TOML
    void LoadCoreSettings(std::string_view path);
    void LoadFinalSettings(const std::string& projectDir);

public: // Main storage space for configurations
    ProjectConfig projectConfig;
    BuildConfig buildConfig;
    NetworkConfig networkConfig;
    ENVConfig envConfig;
    SSLConfig sslConfig;
    OSSpecificConfig osSpecificConfig;
    LoggingConfig loggingConfig;
    MiscConfig miscConfig;
};

// Free function declaration (defined in 'config.cpp')
Config& GetConfig() noexcept;

} // namespace WFX::Core

#endif // WFX_CONFIG_HPP