#include "run.hpp"

#include "cli/commands/common/common.hpp"
#include "config/config.hpp"
#include "engine/core_engine.hpp"
#include "engine/template_engine.hpp"
#include "http/common/http_global_state.hpp"
#include "utils/dotenv/dotenv.hpp"
#include "utils/logger/logger.hpp"
#include "utils/filesystem/filesystem.hpp"
#include "utils/backport/string.hpp"

#ifdef _WIN32
    #include <windows.h>
#else
    #include <wait.h>
    #include <signal.h>
#endif

namespace WFX::CLI {

using namespace WFX::Http;  // For 'WFXGlobalState', ...
using namespace WFX::Utils; // For 'Logger', 'BufferPool', 'FileCache', ...

int RunServer(const std::string& project, const ServerConfig& cfg)
{
    auto& logger      = Logger::GetInstance();
    auto& config      = Config::GetInstance();
    auto& fs          = FileSystem::GetFileSystem();
    auto& globalState = GetGlobalState();
    auto& osConfig    = config.osSpecificConfig;
    auto& projConfig  = config.projectConfig;

    // Sanity check project directory existence
    if(!fs.DirectoryExists(project.c_str()))
        logger.Fatal("[WFX]: '", project, "' directory does not exist");

#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    SetUnhandledExceptionFilter(ExceptionFilter);

    WFX::Core::CoreEngine engine{noCache};
    engine.Listen(host, port);

    while(!shouldStop)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    engine.Stop();
#else
    // -------------------- LOADING PHASE --------------------
    config.LoadCoreSettings(project + "/wfx.toml");
    config.LoadFinalSettings(project);

    EnvConfig envConfig;
    envConfig.SetFlag(EnvFlags::REQUIRE_OWNER_UID);
    envConfig.SetFlag(EnvFlags::REQUIRE_PERMS_600);

    if(Dotenv::LoadFromFile(config.envConfig.envPath, envConfig))
        logger.Info("[WFX-Master]: Loaded '.env' successfully");

    // -------------------- INITIALIZING PHASE --------------------
    signal(SIGINT, HandleMasterSignal);
    signal(SIGTERM, SIG_IGN);

    // Handle initialization of SSL key before we do anything else
    if(!RandomPool::GetInstance().GetBytes(globalState.sslKey.data(), globalState.sslKey.size()))
        logger.Fatal("[WFX-Master]: Failed to initialize SSL key");

    // -------------------- TEMPLATE / USER CODE COMPILATION PHASE --------------------
    HandleBuildDirectory();

    auto& templateEngine = TemplateEngine::GetInstance();
    auto [success, hasDynamic] = templateEngine.PreCompileTemplates();

    // Compile only user source
    if(!success || !hasDynamic)
        HandleUserCxxCompilation(CxxCompilationOption::SOURCE_ONLY);

    // Compile both source + templates
    else {
        HandleUserCxxCompilation();
        templateEngine.LoadDynamicTemplatesFromLib();
    }

    // Switch ports if we enable https and we don't want to override https default port
    bool useHttps = cfg.GetFlag(ServerFlags::USE_HTTPS);
    bool ohp      = cfg.GetFlag(ServerFlags::OVERRIDE_HTTPS_PORT);

    int port = useHttps && !ohp ? 443 : cfg.port;
    logger.Info("[WFX-Master]: Dev server running at ",
                useHttps ? "https://" : "http://", cfg.host, ':', port);

    logger.Info("[WFX-Master]: Press Ctrl+C to stop");
    logger.SetLevelMask(WFX_LOG_INFO | WFX_LOG_WARNINGS);

    // -------------------- WORKERS SPAWNING PHASE --------------------
    const std::string dllDir = projConfig.buildDir + "/user_entry.so";
    for(int i = 0; i < osConfig.workerProcesses; i++) {
        pid_t pid = fork();

        // --- Child Worker ---
        if(pid == 0) {
            if(i == 0)
                setpgid(0, 0);          // First worker becomes group leader
            else
                setpgid(0, globalState.workerPGID); // Join first worker's group

            // For every process initialize its own BufferPool and FileCache
            BufferPool::GetInstance().Init(1024 * 1024, [](std::size_t curSize) { return curSize * 2; });
            FileCache::GetInstance().Init(config.miscConfig.fileCacheSize);

            WFX::Core::CoreEngine engine{dllDir.c_str(), useHttps};
            globalState.enginePtr = &engine;

            signal(SIGTERM, HandleWorkerSignal);
            signal(SIGINT, SIG_IGN);  // SigTerm will kill it, SigInt handled by master
            signal(SIGPIPE, SIG_IGN); // We will handle it internally
            signal(SIGHUP, SIG_IGN);  // Terminals should not kill workers

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

    // -------------------- SHUTDOWN PHASE --------------------
    for(int i = 0; i < osConfig.workerProcesses; i++)
        waitpid(globalState.workerPids[i], nullptr, 0);
#endif // _WIN32

    logger.Info("[WFX-Master]: Shutdown successfully");
    return 0;
}

}  // namespace WFX::CLI