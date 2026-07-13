// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "run.hpp"
#include "cli/commands/common/common.hpp"
#include "engine/core_engine.hpp"
#include "engine/template_engine.hpp"
#include "http/common/http_master_state.hpp"
#include "utils/pool/buffer_pool.hpp"
#include "utils/daemon/daemon_registry.hpp"
#include "utils/diagnostics/crash_tracer.hpp"
#include "utils/diagnostics/metric_tracer.hpp"

#include <wait.h>
#include <signal.h>
#include <fcntl.h>

#include <ctime>
#include <thread>
#include <vector>

namespace WFX::CLI {

using namespace WFX::Http;  // For 'WFXGlobalState', ...
using namespace WFX::Utils; // For 'Logger', 'BufferPool', 'FileCache', ...
using namespace WFX::Core;  // For 'Config', 'TemplateEngine'

// vvv Constants vvv
// Slot state encoding via workerPids:
//   >= 0  -> live worker PID
//   -1    -> marked for revival (backoff window not yet expired)
//   -2    -> permanently dead (max restart attempts exceeded)
static constexpr pid_t SLOT_PENDING = -1;
static constexpr pid_t SLOT_DEAD = -2;

// vvv Helper Functions vvv
static void InstallSignal(int sig, void (*handler)(int), const char* name)
{
    if(signal(sig, handler) == SIG_ERR)
        GetLogger().Fatal("[WFX-Master]: Failed to install handler for '", name, "'");
}

static bool SpawnWorker(int slotIndex, const std::string& dllDir, const std::string& logsDir,
                        const std::string& crashLogsDir, bool useHttps, bool pinToCpu, const std::string& host,
                        std::uint16_t port)
{
    auto& globalState = GetMasterState();
    auto& logger = GetLogger();
    auto& config = GetConfig();
    auto& loggingConfig = config.loggingConfig;

    const pid_t pid = fork();

    // --- Child Worker ---
    if(pid == 0) {
        // First worker becomes group leader, rest join its group
        if(globalState.workerPGID == 0)
            setpgid(0, 0);
        else
            setpgid(0, globalState.workerPGID);

        // Every process will have its own metric tracer
        MetricTracer::InitWorker(slotIndex);

        // AND A CRASH tracer for good luck
        char workerName[32];
        (void)std::snprintf(workerName, sizeof(workerName), "worker-%d", slotIndex);
        CrashTracer::SetWorkerName(workerName);
        CrashTracer::Install(crashLogsDir.c_str());

        // AND ITS OWN LOGGING IF ENABLED
        if(loggingConfig.enableFile)
            logger.OpenFile((logsDir + workerName + ".log").c_str(), loggingConfig.maxFileSize,
                            loggingConfig.maxRotations);

        // AAAAAAAAAAAAND ITS OWNNNNNNNNNNNNN BufferPool and FileCache
        GetBufferPool().Init(1024 * 1024, [](std::size_t curSize) { return curSize * 2; });
        GetFileCache().Init(config.miscConfig.fileCacheSize);

        InstallSignal(SIGTERM, HandleWorkerSignal, "SIGTERM");
        InstallSignal(SIGINT, SIG_IGN, "SIGINT");   // SigTerm will kill it, SigInt handled by master
        InstallSignal(SIGPIPE, SIG_IGN, "SIGPIPE"); // We will handle it internally
        InstallSignal(SIGHUP, SIG_IGN, "SIGHUP");   // Terminals should not kill workers

        if(pinToCpu)
            PinWorkerToCPU(slotIndex);

        // Starting the server bois. Brace yourself cuz shits about to get real
        {
            Core::CoreEngine engine{dllDir.c_str(), useHttps};
            globalState.enginePtr = &engine;
            engine.Listen(host, port);
        }

        std::exit(0);
    }

    // --- Failure ---
    else if(pid < 0) {
        logger.Error("[WFX-Master]: Failed to fork worker ", slotIndex);
        return false;
    }

    // --- Master ---
    // First worker spawned becomes group leader
    if(globalState.workerPGID == 0)
        globalState.workerPGID = pid;

    setpgid(pid, globalState.workerPGID);
    globalState.workerPids[slotIndex] = pid;

    // Update slot self metrics
    auto* slot = MetricTracer::Slot(slotIndex);
    if(slot) {
        slot->self.pid = static_cast<std::int32_t>(pid);
        slot->self.startedAt = static_cast<std::int64_t>(std::time(nullptr));
    }

    return true;
}

// Step 1: Reap dead workers and mark slots for revival or permanently dead
static void ReapDeadWorkers()
{
    auto& globalState = GetMasterState();
    auto& logger = GetLogger();
    auto& miscConfig = GetConfig().miscConfig;

    int status;
    pid_t dead;

    // Drain all dead children, SIGCHLD can be coalesced so we loop until nothing is left
    while((dead = waitpid(-1, &status, WNOHANG)) > 0) {
        // Find which slot this PID belongs to
        int slotIndex = -1;
        for(int i = 0; i < static_cast<int>(globalState.workerPids.size()); i++) {
            if(globalState.workerPids[i] == dead) {
                slotIndex = i;
                break;
            }
        }

        if(slotIndex < 0) {
            logger.Warn("[WFX-Master]: Unknown child died (pid=", dead, "), ignoring");
            continue;
        }

        auto* slot = MetricTracer::Slot(slotIndex);
        if(!slot) {
            logger.Warn("[WFX-Master]: Corrupted slot while reaping (pid=", dead, "), ignoring");
            continue;
        }

        // Every dead worker is a CRASH, there is no normal exit in the loop
        slot->self.crashes++;

        if(WIFSIGNALED(status))
            logger.Warn("[WFX-Master]: Worker ", slotIndex, " crashed on signal ", WTERMSIG(status), " (pid=", dead,
                        ")");
        else
            logger.Warn("[WFX-Master]: Worker ", slotIndex, " died with exit code ", WEXITSTATUS(status),
                        " (pid=", dead, ")");

        // -------------------- BACKOFF CHECK --------------------
        const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        const std::uint32_t attempts = slot->self.backoffAttempts;

        if(attempts >= miscConfig.maxWorkerRestarts) {
            logger.Error("[WFX-Master]: Worker ", slotIndex, " exceeded max restart attempts (",
                         miscConfig.maxWorkerRestarts, "). Slot remains dead until server restart");
            globalState.workerPids[slotIndex] = SLOT_DEAD;
            continue;
        }

        // Compute backoff window and mark as pending revival
        const std::uint32_t base = miscConfig.workerBackoffBase;
        const std::uint32_t backoffSecs =
            (attempts >= 32) ? static_cast<std::uint32_t>(miscConfig.workerBackoffMax)
                             : std::min(base << attempts, static_cast<std::uint32_t>(miscConfig.workerBackoffMax));

        slot->self.nextRetryAt = now + backoffSecs;

        globalState.workerPids[slotIndex] = SLOT_PENDING;

        logger.Info("[WFX-Master]: Worker ", slotIndex, " marked for revival in ", backoffSecs, "s (attempt ",
                    slot->self.backoffAttempts + 1, "/", miscConfig.maxWorkerRestarts, ")");
    }
}

// Step 2: Revive slots whose backoff window has expired
static void RevivePendingWorkers(const std::string& dllDir, const std::string& logsDir, const std::string& crashLogsDir,
                                 bool useHttps, bool pinToCpu, const std::string& host, std::uint16_t port)
{
    auto& globalState = GetMasterState();
    auto& logger = GetLogger();
    auto& miscConfig = GetConfig().miscConfig;

    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));

    for(int i = 0; i < static_cast<int>(globalState.workerPids.size()); i++) {
        if(globalState.workerPids[i] != SLOT_PENDING)
            continue;

        auto* slot = MetricTracer::Slot(i);
        if(!slot) {
            logger.Warn("[WFX-Master]: Corrupted slot while reviving worker ", i, ", ignoring");
            continue;
        }

        // Backoff window not yet expired
        if(slot->self.nextRetryAt > now)
            continue;

        logger.Info("[WFX-Master]: Reviving worker ", i, " (attempt ", slot->self.backoffAttempts + 1, "/",
                    miscConfig.maxWorkerRestarts, ")");

        if(SpawnWorker(i, dllDir, logsDir, crashLogsDir, useHttps, pinToCpu, host, port)) {
            slot->self.restarts++;
            slot->self.backoffAttempts++;
        }
        else {
            logger.Error("[WFX-Master]: Failed to revive worker ", i);
            globalState.workerPids[i] = SLOT_DEAD;
        }
    }
}

