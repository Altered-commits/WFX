#include "dev.hpp"

#include "config/config.hpp"
#include "engine/engine.hpp"
#include "utils/logger/logger.hpp"

#include <string>
#include <atomic>

namespace WFX::CLI {

using namespace WFX::Utils; // For 'Logger'
using namespace WFX::Core;  // For 'Config'

static ::std::atomic<bool> shouldStop = false;

#ifdef _WIN32
#include <Windows.h>
#include <Dbghelp.h>
#include <thread>
#include <chrono>

#pragma comment(lib, "Dbghelp.lib")

BOOL WINAPI ConsoleHandler(DWORD signal)
{
    if(signal == CTRL_C_EVENT) {
        Logger::GetInstance().Info("[WFX]: Shutting down...");
        shouldStop = true;
        return TRUE;
    }
    return FALSE;
}

LONG WINAPI ExceptionFilter(EXCEPTION_POINTERS* ep) {
    HANDLE file = CreateFileA("crash.dmp", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    
    if(file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpWithFullMemory, &mei, nullptr, nullptr);
        CloseHandle(file);
    }

    return EXCEPTION_EXECUTE_HANDLER;
}
#else
#include <csignal>

static ::std::vector<pid_t> workerPids;
static ::WFX::Core::Engine* globalEnginePtr = nullptr;
static pid_t                workerPGID      = 0; // Process group ID

void HandleMasterSignal(int)
{
    Logger::GetInstance().Info("[WFX-Master]: Ctrl+C pressed, shutting down workers...");
    
    shouldStop = true;

    if(workerPGID > 0)
        kill(-workerPGID, SIGTERM); // Broadcast SIGTERM to all workers
}

void HandleWorkerSignal(int)
{
    // Wake up master process
    shouldStop = true;
    
    // Stop is atomic, its safe to call it in signal handler
    if(globalEnginePtr) {
        globalEnginePtr->Stop();
        globalEnginePtr = nullptr;
    }
}

void PinWorkerToCPU(int workerIndex) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    
    int cpu = workerIndex % sysconf(_SC_NPROCESSORS_ONLN); // Round-Robin
    
    CPU_SET(cpu, &cpuset);

    if(sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0)
        Logger::GetInstance().Error("[WFX-Master]: Failed to pin worker ", workerIndex, " to CPU");

    Logger::GetInstance().Info("[WFX-Master]: Worker ", workerIndex, " pinned to CPU ", cpu);
}
#endif

int RunDevServer(const ::std::string& host, int port, bool noCache)
{
    auto& logger = Logger::GetInstance();

#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    SetUnhandledExceptionFilter(ExceptionFilter);

    WFX::Core::Engine engine{noCache};
    engine.Listen(host, port);

    while(!shouldStop)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    engine.Stop();
#else
    auto& config   = Config::GetInstance();
    auto& osConfig = config.osSpecificConfig;

    config.LoadCoreSettings("wfx.toml");
    config.LoadToolchainSettings("toolchain.toml");

    std::signal(SIGINT, HandleMasterSignal);
    std::signal(SIGTERM, SIG_IGN);

    logger.Info("[WFX-Master]: Dev server running at http://", host, ':', port);
    logger.Info("[WFX-Master]: Press Ctrl+C to stop");

    logger.SetLevelMask(WFX_LOG_INFO | WFX_LOG_WARNINGS);

    for(int i = 0; i < osConfig.workerProcesses; i++) {
        pid_t pid = fork();
        
        // --- Child Worker ---
        if(pid == 0) {
            if(i == 0)
                setpgid(0, 0);          // First worker becomes group leader
            else
                setpgid(0, workerPGID); // Join first worker's group

            WFX::Core::Engine engine{noCache};
            globalEnginePtr = &engine;

            std::signal(SIGTERM, HandleWorkerSignal);
            std::signal(SIGINT, SIG_IGN);

            PinWorkerToCPU(i);

            engine.Listen(host, port);
            return 0;
        }

        // --- Master ---
        else if(pid > 0) {
            workerPids.push_back(pid);
            if(i == 0)
                workerPGID = pid; // Store PGID for process group
            
            setpgid(pid, workerPGID);
        }
        
        else {
            logger.Error("[WFX-Master]: Failed to fork worker ", i);
            return 1;
        }
    }

    // --- master ---
    while(!shouldStop)
        pause();

    // on Ctrl+C
    for(int i = 0; i < osConfig.workerProcesses; i++)
        waitpid(workerPids[i], nullptr, 0);
#endif

    logger.Info("[WFX-Master]: Shutdown successfully");

    return 0;
}

}  // namespace WFX::CLI