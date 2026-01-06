#include "config.hpp"
#include "config_helper.hpp"

#ifdef _WIN32
    #include <thread>
#endif

namespace WFX::Core {

using namespace WFX::Utils;
using namespace WFX::Core::ConfigHelpers;

Config& Config::GetInstance()
{
    static Config config;
    return config;
}

void Config::LoadCoreSettings(std::string_view path)
{
    Logger& logger = Logger::GetInstance();

    try {
        auto tbl = toml::parse_file(path);

        // vvv Project vvv
        ExtractValueOrFatal(tbl, "Project", "project_name", projectConfig.projectName);
        ExtractStringArrayOrFatal(tbl, "Project", "middleware_list", projectConfig.middlewareList);
        
        projectConfig.publicDir   = projectConfig.projectName + "/public";
        projectConfig.templateDir = projectConfig.projectName + "/templates";

        // vvv ENV vvv
        ExtractValueOrFatal(tbl, "ENV", "env_path", envConfig.envPath);

        // vvv SSL vvv
        ExtractValueOrFatal(tbl, "SSL", "cert_path", sslConfig.certPath);
        ExtractValueOrFatal(tbl, "SSL", "key_path",  sslConfig.keyPath);

        ExtractValue(tbl, "SSL", "tls13_ciphers",        sslConfig.tls13Ciphers);
        ExtractValue(tbl, "SSL", "tls12_ciphers",        sslConfig.tls12Ciphers);
        ExtractValue(tbl, "SSL", "curves",               sslConfig.curves);
        ExtractValue(tbl, "SSL", "enable_session_cache", sslConfig.enableSessionCache);
        ExtractValue(tbl, "SSL", "enable_ktls",          sslConfig.enableKTLS);
        ExtractValue(tbl, "SSL", "session_cache_size",   sslConfig.sessionCacheSize);
        ExtractValue(tbl, "SSL", "min_proto_version",    sslConfig.minProtoVersion);
        ExtractValue(tbl, "SSL", "security_level",       sslConfig.securityLevel);

        // vvv Network vvv
        ExtractValue(tbl, "Network", "send_buffer_max",             networkConfig.maxSendBufferSize);
        ExtractValue(tbl, "Network", "recv_buffer_max",             networkConfig.maxRecvBufferSize);
        ExtractValue(tbl, "Network", "recv_buffer_incr",            networkConfig.bufferIncrSize);
        ExtractValue(tbl, "Network", "header_reserve_hint",         networkConfig.headerReserveHintSize);
        ExtractValue(tbl, "Network", "max_header_size",             networkConfig.maxHeaderTotalSize);
        ExtractValue(tbl, "Network", "max_body_size",               networkConfig.maxBodyTotalSize);
        ExtractValue(tbl, "Network", "max_header_count",            networkConfig.maxHeaderTotalCount);
        ExtractValue(tbl, "Network", "header_timeout",              networkConfig.headerTimeout);
        ExtractValue(tbl, "Network", "body_timeout",                networkConfig.bodyTimeout);
        ExtractValue(tbl, "Network", "idle_timeout",                networkConfig.idleTimeout);
        ExtractValue(tbl, "Network", "max_connections",             networkConfig.maxConnections);
        ExtractValue(tbl, "Network", "max_connections_per_ip",      networkConfig.maxConnectionsPerIp);
        ExtractValue(tbl, "Network", "max_request_burst_per_ip",    networkConfig.maxRequestBurstSize);
        ExtractValue(tbl, "Network", "max_requests_per_ip_per_sec", networkConfig.maxTokensPerSecond);

        // vvv OS Specific vvv
    #ifdef _WIN32
        unsigned int cores = std::thread::hardware_concurrency();

        // Sanity checks
        if(cores == 0 || cores > std::numeric_limits<std::uint16_t>::max()) {
            // Fallback
            cores = 2; 
            logger.Warn("[Config]: Invalid hardware_concurrency() result. Using fallback = ", cores);
        }

        std::uint16_t threadCount = static_cast<std::uint16_t>(cores);
        logger.Info("[Config]: Detected hardware concurrency = ", threadCount);

        std::uint16_t defaultIOCP = std::max(2, threadCount / 2);
        std::uint16_t defaultUser = std::max(2, threadCount - defaultIOCP);

        ExtractValue(tbl, "Windows", "accept_slots", osSpecificConfig.maxAcceptSlots);
        ExtractAutoOrAll(tbl, "Windows", "connection_threads",
                         osSpecificConfig.workerThreadCount, defaultIOCP, threadCount);
        ExtractAutoOrAll(tbl, "Windows", "request_threads",
                         osSpecificConfig.callbackThreadCount, defaultUser, threadCount);
    #else
        ExtractValue(tbl, "Linux", "worker_processes", osSpecificConfig.workerProcesses);
        ExtractValue(tbl, "Linux", "backlog",          osSpecificConfig.backlog);
        
        #ifdef WFX_LINUX_USE_IO_URING
            ExtractValue(tbl, "Linux.IoUring", "accept_slots",    osSpecificConfig.acceptSlots);
            ExtractValue(tbl, "Linux.IoUring", "queue_depth",     osSpecificConfig.queueDepth);
            ExtractValue(tbl, "Linux.IoUring", "batch_size",      osSpecificConfig.batchSize);
            ExtractValue(tbl, "Linux.IoUring", "file_chunk_size", osSpecificConfig.fileChunkSize);
        #else
            ExtractValue(tbl, "Linux.Epoll", "max_events", osSpecificConfig.maxEvents);
        #endif // WFX_LINUX_USE_IO_URING
    #endif // _WIN32

        // vvv Misc vvv
        ExtractValue(tbl, "Misc", "file_cache_size",      miscConfig.fileCacheSize);
        ExtractValue(tbl, "Misc", "cache_chunk_size",     miscConfig.cacheChunkSize);
        ExtractValue(tbl, "Misc", "template_chunk_size",  miscConfig.templateChunkSize);
    }
    catch(const toml::parse_error& err) {
        logger.Fatal("[Config]: File -> 'wfx.toml', Error -> ", err.what());
    }
}

void Config::LoadToolchainSettings(std::string_view path, bool isDebug)
{
    try {
        auto tbl = toml::parse_file(path);

        ExtractValueOrFatal(tbl, "Compiler", "ccmd",    toolchainConfig.ccmd);
        ExtractValueOrFatal(tbl, "Compiler", "lcmd",    toolchainConfig.lcmd);
        ExtractValueOrFatal(tbl, "Compiler", "objflag", toolchainConfig.objFlag);
        ExtractValueOrFatal(tbl, "Compiler", "dllflag", toolchainConfig.dllFlag);

        const char* sec = isDebug ? "Compiler.Debug" : "Compiler.Prod";
        ExtractValueOrFatal(tbl, sec, "cargs", toolchainConfig.cargs);
        ExtractValueOrFatal(tbl, sec, "largs", toolchainConfig.largs);
    }
    catch(const toml::parse_error& err) {
        Logger::GetInstance().Fatal("[Config]: File -> 'toolchain.toml', Error -> ", err.what());
    }
}

} // namespace WFX::Core