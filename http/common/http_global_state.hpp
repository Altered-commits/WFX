#ifndef WFX_HTTP_GLOBAL_STATE_HPP
#define WFX_HTTP_GLOBAL_STATE_HPP

#include <atomic>
#include <array>
#include <vector>

// Forward declare to not create dependency hell
namespace WFX::Core {
    class CoreEngine;
    class TemplateEngine;
}

namespace WFX::Http {

using namespace WFX::Core; // For 'CoreEngine', 'TemplateEngine'

using SSLKey = std::array<std::uint8_t, 80>;

// Forward declare 'HttpConnectionHandler'. This must be strictly set inside of core_engine.cpp
class HttpConnectionHandler;

struct WFXGlobalState {
    std::atomic<bool>      shouldStop        = false;
    CoreEngine*            enginePtr         = nullptr;
    TemplateEngine*        templateEnginePtr = nullptr;
    SSLKey                 sslKey            = { 0 };
    HttpConnectionHandler* connHandler       = nullptr;

#ifdef _WIN32
    // Nothing in Windows for now...
#else
    pid_t              workerPGID{0};
    std::vector<pid_t> workerPids;
#endif
};

inline WFXGlobalState& GetGlobalState()
{
    static WFXGlobalState instance;
    return instance;
}

} // namespace WFX::Http

#endif // WFX_HTTP_GLOBAL_STATE_HPP