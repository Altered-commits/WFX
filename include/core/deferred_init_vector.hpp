// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_CORE_DEFERRED_INIT_VECTOR_HPP
#define WFX_INC_CORE_DEFERRED_INIT_VECTOR_HPP

#include <functional>
#include <vector>

namespace WFX::Core {

using DeferredCallback = std::function<void()>;
using DeferredVector = std::vector<DeferredCallback>;

// vvv Global Registries vvv
inline DeferredVector GlobalWFXDeferred;

// vvv Helper Functions vvv
inline void ExecuteAndEraseDeferred()
{
    // Run tasks
    for(const auto& func : GlobalWFXDeferred)
        func();

    GlobalWFXDeferred.clear();
    GlobalWFXDeferred.shrink_to_fit();
}

} // namespace WFX::Core

#endif // WFX_INC_CORE_DEFERRED_INIT_VECTOR_HPP