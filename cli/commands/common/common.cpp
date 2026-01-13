#include "common.hpp"

#include "config/config.hpp"
#include "engine/core_engine.hpp"
#include "http/common/http_global_state.hpp"
#include "utils/logger/logger.hpp"
#include "utils/fileops/filesystem.hpp"
#include "utils/process/process.hpp"
#include "utils/backport/string.hpp"

// Linux
#ifdef __linux__
    #include <wait.h>
#endif

namespace WFX::CLI {

using namespace WFX::Utils; // For ...
using namespace WFX::Core;  // For 'Config'

// vvv Common Stuff vvv
void HandleBuildDirectory()
{
    auto& logger = Logger::GetInstance();
    auto& config = Config::GetInstance();

    auto& projectConfig = config.projectConfig;
    auto& buildConfig   = config.buildConfig;
    
    // Short circuit if build/ directory already exists
    // Any unwanted changes inside of build/ is solely users fault
    if(FileSystem::DirectoryExists(buildConfig.buildDir.c_str()))
        return;

    std::string intDir   = projectConfig.projectName + "/intermediate/dynamic";
    std::string intDummy = intDir + "/_d.cpp";

    // If intermediate directory doesn't exist, handle its creation (to ensure cmake succeeds)
    if(!FileSystem::DirectoryExists(intDir.c_str())) {
        if(!FileSystem::CreateDirectory(std::move(intDir)))
            logger.Fatal(
                "[WFX-Master]: Failed to create intermediate directory (needed for CMake to work)"
            );

        if(!FileSystem::CreateFile(intDummy.c_str())) {
            // Cleanup the intermediate/ directory
            if(!FileSystem::DeleteDirectory((projectConfig.projectName + "/intermediate").c_str()))
                logger.Error("[WFX-Master]: Failed to delete intermediate/ (incoming 'Fatal' error)");

            logger.Fatal(
                "[WFX-Master]: Failed to create intermediate dummy (needed for CMake to work)"
            );
        }
    }

    // Now do the fancy cmake command and run it
    std::string cmakeInitCommand = "cmake -DCMAKE_BUILD_TYPE=" + buildConfig.buildType
                                    + " -S " + projectConfig.projectName
                                    + " -B " + buildConfig.buildDir
                                    + " -G \"" + buildConfig.buildGenerator + '"';

    auto initResult = ProcessUtils::RunProcess(cmakeInitCommand);
    if(initResult.exitCode != 0)
        logger.Fatal("[WFX-Master]: CMake init failed. Exit code: ", initResult.exitCode);

    logger.Info("[WFX-Master]: CMake initialized successfully");
}

void HandleUserCxxCompilation(CxxCompilationOption opt)
{
    /*
     * Handles both src and template cxx compilation with one single build directory
     */
    auto& logger      = Logger::GetInstance();
    auto& buildConfig = Config::GetInstance().buildConfig;

    std::string cmakeBuildCommand = "cmake --build " + buildConfig.buildDir;

    switch(opt) {    
        case CxxCompilationOption::SOURCE_ONLY:
            cmakeBuildCommand += " --target user_entry";
            break;
        case CxxCompilationOption::TEMPLATES_ONLY:
            cmakeBuildCommand += " --target user_templates";
            break;
        // Ignore everything else
    }

    auto buildResult = ProcessUtils::RunProcess(cmakeBuildCommand);
    if(buildResult.exitCode != 0)
        logger.Fatal("[WFX-Master]: CMake build failed. Exit code: ", buildResult.exitCode);

    logger.Info("[WFX-Master]: User project successfully compiled");
}

// vvv OS Specific Stuff vvv
#ifdef _WIN32
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
void HandleMasterSignal(int)
{
    auto& globalState = GetGlobalState();
    globalState.shouldStop = true;
    
    Logger::GetInstance().Info("[WFX-Master]: Ctrl+C pressed, shutting down workers...");

    if(globalState.workerPGID > 0)
        kill(-globalState.workerPGID, SIGTERM); // Broadcast SIGTERM to all workers
}

void HandleWorkerSignal(int)
{
    auto& globalState = GetGlobalState();
    globalState.shouldStop = true;
    
    // Stop is atomic, its safe to call it in signal handler
    if(globalState.enginePtr) {
        globalState.enginePtr->Stop();
        globalState.enginePtr = nullptr;
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

} // namespace WFX::CLI
