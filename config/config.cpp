#include "config.hpp"
#include "config_helper.hpp"

#include <thread>
#include <algorithm>

namespace WFX::Core {

using namespace WFX::Utils;
using namespace WFX::Core::ConfigHelpers;

Config& Config::GetInstance() {
    static Config config;
    return config;
}

void Config::LoadCoreSettings(std::string_view path) {
    Logger& logger = Logger::GetInstance();

    try {
        auto tbl = toml::parse_file(path);

        // vvv Project vvv
        ExtractValueOrFatal(tbl, logger, "Project", "project_name", projectConfig.projectName);
        ExtractStringArrayOrFatal(tbl, logger, "Project", "middleware_list", projectConfig.middlewareList);

        // vvv SSL vvv
        ExtractValue(tbl, logger, "SSL", "cert_path", sslConfig.certPath);
        ExtractValue(tbl, logger, "SSL", "key_path",  sslConfig.keyPath);

        // vvv Network vvv
        projectConfig.publicDir   = projectConfig.projectName + "/public/";
        projectConfig.templateDir = projectConfig.projectName + "/templates/";

        ExtractValue(tbl, logger, "Network", "send_buffer_max",             networkConfig.maxSendBufferSize);
        ExtractValue(tbl, logger, "Network", "recv_buffer_max",             networkConfig.maxRecvBufferSize);
        ExtractValue(tbl, logger, "Network", "recv_buffer_incr",            networkConfig.bufferIncrSize);
        ExtractValue(tbl, logger, "Network", "header_reserve_hint",         networkConfig.headerReserveHintSize);
        ExtractValue(tbl, logger, "Network", "max_header_size",             networkConfig.maxHeaderTotalSize);
        ExtractValue(tbl, logger, "Network", "max_header_count",            networkConfig.maxHeaderTotalCount);
        ExtractValue(tbl, logger, "Network", "max_body_size",               networkConfig.maxBodyTotalSize);
        ExtractValue(tbl, logger, "Network", "header_timeout",              networkConfig.headerTimeout);
        ExtractValue(tbl, logger, "Network", "body_timeout",                networkConfig.bodyTimeout);
        ExtractValue(tbl, logger, "Network", "idle_timeout",                networkConfig.idleTimeout);
        ExtractValue(tbl, logger, "Network", "max_connections",             networkConfig.maxConnections);
        ExtractValue(tbl, logger, "Network", "max_connections_per_ip",      networkConfig.maxConnectionsPerIp);
        ExtractValue(tbl, logger, "Network", "max_request_burst_per_ip",    networkConfig.maxRequestBurstSize);
        ExtractValue(tbl, logger, "Network", "max_requests_per_ip_per_sec", networkConfig.maxTokensPerSecond);

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

        ExtractValue(tbl, logger, "Windows", "accept_slots", osSpecificConfig.maxAcceptSlots);
        ExtractAutoOrAll(tbl, logger, "Windows", "connection_threads",
                         osSpecificConfig.workerThreadCount, defaultIOCP, threadCount);
        ExtractAutoOrAll(tbl, logger, "Windows", "request_threads",
                         osSpecificConfig.callbackThreadCount, defaultUser, threadCount);
    #else
        ExtractValue(tbl, logger, "Linux", "worker_processes", osSpecificConfig.workerProcesses);
        ExtractValue(tbl, logger, "Linux", "backlog",          osSpecificConfig.backlog);
        ExtractValue(tbl, logger, "Linux", "file_cache_size",  osSpecificConfig.fileCacheSize);
        
        #ifdef WFX_LINUX_USE_IO_URING
            ExtractValue(tbl, logger, "Linux.IoUring", "accept_slots",    osSpecificConfig.acceptSlots);
            ExtractValue(tbl, logger, "Linux.IoUring", "queue_depth",     osSpecificConfig.queueDepth);
            ExtractValue(tbl, logger, "Linux.IoUring", "batch_size",      osSpecificConfig.batchSize);
            ExtractValue(tbl, logger, "Linux.IoUring", "file_chunk_size", osSpecificConfig.fileChunkSize);
        #else
            ExtractValue(tbl, logger, "Linux.Epoll", "max_events", osSpecificConfig.maxEvents);
        #endif // WFX_LINUX_USE_IO_URING
    #endif // _WIN32
    }
    catch(const toml::parse_error& err) {
        logger.Fatal("[Config]: '", path, "' ", err.what(),
                     ". 'wfx.toml' should be present for the framework to 'w o r k'.");
    }
}

void Config::LoadToolchainSettings(std::string_view path) {
    Logger& logger = Logger::GetInstance();
    try {
        auto tbl = toml::parse_file(path);

        ExtractValueOrFatal(tbl, logger, "Compiler", "ccmd",    toolchainConfig.ccmd);
        ExtractValueOrFatal(tbl, logger, "Compiler", "lcmd",    toolchainConfig.lcmd);
        ExtractValueOrFatal(tbl, logger, "Compiler", "cargs",   toolchainConfig.cargs);
        ExtractValueOrFatal(tbl, logger, "Compiler", "largs",   toolchainConfig.largs);
        ExtractValueOrFatal(tbl, logger, "Compiler", "objflag", toolchainConfig.objFlag);
        ExtractValueOrFatal(tbl, logger, "Compiler", "dllflag", toolchainConfig.dllFlag);
    }
    catch(const toml::parse_error& err) {
        logger.Fatal("[Config]: '", path, "' ", err.what(),
                     ". Run 'wfx doctor' to generate ", path);
    }
}

} // namespace WFX::Core