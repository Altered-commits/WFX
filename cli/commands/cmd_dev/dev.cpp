#include "dev.hpp"

#include "config/config.hpp"
#include "engine/engine.hpp"
#include "utils/logger/logger.hpp"
#include "utils/filesystem/filesystem.hpp"
#include "utils/process/process.hpp"
#include "utils/backport/string.hpp"

#include <string>
#include <atomic>

namespace WFX::CLI {

using namespace WFX::Utils; // For 'Logger'
using namespace WFX::Core;  // For 'Config'

static ::std::atomic<bool> shouldStop = false;

// Common functionality used for all OS'es
void HandleUserSrcCompilation(const char* dllDir, const char* dllPath)
{
    auto& fs     = FileSystem::GetFileSystem();
    auto& proc   = ProcessUtils::GetInstance();
    auto& logger = Logger::GetInstance();
    auto& config = Config::GetInstance();

    const std::string& projName  = config.projectConfig.projectName;
    const auto&        toolchain = config.toolchainConfig;
    const std::string  srcDir    = projName + "/src";
    const std::string  objDir    = projName + "/build/objs";

    if(!fs.DirectoryExists(srcDir.c_str()))
        logger.Fatal("[WFX-Master]: Failed to locate 'src' directory inside of '", projName, "/src'.");

    if(!fs.CreateDirectory(objDir))
        logger.Fatal("[WFX-Master]: Failed to create obj dir: ", objDir, '.');

    if(!fs.CreateDirectory(dllDir))
        logger.Fatal("[WFX-Master]: Failed to create dll dir: ", dllDir, '.');

    // Prebuild fixed portions of compiler and linker commands
    const std::string compilerBase = toolchain.ccmd + " " + toolchain.cargs + " ";
    const std::string objPrefix    = toolchain.objFlag + "\"";
    const std::string dllLinkTail  = toolchain.largs + " " + toolchain.dllFlag + "\"" + dllPath + '"';

    std::string linkCmd = toolchain.lcmd + " ";

    // Recurse through src/ files
    fs.ListDirectory(srcDir, true, [&](const std::string& cppFile) {
        if(!EndsWith(cppFile.c_str(), ".cpp") &&
            !EndsWith(cppFile.c_str(), ".cxx") &&
            !EndsWith(cppFile.c_str(), ".cc")) return;

        logger.Info("[WFX-Master]: Compiling src/ file: ", cppFile);

        // Construct relative path
        std::string relPath = cppFile.substr(srcDir.size());
        if(!relPath.empty() && (relPath[0] == '/' || relPath[0] == '\\'))
            relPath.erase(0, 1);

        // Replace .cpp with .obj
        std::string objFile = objDir + "/" + relPath;
        objFile.replace(objFile.size() - 4, 4, ".obj");

        // Ensure obj subdir exists
        std::size_t slash = objFile.find_last_of("/\\");
        if(slash != std::string::npos) {
            std::string dir = objFile.substr(0, slash);
            if(!fs.DirectoryExists(dir.c_str()) && !fs.CreateDirectory(dir))
                logger.Fatal("[WFX-Master]: Failed to create obj subdirectory: ", dir);
        }

        // Construct compile command
        std::string compileCmd = compilerBase + "\"" + cppFile + "\" " + objPrefix + objFile + "\"";
        auto result = proc.RunProcess(compileCmd);
        if(result.exitCode < 0 || result.osCode == 1)
            logger.Fatal("[WFX-Master]: Compilation failed for: ", cppFile, "OS code: ", result.osCode);

        // Append obj to link command
        linkCmd += "\"" + objFile + "\" ";
    });

    // Get any libs which need to be linked to user dll
    auto libList = fs.ListDirectory("wfx/lib", false);

    // Final linking
    // If any libraries exist, give linker the path to the libraries
    if(!libList.empty()) {
#ifndef _WIN32
        // POSIX (Linux/macOS), TODO: Don't hardcode this, have some setting inside of toolchain.toml
        linkCmd += " \"-Wl,-rpath,wfx/lib\" ";
#endif
        for(const auto& libPath : libList)
            linkCmd += " \"" + libPath + "\" ";
    }

    // Final link command
    linkCmd += dllLinkTail;

    auto linkResult = proc.RunProcess(linkCmd);
    if(linkResult.exitCode < 0 || linkResult.osCode == 1)
        logger.Fatal("[WFX-Master]: Linking failed. DLL not created. Error: ", linkResult.osCode);

    logger.Info("[WFX-Master]: User project successfully compiled to ", dllDir);
}

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
#include <wait.h>
#include <signal.h>

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
#endif // _WIN32

int RunDevServer(const ::std::string& host, int port, bool noCache)
{
    auto& logger   = Logger::GetInstance();
    auto& config   = Config::GetInstance();
    auto& fs       = FileSystem::GetFileSystem();
    auto& osConfig = config.osSpecificConfig;

#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    SetUnhandledExceptionFilter(ExceptionFilter);

    WFX::Core::Engine engine{noCache};
    engine.Listen(host, port);

    while(!shouldStop)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    engine.Stop();
#else
    config.LoadCoreSettings("wfx.toml");
    config.LoadToolchainSettings("toolchain.toml");

    signal(SIGINT, HandleMasterSignal);
    signal(SIGTERM, SIG_IGN);

    // This will be used in compiling of user dll
    const std::string dllDir      = config.projectConfig.projectName + "/build/dlls/";
    const std::string dllPath     = dllDir + "user_entry.so";
    const char*       dllPathCStr = dllPath.c_str();
    const char*       dllDirCStr  = dllDir.c_str();
    
    if(noCache || !fs.FileExists(dllPathCStr))
        HandleUserSrcCompilation(dllDirCStr, dllPathCStr);
    else
        logger.Info("[WFX-Master]: File already exists, skipping user code compilation");

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

            WFX::Core::Engine engine{dllPathCStr};
            globalEnginePtr = &engine;

            signal(SIGTERM, HandleWorkerSignal);
            signal(SIGINT, SIG_IGN);  // SigTerm will kill it, SigInt handled by master
            signal(SIGPIPE, SIG_IGN); // We will handle it internally
            signal(SIGHUP, SIG_IGN);  // Terminals should not kill workers

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

    // --- Master ---
    while(!shouldStop)
        pause();

    // On Ctrl+C
    for(int i = 0; i < osConfig.workerProcesses; i++)
        waitpid(workerPids[i], nullptr, 0);
#endif // _WIN32

    logger.Info("[WFX-Master]: Shutdown successfully");
    return 0;
}

}  // namespace WFX::CLI