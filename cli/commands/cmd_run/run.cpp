#include "run.hpp"

#include "cli/commands/common/common.hpp"
#include "config/config.hpp"
#include "engine/core_engine.hpp"
#include "engine/template_engine.hpp"
#include "http/common/http_master_state.hpp"
#include "utils/dotenv/dotenv.hpp"
#include "utils/fileops/filesystem.hpp"
#include "utils/crash_tracer/crash_tracer.hpp"

#ifdef _WIN32
    #include <windows.h>
#else
    #include <wait.h>
    #include <signal.h>
#endif

#include <thread>

namespace WFX::CLI {

using namespace WFX::Http;  // For 'WFXGlobalState', ...
using namespace WFX::Utils; // For 'Logger', 'BufferPool', 'FileCache', ...
using namespace WFX::Core;  // For 'Config', 'TemplateEngine'

// Implemented by each OS differently
int RunServerImpl(const ServerConfig& cfg, const std::string& logsDir, const std::string& crashLogsDir);

// Entrypoint
int RunServer(const std::string& project, const ServerConfig& cfg)
{
    auto& logger = GetLogger();
    auto& config = GetConfig();

    logger.SetMinLevel(Logger::Level::INFO);

    if(!FileSystem::DirectoryExists(project.c_str())) 
        logger.Fatal("[WFX]: '", project, "' directory does not exist");

    const std::string logsDir      = project + "/logs/default_logs/";
    const std::string crashLogsDir = project + "/logs/crash_logs/";

    if(!FileSystem::DirectoryExists(logsDir.c_str()) && !FileSystem::CreateDirectory(logsDir))
        logger.Fatal("[WFX]: Failed to create '", logsDir, "' directory for log dumps");

    if(!FileSystem::DirectoryExists(crashLogsDir.c_str()) && !FileSystem::CreateDirectory(crashLogsDir))
        logger.Fatal("[WFX]: Failed to create '", crashLogsDir, "' directory for crash dumps");

    // -------------------- LOADING PHASE --------------------
    config.LoadCoreSettings(project + "/wfx.toml");
    config.LoadFinalSettings(project);

    EnvConfig envConfig;
    envConfig.SetFlag(EnvFlags::REQUIRE_OWNER_UID);
    envConfig.SetFlag(EnvFlags::REQUIRE_PERMS_600);

    if(Dotenv::LoadFromFile(config.envConfig.envPath, envConfig))
        logger.Info("[WFX-Master]: Loaded '.env' successfully");

    return RunServerImpl(cfg, logsDir, crashLogsDir);
}

#ifdef _WIN32
int RunServerImpl(const ServerConfig& cfg, const std::string& logsDir, const std::string& crashLogsDir)
{
    GetLogger().Fatal("[WFX-Master]: Windows implementation not defined!");
    return 0;
}
#else
int RunServerImpl(const ServerConfig& cfg, const std::string& logsDir, const std::string& crashLogsDir)
{
    auto& globalState = GetMasterState();
    auto& logger      = GetLogger();
    auto& config      = GetConfig();
    auto& osConfig    = config.osSpecificConfig;
    auto& buildConfig = config.buildConfig;

    // -------------------- INITIALIZING PHASE --------------------
    signal(SIGINT, HandleMasterSignal);
    signal(SIGTERM, HandleMasterSignal);

    // -------------------- TEMPLATE / USER CODE COMPILATION PHASE --------------------
    HandleBuildDirectory();

    auto& templateEngine = GetTemplateEngine();
    auto [success, hasDynamic] = templateEngine.PreCompileTemplates();

    // Compile only user source
    if(!success || !hasDynamic)
        HandleUserCxxCompilation(CxxCompilationOption::SOURCE_ONLY);
    // Compile both source + templates
    else
        HandleUserCxxCompilation();

    // Load template library if it exists
    templateEngine.LoadDynamicTemplatesFromLib();

    bool pinToCpu = cfg.GetFlag(ServerFlags::PIN_TO_CPU);
    bool useHttps = cfg.GetFlag(ServerFlags::USE_HTTPS);
    bool ohp      = cfg.GetFlag(ServerFlags::OVERRIDE_HTTPS_PORT);

    // Switch ports if we enable https and we don't want to override https default port
    std::uint16_t port = useHttps && !ohp ? 443U : cfg.port;

    logger.Info("[WFX-Master]: Dev server running at ", useHttps ? "https://" : "http://", cfg.host, ':', port);
    logger.Info("[WFX-Master]: Press Ctrl+C to stop");

    // User logging preferences start from now on
    logger.SetMinLevel(static_cast<Logger::Level>(config.loggingConfig.minLevel));
    logger.EnableStdout(config.loggingConfig.enableStdout);
    logger.EnableColors(config.loggingConfig.enableColors);
    logger.EnableTimestamps(config.loggingConfig.enableTimestamps);
    logger.EnablePrometheus(config.loggingConfig.enablePrometheus);

    // -------------------- WORKERS SPAWNING PHASE --------------------
    const std::string dllDir = buildConfig.buildDir + "/user_entry.so";
    for(int i = 0; i < osConfig.workerProcesses; i++) {
        pid_t pid = fork();

        // --- Child Worker ---
        if(pid == 0) {
            if(i == 0)
                setpgid(0, 0);                      // First worker becomes group leader
            else
                setpgid(0, globalState.workerPGID); // Join first worker's group

            // Every process will have its own crash tracer
            char workerName[32];
            std::snprintf(workerName, sizeof(workerName), "worker-%d", i);
            CrashTracer::SetWorkerName(workerName);
            CrashTracer::Install(crashLogsDir.c_str());

            // And every single process will also have its own log tracer IF ENABLED
            if(config.loggingConfig.enableFile)
                logger.OpenFile(
                    (logsDir + workerName + ".log").c_str(),
                    config.loggingConfig.maxFileSize,
                    config.loggingConfig.maxRotations
                );

            // For every process initialize its own BufferPool and FileCache
            GetBufferPool().Init(1024 * 1024, [](std::size_t curSize) { return curSize * 2; });
            GetFileCache().Init(config.miscConfig.fileCacheSize);

            Core::CoreEngine engine{dllDir.c_str(), useHttps};
            globalState.enginePtr = &engine;

            signal(SIGTERM, HandleWorkerSignal);
            signal(SIGINT, SIG_IGN);  // SigTerm will kill it, SigInt handled by master
            signal(SIGPIPE, SIG_IGN); // We will handle it internally
            signal(SIGHUP, SIG_IGN);  // Terminals should not kill workers

            if(pinToCpu)
                PinWorkerToCPU(i);

            engine.Listen(cfg.host, port);
            return 0;
        }

        // --- Master ---
        else if(pid > 0) {
            globalState.workerPids.push_back(pid);
            if(i == 0)
                globalState.workerPGID = pid; // Store PGID for process group

            setpgid(pid, globalState.workerPGID);
        }

        else {
            logger.Error("[WFX-Master]: Failed to fork worker ", i);
            return 1;
        }
    }

    // --- Master ---
    while(!globalState.shouldStop)
        pause();

    // Before shutting down, reset all the logging capabilities
    // Also i will only be enabling stdout (IF NOT ALREADY ENABLED, don't care about other stuff)
    // Cuz WHY NOT
    logger.EnableStdout(true);
    logger.SetMinLevel(Logger::Level::INFO);

    logger.Info("[WFX-Master]: Signal received (INT / TERM), waiting for workers to shutdown...");

    // -------------------- SHUTDOWN PHASE --------------------
    for(std::uint32_t i = 0; i < osConfig.workerProcesses; i++) {
        pid_t pid    = globalState.workerPids[i];
        bool  exited = false;

        for(std::uint32_t t = 0; t < config.osSpecificConfig.workerShutdownTimeout * 10; t++) {
            int status;
            pid_t ret = waitpid(pid, &status, WNOHANG);

            // Worker exited normally
            if(ret == pid) {
                exited = true;
                break;
            }

            // Poll every 100ms
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if(!exited) {
            // Worker didn't exit in time, force kill
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0); // Reap zombie
        }
    }

    // GG
    logger.Info("[WFX-Master]: Shutdown successfully");
    return 0;
}
#endif // _WIN32
}  // namespace WFX::CLI