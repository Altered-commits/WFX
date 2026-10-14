// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_APP_HPP
#define WFX_INC_WFX_APP_HPP

// -----------------------------------------------------------------------
// wfx/app.hpp
// Application lifecycle hooks.
//
// Provides:
//   WFX_CONSTRUCTOR(...)   : run code once the engine has loaded your app
//   WFX_API_ENTRY()        : the entry point the engine calls to load it.
//                            Expand it exactly once, in one .cpp (wfx new
//                            puts it in src/api_entry.cpp):
//
//   #include <wfx/app.hpp>
//
//   WFX_API_ENTRY()
// -----------------------------------------------------------------------

#include "core/constructor.hpp"
#include "core/core.hpp"
#include "core/deferred_init_vector.hpp"
#include "shared/utils/compiler_macro.hpp"
#include "wfx/types.hpp"

namespace WFX::Core {

// Hands user code the engine's API table, then runs every registration (routes, middleware,
// WFX_CONSTRUCTOR) queued until the table existed. Once per load, a repeat call does nothing
inline void InstallMasterApi(const Shared::MasterAPITable* api)
{
    static bool GlobalInstalled = false;
    if(GlobalInstalled || !api)
        return;

    SetMasterApi(api);
    ExecuteAndEraseDeferred();
    GlobalInstalled = true;
}

} // namespace WFX::Core

// extern "C" keeps the name unmangled, it is what the engine looks up with dlsym
#define WFX_API_ENTRY()                                                                                                \
    extern "C" WFX_EXPORT void RegisterMasterAPI(const WFX::Shared::MasterAPITable* api)                               \
    {                                                                                                                  \
        WFX::Core::InstallMasterApi(api);                                                                              \
    }

#endif // WFX_INC_WFX_APP_HPP
