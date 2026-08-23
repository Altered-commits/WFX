// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "config_helper.hpp"
#include "utils/fileops/filesystem.hpp"

namespace WFX::Core {

using namespace WFX::Utils;               // For 'Logger', 'Filesystem'
using namespace WFX::Core::ConfigHelpers; // I mean, its quite obvious

// Global configuration instance
static Config GlobalConfig;

Config& GetConfig() noexcept
{
    return GlobalConfig;
}

// vvv Public Functions vvv
void Config::LoadCoreSettings(std::string_view path)
{
    Logger& logger = GetLogger();

    try {
        auto tbl = toml::parse_file(path);

        // vvv Project vvv
        ExtractStringArray(tbl, "Project", "middleware_list", projectConfig.middlewareList, true);

        // vvv Build vvv
        ExtractValue(tbl, "Build", "dir_name", buildConfig.buildDir, true);
        ExtractValue(tbl, "Build", "preferred_config", buildConfig.buildType, true);
        ExtractValue(tbl, "Build", "preferred_generator", buildConfig.buildGenerator, true);

        // vvv ENV vvv
        ExtractValue(tbl, "ENV", "env_path", envConfig.envPath, true);

        // vvv IP vvv
        ExtractValue(tbl, "IP", "max_connections_per_ip", ipConfig.maxConnectionsPerIp);
        ExtractValue(tbl, "IP", "max_request_burst_per_ip", ipConfig.maxRequestBurstSize);
        ExtractValue(tbl, "IP", "max_requests_per_ip_per_sec", ipConfig.maxTokensPerSecond);
        ExtractValue(tbl, "IP", "max_tracked_identities", ipConfig.maxTrackedIdentities);
        ExtractValue(tbl, "IP", "real_ip_header", ipConfig.realIpHeader);
        ExtractValue(tbl, "IP", "real_ip_recursive", ipConfig.realIpRecursive);
        ExtractStringArray(tbl, "IP", "trusted_proxies", ipConfig.trustedProxies);

        // vvv CORS vvv
        ExtractCors(tbl, corsConfig);

        // vvv SSL vvv
        ExtractValue(tbl, "SSL", "cert_path", sslConfig.certPath, true);
        ExtractValue(tbl, "SSL", "key_path", sslConfig.keyPath, true);
        ExtractValue(tbl, "SSL", "outbound_ca_path", sslConfig.outboundCaPath);
        ExtractValue(tbl, "SSL", "client_ca_path", sslConfig.clientCaPath);

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

        // vvv OS Specific vvv
#ifdef WFX_PLATFORM_LINUX
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

        // vvv Metrics vvv
        ExtractValue(tbl, "Metrics", "max_routes", metricsConfig.maxRoutes);
        ExtractValue(tbl, "Metrics", "max_endpoints", metricsConfig.maxEndpoints);
        ExtractValue(tbl, "Metrics", "latency", metricsConfig.latency);
    }
    catch(const toml::parse_error& err) {
        logger.Fatal("[Config]: File -> '", path, "', Error -> ", err.what());
    }

    ValidateSettings();
}

void Config::ValidateSettings()
{
    Logger& logger = GetLogger();

    // vvv Network vvv
    // http_parser.cpp rejects a request once headerEnd > maxReadBufferSize - contentLen, so a
    // maximally-sized-but-legal request (header and body both at their configured caps) can
    // only ever succeed if the buffer holds both in full
    const auto requiredBuffer = std::uint64_t{networkConfig.maxHeaderTotalSize} + networkConfig.maxBodyTotalSize;
    if(networkConfig.maxReadBufferSize < requiredBuffer)
        logger.Fatal("[Config]: recv_buffer_max (", networkConfig.maxReadBufferSize,
                     ") must be at least max_header_size + max_body_size (", requiredBuffer, ")");

    // 0 is a deliberate "grow to exactly what's needed in one shot" mode (see rw_buffer.cpp), but
    // an increment bigger than its own ceiling can never do anything a same-size clamp wouldn't
    if(networkConfig.readBufferIncSize > networkConfig.maxReadBufferSize)
        logger.Fatal("[Config]: recv_buffer_incr (", networkConfig.readBufferIncSize,
                     ") cannot exceed recv_buffer_max (", networkConfig.maxReadBufferSize, ")");
    if(networkConfig.sendBufferIncSize > networkConfig.maxSendBufferSize)
        logger.Fatal("[Config]: send_buffer_incr (", networkConfig.sendBufferIncSize,
                     ") cannot exceed send_buffer_max (", networkConfig.maxSendBufferSize, ")");

    // BitmapPool treats 0 slots as a deliberate always-empty pool, not a failure, so this
    // wouldn't crash: it would just silently make the server unable to ever accept a connection
    if(networkConfig.maxConnections == 0)
        logger.Fatal("[Config]: max_connections must be greater than 0");

    // http_parser.cpp rejects a request the moment the header count exceeds this, so 0 would
    // reject every request outright, including the mandatory Host header
    if(networkConfig.maxHeaderTotalCount == 0)
        logger.Fatal("[Config]: max_header_count must be greater than 0");

    // vvv IP vvv
    // ConnectionLimiter denies a connection once connectionCount >= maxConnectionsPerIp; at 0
    // that's true before any connection ever lands, denying every IP forever
    if(ipConfig.maxConnectionsPerIp == 0)
        logger.Fatal("[Config]: max_connections_per_ip must be greater than 0");

    // RequestRateLimiter can never track more identities than this (FindOrCreate evicts before
    // overflowing); at 0 nothing is ever tracked, and AllowRequest denies by default for
    // anything untracked, so the whole rate limiter silently denies every request forever
    if(ipConfig.maxTrackedIdentities == 0)
        logger.Fatal("[Config]: max_tracked_identities must be greater than 0");

    // vvv SSL vvv
    // http_openssl.cpp already clamps/defaults an out-of-range value at the point of use, so
    // this can't crash. It exists purely to catch a silent, unnoticed security downgrade: e.g.
    // a typo'd min_proto_version falling back to TLS1.2 instead of the caller's real intent
    if(sslConfig.minProtoVersion < 1 || sslConfig.minProtoVersion > 3)
        logger.Fatal("[Config]: min_proto_version must be 1 (TLSv1.1), 2 (TLSv1.2), or 3 (TLSv1.3)");
    if(sslConfig.securityLevel < 0 || sslConfig.securityLevel > 5)
        logger.Fatal("[Config]: security_level must be between 0 and 5");

    // vvv Logging vvv
    // Cast directly to Logger::Level with no range check (logger.cpp), so an out-of-range value
    // here is undefined behavior, not just a silently-wrong log level
    if(loggingConfig.minLevel > 5)
        logger.Fatal("[Config]: min_level must be between 0 (trace) and 5 (fatal)");

    // vvv Misc vvv
    // Fed straight into nanosleep({masterPollInterval, 0}) in the master wait loop; 0 means a
    // zero-duration sleep, so the master process busy-spins at 100% CPU forever
    if(miscConfig.masterPollInterval == 0)
        logger.Fatal("[Config]: master_poll_interval must be greater than 0");
}

void Config::LoadFinalSettings(const std::string& projectDir)
{
    const std::string cwd = FileSystem::GetCurrentPath();
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