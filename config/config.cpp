#include "config.hpp"
#include "utils/logger/logger.hpp"
#include "third_party/tomlplusplus/toml.hpp"

#include <thread>

// For ease of use
#define TOML_GET(tbl, logger, section, field, target)                                   \
    if(auto val = tbl[section][field].value<decltype(target)>())                        \
        target = *val;                                                                  \
    else                                                                                \
        logger.Warn("[Config]: Missing or invalid entry: [", section, "] ", field,      \
                    ". Using default value: ", target)

#define TOML_GET_OR_FATAL(tbl, logger, section, field, target)                            \
    if(auto val = tbl[section][field].value<decltype(target)>())                          \
        target = *val;                                                                    \
    else                                                                                  \
        logger.Fatal("[Config]: Missing or invalid entry: [", section, "] ", field, '.')

#define TOML_GET_AUTO_OR_ALL(tbl, logger, section, field, target, autoValue, allValue)          \
    if(auto node = tbl[section][field]) {                                                       \
        if(auto val = node.value<decltype(target)>())                                           \
            target = *val;                                                                      \
        else if(auto str = node.value<std::string>()) {                                         \
            if(*str == "auto")                                                                  \
                target = autoValue;                                                             \
            else if(*str == "all")                                                              \
                target = allValue;                                                              \
            else                                                                                \
                logger.Warn("[Config]: Invalid string entry: [", section, "] ", field,          \
                            " = ", *str, ". Using default value: ", target);                    \
        } else                                                                                  \
            logger.Warn("[Config]: Invalid entry: [", section, "] ", field,                     \
                        ". Using default value: ", target);                                     \
    } else                                                                                      \
        logger.Warn("[Config]: Missing entry: [", section, "] ", field,                         \
                    ". Using default value: ", target);

namespace WFX::Core {

using namespace WFX::Utils; // For 'Logger'

Config& Config::GetInstance()
{
    static Config config;
    return config;
}

void Config::LoadCoreSettings(std::string_view path)
{
    unsigned int cores = std::thread::hardware_concurrency();
    Logger& logger = Logger::GetInstance();

    try {
        auto tbl = toml::parse_file(path);

        TOML_GET_OR_FATAL(tbl, logger, "Project", "project_name", projectConfig.projectName);

        // Set 'publicDir' and 'templateDir', these will be used alot in future
        projectConfig.publicDir   = projectConfig.projectName + "/public/";
        projectConfig.templateDir = projectConfig.projectName + "/templates/";

        TOML_GET(tbl, logger, "Network", "recv_buffer_max",             networkConfig.maxRecvBufferSize);
        TOML_GET(tbl, logger, "Network", "recv_buffer_incr",            networkConfig.bufferIncrSize);
        TOML_GET(tbl, logger, "Network", "header_reserve_hint",         networkConfig.headerReserveHintSize);
        TOML_GET(tbl, logger, "Network", "max_header_size",             networkConfig.maxHeaderTotalSize);
        TOML_GET(tbl, logger, "Network", "max_header_count",            networkConfig.maxHeaderTotalCount);
        TOML_GET(tbl, logger, "Network", "max_body_size",               networkConfig.maxBodyTotalSize);
        TOML_GET(tbl, logger, "Network", "header_timeout",              networkConfig.headerTimeout);
        TOML_GET(tbl, logger, "Network", "body_timeout",                networkConfig.bodyTimeout);
        TOML_GET(tbl, logger, "Network", "idle_timeout",                networkConfig.idleTimeout);
        TOML_GET(tbl, logger, "Network", "max_connections",             networkConfig.maxConnections);
        TOML_GET(tbl, logger, "Network", "max_connections_per_ip",      networkConfig.maxConnectionsPerIp);
        TOML_GET(tbl, logger, "Network", "max_request_burst_per_ip",    networkConfig.maxRequestBurstSize);
        TOML_GET(tbl, logger, "Network", "max_requests_per_ip_per_sec", networkConfig.maxTokensPerSecond);

    #ifdef _WIN32
        unsigned int defaultIOCP = std::max(2u, cores / 2);
        unsigned int defaultUser = std::max(2u, cores - defaultIOCP);

        TOML_GET(tbl, logger, "Windows", "accept_slots", osSpecificConfig.maxAcceptSlots);
        TOML_GET_AUTO_OR_ALL(tbl, logger, "Windows", "connection_threads",
                        osSpecificConfig.workerThreadCount, defaultIOCP, cores);
        TOML_GET_AUTO_OR_ALL(tbl, logger, "Windows", "request_threads",
                        osSpecificConfig.callbackThreadCount, defaultUser, cores);
    #else
        TOML_GET(tbl, logger, "Linux", "worker_connections", osSpecificConfig.workerConnections);
    #endif
    }
    catch(const toml::parse_error& err) {
        logger.Fatal("[Config]: '", path, "' ", err.what(), ". 'wfx.toml' should be present for the framework to 'w o r k'.");
    }
}

void Config::LoadToolchainSettings(std::string_view path)
{
    Logger& logger = Logger::GetInstance();
    try {
        auto tbl = toml::parse_file(path);

        TOML_GET_OR_FATAL(tbl, logger, "Compiler", "command", toolchainConfig.command);
        TOML_GET_OR_FATAL(tbl, logger, "Compiler", "cargs", toolchainConfig.cargs);
        TOML_GET_OR_FATAL(tbl, logger, "Compiler", "largs", toolchainConfig.largs);
    }
    catch(const toml::parse_error& err) {
        logger.Fatal("[Config]: '", path, "' ", err.what(), ". Run 'wfx doctor' to generate ", path);
    }
}

} // namespace WFX::Core