// Step 3: Poll /proc/<pid>/status for each live worker
void PollWorkerMetrics()
{
    auto& globalState = GetMasterState();

    for(int i = 0; i < static_cast<int>(globalState.workerPids.size()); i++) {
        const pid_t pid = globalState.workerPids[i];
        if(pid <= 0)
            continue;

        auto* slot = MetricTracer::Slot(i);
        if(!slot)
            continue;

        // Build /proc/<pid>/status path on stack
        char path[32];
        (void)std::snprintf(path, sizeof(path), "/proc/%d/status", static_cast<int>(pid));

        const int fd = ::open(path, O_RDONLY);
        if(fd < 0)
            continue;

        // proc files report size 0, just read directly into stack buffer
        char buf[2048];
        const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
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

            const std::size_t lineLen = static_cast<std::size_t>(lineEnd - ptr);

            // VmRSS and VmSize are 6 - 7 chars + ':'
            if(lineLen > 7) {
                std::uint64_t* target = nullptr;
                std::size_t keySize = 0;

                if(std::memcmp(ptr, "VmRSS:", 6) == 0) {
                    target = &rssKb;
                    keySize = 6;
                    ++found;
                }
                else if(std::memcmp(ptr, "VmSize:", 7) == 0) {
                    target = &vmKb;
                    keySize = 7;
                    ++found;
                }

                if(target) {
                    // Skip past the key and whitespace
                    const char* valPtr = ptr + keySize;
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
    InstallSignal(SIGINT, HandleMasterSignal, "SIGINT");
    InstallSignal(SIGTERM, HandleMasterSignal, "SIGTERM");
    InstallSignal(SIGCHLD, [](int) {}, "SIGCHLD"); // Used to wake master up from 'nanosleep'

    if(!GetRandomPool().GenerateSSLKey())
        logger.Fatal("[WFX-Master]: Failed to generate SSL key");

    if(!MetricTracer::Create(static_cast<int>(osConfig.workerProcesses)))
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

    const bool pinToCpu = cfg.GetFlag(ServerFlags::PIN_TO_CPU);
    const bool useHttps = cfg.GetFlag(ServerFlags::USE_HTTPS);
    const bool ohp = cfg.GetFlag(ServerFlags::OVERRIDE_HTTPS_PORT);

    // Switch ports if we enable https and we don't want to override https default port
    const std::uint16_t port = useHttps && !ohp ? 443U : cfg.port;

    logger.Info("[WFX-Master]: Server running at ", useHttps ? "https://" : "http://", cfg.host, ':', port);
    logger.Info("[WFX-Master]: Press Ctrl+C to stop");

    // -------------------- DAEMON CONVERSION PHASE (IF ENABLED) --------------------
    if(cfg.GetFlag(ServerFlags::USE_DAEMON)) {
        const pid_t pid = fork();

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
        const int devNull = open("/dev/null", O_RDWR);
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
        info.workers = static_cast<int>(osConfig.workerProcesses);
        info.workerShutdownTimeout = static_cast<int>(osConfig.workerShutdownTimeout);
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

    // Master will have it's own logging file, why discriminate yeah?
    if(loggingConfig.enableFile)
        logger.OpenFile((logsDir + "master.log").c_str(), loggingConfig.maxFileSize, loggingConfig.maxRotations);

    // -------------------- WORKERS SPAWNING PHASE --------------------
    const std::string dllDir = buildConfig.buildDir + "/user_entry.so";

    globalState.workerPids.resize(osConfig.workerProcesses, -1);

    for(int i = 0; i < osConfig.workerProcesses; i++) {
        if(!SpawnWorker(i, dllDir, logsDir, crashLogsDir, useHttps, pinToCpu, cfg.host, port)) {
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
        const struct timespec ts {
            config.miscConfig.masterPollInterval, 0
        };
        nanosleep(&ts, nullptr);

        // Server stopped
        if(globalState.shouldStop)
            break;

        // 1. Reap dead workers and mark slots
        ReapDeadWorkers();

        // 2. Revive slots whose backoff window has expired
        RevivePendingWorkers(dllDir, logsDir, crashLogsDir, useHttps, pinToCpu, cfg.host, port);

        // 3. Poll metrics for all live workers
        PollWorkerMetrics();
    }

    logger.Info("[WFX-Master]: Signal received (INT / TERM), waiting for workers to shutdown...");

    // -------------------- SHUTDOWN PHASE --------------------
    // Wait on every worker CONCURRENTLY, inside one shared 'workerShutdownTimeout' window,-
    // -instead of one worker's full timeout at a time: N workers waited on serially could take-
    // -N times as long as configured, long enough for an external 'wfx control stop' to give up-
    // -and kill this process first, orphaning whichever workers hadn't been reached yet
    std::vector<pid_t> pending;
    for(std::uint32_t i = 0; i < static_cast<std::uint32_t>(osConfig.workerProcesses); i++) {
        const pid_t pid = globalState.workerPids[i];

        // Slot may be dead from backoff exhaustion or failed restart
        if(pid > 0)
            pending.push_back(pid);
    }

    for(std::uint32_t t = 0; !pending.empty() && t < config.osSpecificConfig.workerShutdownTimeout * 10; t++) {
        for(auto it = pending.begin(); it != pending.end();) {
            int status;
            const pid_t ret = waitpid(*it, &status, WNOHANG);

            if(ret == *it)
                it = pending.erase(it);
            else
                ++it;
        }

        if(!pending.empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Anything still here ignored SIGTERM, or was spawned too late to receive it. Either way,-
    // -force it and block until it's actually reaped, no worker is left running past this point
    for(const pid_t pid : pending) {
        logger.Warn("[WFX-Master]: Worker (pid=", pid, ") did not exit in time, sending SIGKILL");
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
    }

    // Hygiene (Not that it matters, OS would reclaim it anyways if this crashes)
    MetricTracer::Destroy();

    if(!DaemonRegistry::Delete(config.projectConfig.projectName))
        logger.Warn("[WFX-Master]: Failed to delete PID file on shutdown");

    // GG
    logger.Info("[WFX-Master]: Shutdown successfully");
    return 0;
}

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