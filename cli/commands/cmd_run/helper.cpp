#include "run.hpp"
#include "cli/commands/common/common.hpp"
#include "engine/core_engine.hpp"
#include "engine/template_engine.hpp"
#include "http/common/http_master_state.hpp"
#include "utils/daemon/daemon_registry.hpp"
#include "utils/diagnostics/crash_tracer.hpp"
#include "utils/diagnostics/metric_tracer.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <wait.h>
#include <signal.h>
#include <fcntl.h>
#endif

#include <ctime>
#include <thread>

namespace WFX::CLI {

using namespace WFX::Http;  // For 'WFXGlobalState', ...
using namespace WFX::Utils; // For 'Logger', 'BufferPool', 'FileCache', ...
using namespace WFX::Core;  // For 'Config', 'TemplateEngine'

#ifdef _WIN32
// Windows: future work
#else
void PollWorkerMetrics()
{
    auto& globalState = GetMasterState();

    for(int i = 0; i < static_cast<int>(globalState.workerPids.size()); i++) {
        pid_t pid = globalState.workerPids[i];
        if(pid <= 0)
            continue;

        auto* slot = MetricTracer::Slot(i);
        if(!slot)
            continue;

        // Build /proc/<pid>/status path on stack
        char path[32];
        std::snprintf(path, sizeof(path), "/proc/%d/status", static_cast<int>(pid));

        int fd = ::open(path, O_RDONLY);
        if(fd < 0)
            continue;

        // proc files report size 0, just read directly into stack buffer
        char buf[2048];
        ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        ::close(fd);

        if(n <= 0)
            continue;

        buf[n] = '\0';

        // Parse VmRSS and VmSize with a single pass
        const char* ptr = buf;
        const char* end = buf + n;

        std::uint64_t rssKb = 0;
        std::uint64_t vmKb = 0;
        int found = 0;

        while(ptr < end && found < 2) {
            // Find line end
            const char* lineEnd = static_cast<const char*>(std::memchr(ptr, '\n', end - ptr));
            if(!lineEnd)
                lineEnd = end;

            std::size_t lineLen = static_cast<std::size_t>(lineEnd - ptr);

            // VmRSS and VmSize are both 6 chars + ':'
            if(lineLen > 7) {
                std::uint64_t* target = nullptr;

                if(std::memcmp(ptr, "VmRSS:", 6) == 0) {
                    target = &rssKb;
                    ++found;
                }
                else if(std::memcmp(ptr, "VmSize:", 7) == 0) {
                    target = &vmKb;
                    ++found;
                }

                if(target) {
                    // Skip past the key and whitespace
                    const char* valPtr = ptr + (target == &rssKb ? 6 : 7);
                    while(valPtr < lineEnd && (*valPtr == ' ' || *valPtr == '\t'))
                        ++valPtr;

                    std::from_chars(valPtr, lineEnd, *target);
                }
            }

            ptr = lineEnd + 1;
        }

        // Convert kB to bytes
        slot->self.rssBytes = rssKb * 1024;
        slot->self.vmBytes = vmKb * 1024;
        slot->self.pid = static_cast<std::int32_t>(pid);
    }
}

int RunServerImpl(const ServerConfig& cfg, const std::string& logsDir, const std::string& crashLogsDir)
{
    auto& globalState = GetMasterState();
    auto& logger = GetLogger();
    auto& config = GetConfig();
    auto& osConfig = config.osSpecificConfig;
    auto& buildConfig = config.buildConfig;
    auto& loggingConfig = config.loggingConfig;

    // Used in daemon registry
    auto& projectName = config.projectConfig.projectName;
    auto& projectAbsolutePath = config.projectConfig.projectPath;

    // -------------------- INITIALIZING PHASE --------------------
    signal(SIGINT, HandleMasterSignal);
    signal(SIGTERM, HandleMasterSignal);
    signal(SIGCHLD, [](int) {}); // Used to wake master up from 'pause()'

    if(!GetRandomPool().GenerateSSLKey())
        logger.Fatal("[WFX-Master]: Failed to generate SSL key");

    if(!MetricTracer::Create(osConfig.workerProcesses))
        logger.Fatal("[WFX-Master]: Failed to initialize metric tracer region");

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
    bool ohp = cfg.GetFlag(ServerFlags::OVERRIDE_HTTPS_PORT);

    // Switch ports if we enable https and we don't want to override https default port
    std::uint16_t port = useHttps && !ohp ? 443U : cfg.port;

    logger.Info("[WFX-Master]: Server running at ", useHttps ? "https://" : "http://", cfg.host, ':', port);
    logger.Info("[WFX-Master]: Press Ctrl+C to stop");

    // -------------------- DAEMON CONVERSION PHASE (IF ENABLED) --------------------
    if(cfg.GetFlag(ServerFlags::USE_DAEMON)) {
        pid_t pid = fork();

        if(pid < 0)
            logger.Fatal("[WFX-Master]: Failed to detach from terminal");

        // Parent (launcher) exits, child continues as daemon master
        if(pid > 0) {
            logger.Info("[WFX-Master]: Exec as daemon. Master pid = ", pid);
            std::exit(0);
        }

        // Child becomes session leader
        if(setsid() < 0)
            logger.Fatal("[WFX-Master]: Failed to create new session");

        // Redirect stdio to /dev/null
        int devNull = open("/dev/null", O_RDWR);
        if(devNull < 0)
            logger.Fatal("[WFX-Master]: Failed to open /dev/null: ", strerror(errno));

        dup2(devNull, STDIN_FILENO);
        dup2(devNull, STDOUT_FILENO);
        dup2(devNull, STDERR_FILENO);
        close(devNull);
    }

    // Written after detach logic (if enabled) so PID reflects actual daemon master
    {
        DaemonInfo info;
        info.project = projectName;
        info.path = projectAbsolutePath;
        info.host = cfg.host;
        info.port = port;
        info.https = useHttps;
        info.workers = osConfig.workerProcesses;
        info.started = static_cast<std::int64_t>(std::time(nullptr));
        info.pid = getpid();

        if(!DaemonRegistry::Write(info))
            logger.Fatal("[WFX-Master]: Failed to write PID file for '", projectName, "'");
    }

    // User logging preferences start from now on
    logger.SetMinLevel(static_cast<Logger::Level>(loggingConfig.minLevel));
    logger.EnableStdout(loggingConfig.enableStdout);
    logger.EnableColors(loggingConfig.enableColors);
    logger.EnableTimestamps(loggingConfig.enableTimestamps);

    // -------------------- WORKERS SPAWNING PHASE --------------------
    const std::string dllDir = buildConfig.buildDir + "/user_entry.so";
    for(int i = 0; i < osConfig.workerProcesses; i++) {
        pid_t pid = fork();

        // --- Child Worker ---
        if(pid == 0) {
            if(i == 0)
                setpgid(0, 0); // First worker becomes group leader
            else
                setpgid(0, globalState.workerPGID); // Join first worker's group

            // Every process will have its own metric tracer
            MetricTracer::InitWorker(i);

            // AND A CRASH tracer for good luck
            char workerName[32];
            std::snprintf(workerName, sizeof(workerName), "worker-%d", i);
            CrashTracer::SetWorkerName(workerName);
            CrashTracer::Install(crashLogsDir.c_str());

            // AND ITS OWN LOGGING IF ENABLED
            if(loggingConfig.enableFile)
                logger.OpenFile((logsDir + workerName + ".log").c_str(), loggingConfig.maxFileSize,
                                loggingConfig.maxRotations);

            // AAAAAAAAAAAAND ITS OWNNNNNNNNNNNNN BufferPool and FileCache
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

        // --- Failure ---
        else {
            logger.Error("[WFX-Master]: Failed to fork worker ", i);

            // Clean up before bailing
            MetricTracer::Destroy();
            if(!DaemonRegistry::Delete(projectName))
                logger.Warn("[WFX-Master]: Failed to delete PID file during fork failure cleanup");

            return 1;
        }
    }

    // --- Master wait loop ---
    while(!globalState.shouldStop) {
        // Sleep for poll interval, wake early on any signal
        struct timespec ts {
            config.miscConfig.metricsPollInterval, 0
        };
        nanosleep(&ts, nullptr);

        // Server stopped
        if(globalState.shouldStop)
            break;

        PollWorkerMetrics();
    }

    // Before shutting down, reset all the logging capabilities
    // Also i will only be enabling stdout (IF NOT ALREADY ENABLED, don't care about other stuff)
    // Cuz WHY NOT
    logger.EnableStdout(true);
    logger.SetMinLevel(Logger::Level::INFO);

    logger.Info("[WFX-Master]: Signal received (INT / TERM), waiting for workers to shutdown...");

    // -------------------- SHUTDOWN PHASE --------------------
    for(std::uint32_t i = 0; i < osConfig.workerProcesses; i++) {
        pid_t pid = globalState.workerPids[i];
        bool exited = false;

        for(std::uint32_t t = 0; t < config.osSpecificConfig.workerShutdownTimeout * 10; t++) {
            int status;
            pid_t ret = waitpid(pid, &status, WNOHANG);

            // Worker exited normally
            if(ret == pid) {
                exited = true;
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if(!exited) {
            // Worker didn't exit in time, force kill
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0); // Reap zombie
        }
    }

    // Hygiene (Not that it matters, OS would reclaim it anyways if this crashes)
    MetricTracer::Destroy();

    if(!DaemonRegistry::Delete(config.projectConfig.projectName))
        logger.Warn("[WFX-Master]: Failed to delete PID file on shutdown");

    // GG
    logger.Info("[WFX-Master]: Shutdown successfully");
    return 0;
}
#endif // _WIN32

void CheckAlreadyRunning(const std::string& projectName)
{
    auto& logger = GetLogger();

    DaemonInfo existing;
    switch(DaemonRegistry::Read(projectName, existing)) {
        case ReadResult::NOT_FOUND:
            return; // Good to go

        case ReadResult::IO_ERROR:
            logger.Fatal("[WFX-Master]: Failed to read PID file for '", projectName,
                         "'. "
                         "Check permissions on '",
                         DaemonRegistry::PidFilePath(projectName), "'");

        case ReadResult::CORRUPTED:
            logger.Warn("[WFX-Master]: Corrupted PID file found for '", projectName, "', removing it");

            if(!DaemonRegistry::Delete(projectName))
                logger.Fatal("[WFX-Master]: Failed to delete corrupted PID file for '", projectName, "'");

            return;

        case ReadResult::OK:
            break;
    }

    if(DaemonRegistry::IsAlive(existing.pid)) {
        logger.Fatal("[WFX-Master]: Project '", projectName, "' is already running (pid=", existing.pid,
                     "). "
                     "Use 'wfx control stop ",
                     projectName, "' to stop it or Ctrl+C if running in terminal");
    }

    // Process is dead but PID file exists, clean it up
    logger.Warn("[WFX-Master]: Removing stale PID file for '", projectName, "'");

    if(!DaemonRegistry::Delete(projectName))
        logger.Fatal("[WFX-Master]: Failed to delete stale PID file for '", projectName, "'");
}

} // namespace WFX::CLI