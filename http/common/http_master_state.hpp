// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_HTTP_GLOBAL_STATE_HPP
#define WFX_HTTP_GLOBAL_STATE_HPP

#include <atomic>
#include <array>
#include <vector>

/*
 * Master process lifecycle state. Contains coordination data for the master <-> worker process group
 */

// Forward declare to not create dependency hell
namespace WFX::Core {
class CoreEngine;
}

namespace WFX::Http {

struct WFXMasterState {
    Core::CoreEngine* enginePtr = nullptr;
    std::atomic<bool> shouldStop = false;

#ifdef _WIN32
    // Nothing in Windows for now...
#else
    pid_t workerPGID{0};
    std::vector<pid_t> workerPids;
#endif
};

// Free function declaration (defined in 'http_master_state.cpp')
WFXMasterState& GetMasterState() noexcept;

} // namespace WFX::Http

#endif // WFX_HTTP_GLOBAL_STATE_HPP