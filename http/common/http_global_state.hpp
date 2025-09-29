#ifndef WFX_HTTP_GLOBAL_STATE_HPP
#define WFX_HTTP_GLOBAL_STATE_HPP

#include "engine/engine.hpp"
#include <atomic>
#include <array>
#include <vector>

namespace WFX::Http {

using namespace WFX::Core; // For 'Engine'

using SSLKey = std::array<std::uint8_t, 80>;

struct WFXGlobalState {
    std::atomic<bool> shouldStop{false};
    Engine*           enginePtr{nullptr};
    SSLKey            sslKey{0};

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