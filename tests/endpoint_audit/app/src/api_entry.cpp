// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include <core/deferred_init_vector.hpp>
#include <core/core.hpp>
#include <shared/utils/compiler_macro.hpp>

// To prevent name mangling 
extern "C" {
    WFX_EXPORT void RegisterMasterAPI(const WFX::Shared::MasterAPITable* api)
    {
        static bool registered = false;
        if(registered)
            return;

        if(api) {
            WFX::Core::SetMasterApi(api);
            WFX::Core::ExecuteAndEraseDeferred();

            registered = true;
        }
    }
}