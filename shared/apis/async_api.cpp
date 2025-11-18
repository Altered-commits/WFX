#include "async_api.hpp"
#include "utils/logger/logger.hpp"
#include "http/common/http_global_state.hpp"
#include "http/connection/http_connection.hpp"

namespace WFX::Shared {

using WFX::Http::ConnectionContext;
using WFX::Utils::Logger;
using WFX::Http::GetGlobalState;

const ASYNC_API_TABLE* GetAsyncAPIV1()
{
    // 'ctx' is ConnectionContext just type erased so user doesn't DO anything
    static ASYNC_API_TABLE __GlobalAsyncAPIV1 = {
        [](void* ctx, Async::CoroutinePtr&& frame) -> AsyncPtr {  // RegisterAsyncCallback
            if(!ctx || !frame) {
                Logger::GetInstance().Warn(
                    "[AsyncApi]: 'RegisterAsyncCallback' recived null [context / coroutine frame]"
                );
                return nullptr;
            }

            auto cctx = static_cast<ConnectionContext*>(ctx);
            return (cctx->coroStack.emplace_back(std::move(frame))).get();
        },
        [](void* ctx) { // PopAsyncCallback
            if(!ctx) {
                Logger::GetInstance().Warn(
                    "[AsyncApi]: 'PopAsyncCallback' recived null context"
                );
                return;
            }

            auto cctx = static_cast<ConnectionContext*>(ctx);
            cctx->coroStack.pop_back();
        },
        [](void* ctx) { // ResumeRecentCallback
            if(!ctx) {
                Logger::GetInstance().Warn(
                    "[AsyncApi]: 'ResumeRecentCallback' recived null context"
                );
                return true;
            }

            auto cctx = static_cast<ConnectionContext*>(ctx);
            if(!cctx->coroStack.empty()) {
                auto& coro = cctx->coroStack.back();
                coro->Resume();
                return coro->IsFinished();
            }

            return true;
        },

        // vvv Async Functions vvv
        [](void* ctx, std::uint32_t delayMs) { // RegisterAsyncTimer
            auto& logger = Logger::GetInstance();

            if(!ctx) {
                logger.Warn("[AsyncApi]: 'RegisterAsyncTimer' recived null context");
                return false;
            }

            auto  cctx        = static_cast<ConnectionContext*>(ctx);
            auto* connHandler = GetGlobalState().connHandler;

            // Shouldn't happen considering we set it in core_engine.cpp
            if(!connHandler) {
                logger.Warn("[AsyncApi]: 'RegisterAsyncTimer' recived null connection handler");
                return false;
            }

            connHandler->RefreshAsyncTimer(cctx, delayMs);
            return true;
        },

        // Version
        AsyncAPIVersion::V1
    };

    return &__GlobalAsyncAPIV1;
}

} // namespace WFX::Shared