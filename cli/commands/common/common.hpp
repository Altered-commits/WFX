#ifndef WFX_CLI_COMMANDS_COMMON_HPP
#define WFX_CLI_COMMANDS_COMMON_HPP

#ifdef _WIN32
    #include <Windows.h>
#endif

#include <cstdint>

namespace WFX::CLI {

enum class CxxCompilationOption: std::uint8_t {
    SOURCE_ONLY,
    TEMPLATES_ONLY,
    ALL,
};

// vvv Common Stuff vvv
void HandleBuildDirectory();
void HandleUserCxxCompilation(CxxCompilationOption = CxxCompilationOption::ALL);

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