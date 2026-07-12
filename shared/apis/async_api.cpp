// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "async_api.hpp"
#include "utils/diagnostics/logger.hpp"
#include "http/connection/http_connection.hpp"

namespace WFX::Shared {

// Important stuff :)
static AsyncAPIDataExt1 GlobalAsyncDataExt1;

const AsyncAPIExt1* GetAsyncAPIExt1()
{
    // clang-format off
    // NOLINTNEXTLINE(readability-identifier-naming) - singleton table, treated as Global variable
    static const AsyncAPIExt1 GlobalAsyncAPIExt1 = {
        // vvv Async Functions vvv
        [](void* ctx, std::uint32_t delayMs, AsyncData asyncData) { // RegisterAsyncTimer
            auto& logger = Utils::GetLogger();

            if(!ctx) {
                logger.Warn("[AsyncAPIExt1]: 'RegisterAsyncTimer' recived null context");
                return false;
            }

            auto cctx = static_cast<Http::ClientCtx*>(ctx);
            auto* connHandler = GlobalAsyncDataExt1.connHandler;

            // Shouldn't happen considering we set it in core_engine.cpp
            if(!connHandler) {
                logger.Warn("[AsyncAPIExt1]: 'RegisterAsyncTimer' recived null connection handler");
                return false;
            }

            return connHandler->RefreshAsyncTimer(cctx, delayMs, asyncData);
        }
    };
    // clang-format on

    return &GlobalAsyncAPIExt1;
}

void InitAsyncAPIExt1(Http::HttpConnectionHandler* connHandler)
{
    GlobalAsyncDataExt1.connHandler = connHandler;
}

} // namespace WFX::Shared