#ifndef WFX_CLI_COMMANDS_COMMON_HPP
#define WFX_CLI_COMMANDS_COMMON_HPP

#ifdef _WIN32
    #include <Windows.h>
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

#endif // WFX_CLI_COMMANDS_COMMON_HPP