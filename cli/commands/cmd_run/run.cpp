// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "run.hpp"

#include "config/config.hpp"
#include "utils/fileops/filesystem.hpp"
#include "utils/dotenv/dotenv.hpp"
#include "utils/diagnostics/logger.hpp"

namespace WFX::CLI {

using namespace WFX::Utils; // For 'Logger', ...
using namespace WFX::Core;  // For 'Config'

// Forward declarations [all defined in 'helper.cpp' for clean code ofc *i should NOT be saying this shit*]
int RunServerImpl(const ServerConfig& cfg, const std::string& logsDir, const std::string& crashLogsDir);
void CheckAlreadyRunning(const std::string& projectName);

// Entrypoint
int RunServer(const std::string& projectName, const ServerConfig& cfg)
{
    auto& logger = GetLogger();
    auto& config = GetConfig();

    logger.SetMinLevel(Logger::Level::INFO);

    // -------------------- DUPLICATE CALL CHECK --------------------
    CheckAlreadyRunning(projectName);

    // -------------------- STARTUP PHASE --------------------
    if(!FileSystem::DirectoryExists(projectName.c_str()))
        logger.Fatal("[WFX]: '", projectName, "' directory does not exist");

    const std::string logsDir = projectName + "/logs/default_logs/";
    const std::string crashLogsDir = projectName + "/logs/crash_logs/";

    if(!FileSystem::DirectoryExists(logsDir.c_str()) && !FileSystem::CreateDirectory(logsDir))
        logger.Fatal("[WFX]: Failed to create '", logsDir, "' directory for log dumps");

    if(!FileSystem::DirectoryExists(crashLogsDir.c_str()) && !FileSystem::CreateDirectory(crashLogsDir))
        logger.Fatal("[WFX]: Failed to create '", crashLogsDir, "' directory for crash dumps");

    // -------------------- LOADING PHASE --------------------
    config.LoadCoreSettings(projectName + "/config/wfx." + cfg.env + ".toml");
    config.LoadFinalSettings(projectName);

    EnvConfig envConfig;
    envConfig.SetFlag(EnvFlags::REQUIRE_OWNER_UID);
    envConfig.SetFlag(EnvFlags::REQUIRE_PERMS_600);

    if(Dotenv::LoadFromFile(config.envConfig.envPath, envConfig))
        logger.Info("[WFX-Master]: Loaded '.env' successfully");

    return RunServerImpl(cfg, logsDir, crashLogsDir);
}

} // namespace WFX::CLI