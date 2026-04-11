#include "http_middleware.hpp"
#include "engine/core_engine.hpp"
#include "http/request.hpp"  // |
#include "http/response.hpp" // | -> User side implementations
#include "shared/apis/http_api.hpp"
#include "utils/logger/logger.hpp"
#include <unordered_set>

namespace WFX::Http {

using namespace WFX::Shared; // For every single abi type

// vvv Main Functions vvv
void HttpMiddleware::RegisterMiddleware(std::string_view name, MwCallback mw)
{
    auto&& [it, inserted] = middlewareFactories_.emplace(name, mw);
    if(!inserted) {
        auto& logger = WFX::Utils::Logger::GetInstance();
        logger.Fatal("[HttpMiddleware]: Duplicate registration attempt for middleware '", name, '\'');
    }
}

void HttpMiddleware::RegisterPerRouteMiddleware(const TrieNode* node, MiddlewareStack mwStack)
{
    auto& logger = WFX::Utils::Logger::GetInstance();
    if(!node)
        logger.Fatal(
            "[HttpMiddleware]: Route node is nullptr for per-route middleware registeration"
        );

    auto&& [it, inserted] = middlewarePerRouteCallbacks_.emplace(node, std::move(mwStack));
    if(!inserted)
        logger.Fatal(
            "[HttpMiddleware]: Duplicate registration attempt for route node '", (void*)node, '\''
        );
}

MiddlewareResult HttpMiddleware::ExecuteMiddleware(
    ConnectionContext* ctx, const TrieNode* node, Request req, Response res
) {
    if(ctx->trackAsync.GetMLevel() == MiddlewareLevel::GLOBAL) {
        // Initially execute the global middleware stack
        auto mwRes = ExecuteHelper(ctx, req, res, middlewareGlobalCallbacks_);
        if(!mwRes.success)
            return mwRes;

        // Reset the context to prepare for per route if it exists
        ctx->trackAsync.SetMIndex(0);
        ctx->trackAsync.SetMLevel(MiddlewareLevel::PER_ROUTE);
    }

    // We assume that no node means no per-route middleware
    if(!node)
        return {true, false};

    auto elem = middlewarePerRouteCallbacks_.find(node);
    
    // Node exists but no middleware exist, return true
    if(elem == middlewarePerRouteCallbacks_.end())
        return {true, false};

    // Per route middleware exists, execute it
    return ExecuteHelper(ctx, req, res, elem->second);
}

void HttpMiddleware::LoadMiddlewareFromConfig(MiddlewareConfigOrder order)
{
    middlewareGlobalCallbacks_.clear();

    auto& logger = WFX::Utils::Logger::GetInstance();
    std::unordered_set<std::string_view> loadedNames;

    for(const auto& nameStr : order) {
        std::string_view name = nameStr;

        // Duplicate middleware name from config
        if(!loadedNames.insert(name).second)
            logger.Fatal(
                "[HttpMiddleware]: Middleware '",
                name,
                "' is listed multiple times in config"
            );

        auto it = middlewareFactories_.find(name);
        if(it != middlewareFactories_.end())
            middlewareGlobalCallbacks_.push_back(std::move(it->second));
        else
            logger.Fatal(
                "[HttpMiddleware]: Middleware '",
                name,
                "' was listed in config but has not been registered."
                " This may be a typo or missing registration"
            );
    }
}

void HttpMiddleware::DiscardFactoryMap()
{
    middlewareFactories_.clear();
    middlewareFactories_.rehash(0); // Force deallocation of internal buckets
}

// vvv Helper Functions vvv
MiddlewareResult HttpMiddleware::ExecuteHelper(
    ConnectionContext* ctx, Request req, Response res, MiddlewareStack& stack
) {
    std::size_t stackSize = stack.size();
    if(stackSize == 0)
        return {true, false};

    auto& trackAsync = ctx->trackAsync;
    auto mIndex = trackAsync.GetMIndex();

    // Check if we already executed this beforehand, we just need to continue from where we left off
    if(mIndex > 0) {
        // But before we jump to executing middleware, we need to consider previous async middlewares-
        // -return value
        auto lastAction = *trackAsync.GetMAction();
        switch(lastAction) {
            case MiddlewareAction::CONTINUE:
                break; // Proceed normally

            case MiddlewareAction::SKIP_NEXT:
                mIndex++;
                break;

            case MiddlewareAction::BREAK:
                return {false, false};
        }
    }

    for(std::uint16_t i = mIndex; i < stackSize; i++) {
        auto& mw = stack[i];

        // Execute
        auto [action, isAsync] = ExecuteFunction(ctx, req, res, mw);

        // Async function, so we need to store the next valid middleware index because this async function-
        // -will run in scheduler seperate from this middleware chain, after it completes we need to invoke-
        // -the next valid scheduler
        if(isAsync) {
            trackAsync.SetMIndex(i + 1);
            return {false, true};
        }

        // Interpret the result
        switch(action) {
            case MiddlewareAction::CONTINUE:
                break;

            case MiddlewareAction::SKIP_NEXT:
                // Skip one element in this chain
                ++i;
                break;

            case MiddlewareAction::BREAK:
                return {false, false};
        }
    }

    return {true, false};
}

MiddlewareFunctionResult HttpMiddleware::ExecuteFunction(
    ConnectionContext* ctx, Request req, Response res, MwCallback mw
) {
    auto& logger = WFX::Utils::Logger::GetInstance();

    // Check if its a sync function, it directly returns value
    if(mw.kind == CallbackKind::SYNC)
        return {mw.sync(req, res), false};

    // Async path, call through C boundary
    auto* httpApi = WFX::Shared::GetHttpAPIV1();
    httpApi->SetGlobalPtrData(static_cast<void*>(ctx));

    // Engine passes its own callback into the async middleware
    // The middleware coroutine's 'final_suspend' will fire this when done
    mw.async(req, res, WFX::Core::CoreEngine::OnCoroutineComplete, ctx);

    httpApi->SetGlobalPtrData(nullptr);

    return { MiddlewareAction::CONTINUE, true };
}

} // namespace WFX::Http