#include "http_middleware.hpp"
#include "http/connection/http_connection.hpp"
#include "http/response.hpp"
#include "shared/apis/http_api.hpp"
#include "utils/logger/logger.hpp"
#include <unordered_set>

namespace WFX::Http {

// vvv Main Functions vvv
void HttpMiddleware::RegisterMiddleware(MiddlewareName name, HttpMiddlewareType mw)
{
    auto&& [it, inserted] = middlewareFactories_.emplace(name, std::move(mw));
    if(!inserted) {
        auto& logger = WFX::Utils::Logger::GetInstance();
        logger.Fatal("[HttpMiddleware]: Duplicate registration attempt for middleware '", name, '\'');
    }
}

void HttpMiddleware::RegisterPerRouteMiddleware(const TrieNode* node, HttpMiddlewareStack mwStack)
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
    const TrieNode* node, HttpRequest& req, Response res, ConnectionContext* ctx
) {
    if(ctx->trackAsync.GetMLevel() == MiddlewareLevel::GLOBAL) {
        // Initially execute the global middleware stack
        auto [success, task] = ExecuteHelper(req, res, middlewareGlobalCallbacks_, ctx);
        if(!success)
            return {false, std::move(task)};

        // Reset the context to prepare for per route if it exists
        ctx->trackAsync.SetMIndex(0);
        ctx->trackAsync.SetMLevel(MiddlewareLevel::PER_ROUTE);
    }

    // We assume that no node means no per-route middleware
    if(!node)
        return {true, AsyncMiddlewareAction{nullptr}};

    auto elem = middlewarePerRouteCallbacks_.find(node);
    
    // Node exists but no middleware exist, return true
    if(elem == middlewarePerRouteCallbacks_.end())
        return {true, AsyncMiddlewareAction{nullptr}};

    // Per route middleware exists, execute it
    return ExecuteHelper(req, res, elem->second, ctx);
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
    HttpRequest& req, Response res, HttpMiddlewareStack& stack, ConnectionContext* ctx
) {
    std::size_t stackSize = stack.size();
    if(stackSize == 0)
        return {true, AsyncMiddlewareAction{nullptr}};

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
                return {false, AsyncMiddlewareAction{nullptr}};
        }
    }

    for(std::uint16_t i = mIndex; i < stackSize; i++) {
        HttpMiddlewareType& entry = stack[i];

        // Execute
        auto [action, task] = ExecuteFunction(ctx, entry, req, res);

        // Async function, so we need to store the next valid middleware index because this async function-
        // -will run in scheduler seperate from this middleware chain, after it completes we need to invoke-
        // -the next valid scheduler
        if(task) {
            trackAsync.SetMIndex(i + 1);
            return {false, std::move(task)};
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
                return {false, AsyncMiddlewareAction{nullptr}};
        }
    }

    return {true, AsyncMiddlewareAction{nullptr}};
}

MiddlewareFunctionResult HttpMiddleware::ExecuteFunction(
    ConnectionContext* ctx, HttpMiddlewareType& entry, HttpRequest& req, Response res
) {
    auto& logger = WFX::Utils::Logger::GetInstance();

    // Sanity check, this shouldn't happen if user properly set handled types
    if(std::holds_alternative<std::monostate>(entry)) {
        logger.Warn(
            "[HttpMiddleware]: Found empty handler while executing middleware."
            " Corrupted state"
        );
        return {MiddlewareAction::CONTINUE, AsyncMiddlewareAction{nullptr}};
    }

    // Check if its a sync function, it directly returns value
    if(auto* sync = std::get_if<SyncMiddlewareType>(&entry))
        return {(*sync)(req, res), AsyncMiddlewareAction{nullptr}};

    // For async function, the return value is stored in ctx 'mAction'
    auto* httpApi = WFX::Shared::GetHttpAPIV1();
    auto& async   = std::get<AsyncMiddlewareType>(entry);

    // Set context (type erased) at http api side before calling async callback
    httpApi->SetGlobalPtrData(static_cast<void*>(ctx));

    auto task = async(req, res);
    task.Resume();

    // Reset to remove dangling references
    httpApi->SetGlobalPtrData(nullptr);

    // Check if we are done with async, if not return ptr
    if(!task.IsFinished())
        return {MiddlewareAction{}, std::move(task)};

    // We were able to finish async in sync, check for errors
    // Right now we will crash the engine on errors, later we will actually handle them
    auto [action, err] = task.GetResult();
    if(err != Async::Status::NONE)
        logger.Fatal("[HttpMiddleware]: Coroutine completed with errors");

    return {action, AsyncMiddlewareAction{nullptr}};
}

} // namespace WFX::Http