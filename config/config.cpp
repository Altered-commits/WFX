// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "config.hpp"
#include "config_helper.hpp"
#include "utils/fileops/filesystem.hpp"

namespace WFX::Core {

using namespace WFX::Utils;               // For 'Logger', 'Filesystem'
using namespace WFX::Core::ConfigHelpers; // I mean, its quite obvious

// Global configuration instance
static Config __GlobalConfig;

Config& GetConfig() noexcept
{
    return __GlobalConfig;
}

// vvv Public Functions vvv
void Config::LoadCoreSettings(std::string_view path)
{
    Logger& logger = GetLogger();

    try {
        auto tbl = toml::parse_file(path);

        // vvv Project vvv
        ExtractStringArrayOrFatal(tbl, "Project", "middleware_list", projectConfig.middlewareList);

        // vvv Build vvv
        ExtractValueOrFatal(tbl, "Build", "dir_name", buildConfig.buildDir);
        ExtractValueOrFatal(tbl, "Build", "preferred_config", buildConfig.buildType);
        ExtractValueOrFatal(tbl, "Build", "preferred_generator", buildConfig.buildGenerator);

        // vvv ENV vvv
        ExtractValueOrFatal(tbl, "ENV", "env_path", envConfig.envPath);

        // vvv SSL vvv
        ExtractValueOrFatal(tbl, "SSL", "cert_path", sslConfig.certPath);
        ExtractValueOrFatal(tbl, "SSL", "key_path", sslConfig.keyPath);
        ExtractValueOrFatal(tbl, "SSL", "ca_cert_path", sslConfig.caCertPath);

        ExtractValue(tbl, "SSL", "tls13_ciphers", sslConfig.tls13Ciphers);
        ExtractValue(tbl, "SSL", "tls12_ciphers", sslConfig.tls12Ciphers);
        ExtractValue(tbl, "SSL", "curves", sslConfig.curves);
        ExtractValue(tbl, "SSL", "enable_server_session_cache", sslConfig.enableServerSessionCache);
        ExtractValue(tbl, "SSL", "enable_client_session_cache", sslConfig.enableClientSessionCache);
        ExtractValue(tbl, "SSL", "enable_ktls", sslConfig.enableKTLS);
        ExtractValue(tbl, "SSL", "server_session_cache_size", sslConfig.serverSessionCacheSize);
        ExtractValue(tbl, "SSL", "client_session_cache_size", sslConfig.clientSessionCacheSize);
        ExtractValue(tbl, "SSL", "min_proto_version", sslConfig.minProtoVersion);
        ExtractValue(tbl, "SSL", "security_level", sslConfig.securityLevel);

        // vvv Network vvv
        ExtractValue(tbl, "Network", "send_buffer_max", networkConfig.maxSendBufferSize);
        ExtractValue(tbl, "Network", "recv_buffer_max", networkConfig.maxReadBufferSize);
        ExtractValue(tbl, "Network", "send_buffer_incr", networkConfig.sendBufferIncSize);
        ExtractValue(tbl, "Network", "recv_buffer_incr", networkConfig.readBufferIncSize);
        ExtractValue(tbl, "Network", "header_reserve_hint", networkConfig.headerReserveHintSize);
        ExtractValue(tbl, "Network", "max_header_size", networkConfig.maxHeaderTotalSize);
        ExtractValue(tbl, "Network", "max_body_size", networkConfig.maxBodyTotalSize);
        ExtractValue(tbl, "Network", "max_header_count", networkConfig.maxHeaderTotalCount);
        ExtractValue(tbl, "Network", "header_timeout", networkConfig.headerTimeout);
        ExtractValue(tbl, "Network", "body_timeout", networkConfig.bodyTimeout);
        ExtractValue(tbl, "Network", "idle_timeout", networkConfig.idleTimeout);
        ExtractValue(tbl, "Network", "max_connections", networkConfig.maxConnections);
        ExtractValue(tbl, "Network", "max_connections_per_ip", networkConfig.maxConnectionsPerIp);
        ExtractValue(tbl, "Network", "max_request_burst_per_ip", networkConfig.maxRequestBurstSize);
        ExtractValue(tbl, "Network", "max_requests_per_ip_per_sec", networkConfig.maxTokensPerSecond);

        // vvv OS Specific vvv
#if defined(WFX_PLATFORM_LINUX)
        ExtractValue(tbl, "Linux", "worker_processes", osSpecificConfig.workerProcesses);
        ExtractValue(tbl, "Linux", "worker_shutdown_timeout", osSpecificConfig.workerShutdownTimeout);
        ExtractValue(tbl, "Linux", "backlog", osSpecificConfig.backlog);
        ExtractValue(tbl, "Linux.Epoll", "max_events", osSpecificConfig.maxEvents);
#else
#error "Unsupported platform - add a WFX_PLATFORM_<X> branch here to load that platform's fields"
#endif

        // vvv Logging vvv
        ExtractValue(tbl, "Logging", "min_level", loggingConfig.minLevel);
        ExtractValue(tbl, "Logging", "enable_stdout", loggingConfig.enableStdout);
        ExtractValue(tbl, "Logging", "enable_colors", loggingConfig.enableColors);
        ExtractValue(tbl, "Logging", "enable_timestamps", loggingConfig.enableTimestamps);
        ExtractValue(tbl, "Logging", "enable_file", loggingConfig.enableFile);
        ExtractValue(tbl, "Logging", "max_file_size", loggingConfig.maxFileSize);
        ExtractValue(tbl, "Logging", "max_rotations", loggingConfig.maxRotations);

        // vvv Misc vvv
        ExtractValue(tbl, "Misc", "file_cache_size", miscConfig.fileCacheSize);
        ExtractValue(tbl, "Misc", "cache_chunk_size", miscConfig.cacheChunkSize);
        ExtractValue(tbl, "Misc", "template_chunk_size", miscConfig.templateChunkSize);
        ExtractValue(tbl, "Misc", "master_poll_interval", miscConfig.masterPollInterval);
        ExtractValue(tbl, "Misc", "max_worker_restarts", miscConfig.maxWorkerRestarts);
        ExtractValue(tbl, "Misc", "worker_backoff_base", miscConfig.workerBackoffBase);
        ExtractValue(tbl, "Misc", "worker_backoff_max", miscConfig.workerBackoffMax);
    }
    catch(const toml::parse_error& err) {
        logger.Fatal("[Config]: File -> 'wfx.toml', Error -> ", err.what());
    }
}

void Config::LoadFinalSettings(const std::string& projectDir)
{
    std::string cwd = FileSystem::GetCurrentPath();
    if(cwd.empty())
        GetLogger().Fatal("[Config]: Failed to resolve current working directory");

    // Its our job to set some of the project configuration (as we have that info)
    projectConfig.projectPath = cwd + "/" + projectDir;
    projectConfig.projectName = projectDir;
    projectConfig.publicDir = projectDir + "/public";
    projectConfig.templateDir = projectDir + "/templates";

    // Right now projectConfig 'buildDir' is folder name only, make it actual dir
    buildConfig.buildDir = projectDir + '/' + buildConfig.buildDir;
}

} // namespace WFX::Core