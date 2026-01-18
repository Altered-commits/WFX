#include "async_api.hpp"
#include "utils/logger/logger.hpp"
#include "http/connection/http_connection.hpp"

namespace WFX::Shared {

using WFX::Http::ConnectionContext;
using WFX::Utils::Logger;

// Important stuff :)
static AsyncAPIDataV1 __GlobalAsyncDataV1;

const ASYNC_API_TABLE* GetAsyncAPIV1()
{
    // 'ctx' is ConnectionContext just type erased so user doesn't DO anything
    static ASYNC_API_TABLE __GlobalAsyncAPIV1 = {
        // vvv Async Functions vvv
        [](void* ctx, std::uint32_t delayMs) { // RegisterAsyncTimer
            auto& logger = Logger::GetInstance();

            if(!ctx) {
                logger.Warn("[AsyncApi]: 'RegisterAsyncTimer' recived null context");
                return false;
            }

            auto  cctx        = static_cast<ConnectionContext*>(ctx);
            auto* connHandler = __GlobalAsyncDataV1.connHandler;

            // Shouldn't happen considering we set it in core_engine.cpp
            if(!connHandler) {
                logger.Warn("[AsyncApi]: 'RegisterAsyncTimer' recived null connection handler");
                return false;
            }

            return connHandler->RefreshAsyncTimer(cctx, delayMs);
        },

        // Version
        AsyncAPIVersion::V1
    };

    return &__GlobalAsyncAPIV1;
}

void InitAsyncAPIV1(HttpConnectionHandler* connHandler)
{
    __GlobalAsyncDataV1.connHandler = connHandler;
}

} // namespace WFX::Shared