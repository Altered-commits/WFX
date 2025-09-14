#include "dev.hpp"

#include "engine/engine.hpp"
#include "utils/logger/logger.hpp"

#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

namespace WFX::CLI {

// For 'Logger'
using namespace WFX::Utils;

static std::atomic<bool> shouldStop = false;

#ifdef _WIN32
#include <Windows.h>
#include <Dbghelp.h>

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
void SignalHandler(int signal)
{
    if(signal == SIGINT) {
        Logger::GetInstance().Info("[WFX]: Shutting down...");
        shouldStop = true;
    }
}
#endif

int RunDevServer(const std::string& host, int port, bool noCache)
{
    Logger::GetInstance().SetLevelMask(WFX_LOG_INFO | WFX_LOG_WARNINGS);

#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    SetUnhandledExceptionFilter(ExceptionFilter);
#else
    std::signal(SIGINT, SignalHandler);
#endif
    WFX::Core::Engine engine{noCache};
    engine.Listen(host, port);
    
    while(!shouldStop)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    engine.Stop();
    return 0;
}

}  // namespace WFX::CLI