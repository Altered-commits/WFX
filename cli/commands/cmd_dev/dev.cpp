#include "dev.hpp"

#include "engine/Engine.hpp"
#include "utils/logger/logger.hpp"

#include <csignal>
#include <atomic>
#include <iostream>
#include <thread>
#include <chrono>

namespace WFX::CLI {

// For 'Logger'
using namespace WFX::Utils;

static std::atomic<bool> shouldStop = false;

#ifdef _WIN32
#include <Windows.h>
BOOL WINAPI ConsoleHandler(DWORD signal)
{
    if(signal == CTRL_C_EVENT) {
        Logger::GetInstance().Info("[WFX]: Shutting down...");
        shouldStop = true;
        return TRUE;
    }
    return FALSE;
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

int RunDevServer(const std::string& host, int port)
{
#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
#else
    std::signal(SIGINT, SignalHandler);
#endif
    Logger::GetInstance().SetLevelMask(WFX_LOG_INFO | WFX_LOG_WARNINGS);

    WFX::Core::Engine engine;
    engine.Listen(host, port);

    Logger::GetInstance().Info("[WFX]: Dev server running at http://", host, ':', port);
    Logger::GetInstance().Info("[WFX]: Press Ctrl+C to stop.");

    // Logger::GetInstance().SetLevelMask(WFX_LOG_NONE);
    
    while(!shouldStop)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    engine.Stop();
    return 0;
}

}  // namespace WFX::CLI