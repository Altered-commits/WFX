#include "async_api.hpp"
#include "utils/diagnostics/logger.hpp"
#include "http/connection/http_connection.hpp"

namespace WFX::Shared {

using WFX::Http::ConnectionContext;

// Important stuff :)
static AsyncAPIDataExt1 __GlobalAsyncDataExt1;

const ASYNC_API_EXT1* GetAsyncAPIExt1()
{
    // clang-format off
    static ASYNC_API_EXT1 __GlobalAsyncAPIExt1 = {
        // vvv Async Functions vvv
        [](void* ctx, std::uint32_t delayMs, AsyncData asyncData) { // RegisterAsyncTimer
            auto& logger = Utils::GetLogger();

            if(!ctx) {
                logger.Warn("[AsyncApiExt1]: 'RegisterAsyncTimer' recived null context");
                return false;
            }

            auto cctx = static_cast<ConnectionContext*>(ctx);
            auto* connHandler = __GlobalAsyncDataExt1.connHandler;

            // Shouldn't happen considering we set it in core_engine.cpp
            if(!connHandler) {
                logger.Warn("[AsyncApiExt1]: 'RegisterAsyncTimer' recived null connection handler");
                return false;
            }

            return connHandler->RefreshAsyncTimer(cctx, delayMs, asyncData);
        }
    };
    // clang-format on

    return &__GlobalAsyncAPIExt1;
}

void InitAsyncAPIExt1(Http::HttpConnectionHandler* connHandler)
{
    __GlobalAsyncDataExt1.connHandler = connHandler;
}

} // namespace WFX::Shared