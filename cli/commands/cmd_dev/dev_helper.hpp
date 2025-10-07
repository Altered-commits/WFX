#ifndef WFX_CLI_COMMANDS_DEV_HELPER_HPP
#define WFX_CLI_COMMANDS_DEV_HELPER_HPP

#include "config/config.hpp"
#include "http/common/http_global_state.hpp"
#include "utils/logger/logger.hpp"
#include "utils/filesystem/filesystem.hpp"
#include "utils/process/process.hpp"
#include "utils/backport/string.hpp"

#ifdef _WIN32
    #include <Windows.h>
    #include <Dbghelp.h>
    #include <thread>
    #include <chrono>
    #pragma comment(lib, "Dbghelp.lib")
#else
    #include <wait.h>
    #include <signal.h>
#endif

namespace WFX::CLI {

// vvv Common Stuff vvv
void HandleUserSrcCompilation(const char* dllDir, const char* dllPath);

// vvv OS Specific Stuff vvv
#ifdef _WIN32
BOOL WINAPI ConsoleHandler(DWORD signal);
LONG WINAPI ExceptionFilter(EXCEPTION_POINTERS* ep);
#else
void HandleMasterSignal(int);
void HandleWorkerSignal(int);
void PinWorkerToCPU(int workerIndex);
#endif

} // namespace WFX::CLI

#endif // WFX_CLI_COMMANDS_DEV_HELPER_HPP