#include "common.hpp"

#include "config/config.hpp"
#include "engine/core_engine.hpp"
#include "http/common/http_global_state.hpp"
#include "utils/logger/logger.hpp"
#include "utils/filesystem/filesystem.hpp"
#include "utils/process/process.hpp"
#include "utils/backport/string.hpp"

// Linux
#ifndef _WIN32
    #include <wait.h>
#endif

namespace WFX::CLI {

using namespace WFX::Utils; // For ...
using namespace WFX::Core;  // For 'Config'

// vvv Common Stuff vvv
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
        if(result.exitCode != 0)
            logger.Fatal("[WFX-Master]: Compilation failed for: ", cppFile, ". OS code: ", result.osCode);

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
    if(linkResult.exitCode != 0)
        logger.Fatal("[WFX-Master]: Linking failed. DLL not created. OS code: ", linkResult.osCode);

    logger.Info("[WFX-Master]: User project successfully compiled to ", dllDir);
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