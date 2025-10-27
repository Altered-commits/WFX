#include "dev.hpp"

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

using namespace WFX::Http;  // For 'WFXGlobalState'
using namespace WFX::Utils; // For 'Logger'

int RunDevServer(const ServerConfig& cfg)
{
    auto& logger      = Logger::GetInstance();
    auto& config      = Config::GetInstance();
    auto& fs          = FileSystem::GetFileSystem();
    auto& globalState = GetGlobalState();
    auto& osConfig    = config.osSpecificConfig;

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
    config.LoadCoreSettings("wfx.toml");
    config.LoadToolchainSettings("toolchain.toml");

    EnvConfig envConfig;
    envConfig.SetFlag(EnvFlags::REQUIRE_OWNER_UID);
    envConfig.SetFlag(EnvFlags::REQUIRE_PERMS_600);

    if(Dotenv::LoadFromFile(config.envConfig.envPath, envConfig))
        logger.Info("[WFX-Master]: Loaded .env successfully");
    else
        logger.Info("[WFX-Master]: Failed to load .env");

    // -------------------- INITIALIZING PHASE --------------------
    signal(SIGINT, HandleMasterSignal);
    signal(SIGTERM, SIG_IGN);

    // Handle initialization of SSL key before we do anything else
    if(!RandomPool::GetInstance().GetBytes(globalState.sslKey.data(), globalState.sslKey.size()))
        logger.Fatal("[WFX-Master]: Failed to initialize SSL key");

    // Create a master FileCache used throught the code
    // This works because master process ain't gon die before worker processes
    FileCache masterCache{config.miscConfig.fileCacheSize};
    globalState.fileCache = &masterCache;

    // -------------------- TEMPLATE COMPILATION PHASE --------------------
    auto& templateEngine = TemplateEngine::GetInstance();
    
    bool recompileViaFlag = cfg.GetFlag(ServerFlags::NO_TEMPLATE_CACHE);
    bool recompile        = recompileViaFlag || !templateEngine.LoadTemplatesFromCache();
    
    if(recompile) {
        logger.Info(
            recompileViaFlag
            ? "[WFX-Master]: --no-template-cache flag detected, compiling templates"
            : "[WFX-Master]: Re-compiling templates"
        );
        templateEngine.PreCompileTemplates();
    }

    // We store it in global state because template engine carries mappings from-
    // -relative paths (index.html) -> template metadata (type, path, etc)
    // We want to be able to access same data across workers even after COW
    globalState.templateEnginePtr = &templateEngine;

    // -------------------- USER CODE COMPILATION PHASE --------------------
    const std::string dllDir      = config.projectConfig.projectName + "/build/dlls/";
    const char*       dllDirCStr  = dllDir.c_str();
    const std::string dllPath     = dllDir + "user_entry.so";
    const char*       dllPathCStr = dllPath.c_str();

    if(cfg.GetFlag(ServerFlags::NO_BUILD_CACHE) || !fs.FileExists(dllPathCStr))
        HandleUserSrcCompilation(dllDirCStr, dllPathCStr);
    else
        logger.Info("[WFX-Master]: File already exists, skipping user code compilation");

    // Switch ports if we enable https and we don't want to override https default port
    bool useHttps = cfg.GetFlag(ServerFlags::USE_HTTPS);
    bool ohp      = cfg.GetFlag(ServerFlags::OVERRIDE_HTTPS_PORT);

    int port = useHttps && !ohp ? 443 : cfg.port;
    logger.Info("[WFX-Master]: Dev server running at ",
                useHttps ? "https://" : "http://", cfg.host, ':', port);

    logger.Info("[WFX-Master]: Press Ctrl+C to stop");
    logger.SetLevelMask(WFX_LOG_INFO | WFX_LOG_WARNINGS);

    // -------------------- WORKERS SPAWNING PHASE --------------------
    for(int i = 0; i < osConfig.workerProcesses; i++) {
        pid_t pid = fork();

        // --- Child Worker ---
        if(pid == 0) {
            if(i == 0)
                setpgid(0, 0);          // First worker becomes group leader
            else
                setpgid(0, globalState.workerPGID); // Join first worker's group

            WFX::Core::CoreEngine engine{dllPathCStr, useHttps};
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

    // Before shutdown, write template cache to cache.bin for future use
    templateEngine.SaveTemplatesToCache();

    logger.Info("[WFX-Master]: Shutdown successfully");
    return 0;
}

}  // namespace WFX::CLI