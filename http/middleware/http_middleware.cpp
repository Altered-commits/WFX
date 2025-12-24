#include "http_middleware.hpp"
#include "http/connection/http_connection.hpp"
#include "shared/apis/http_api.hpp"
#include "utils/logger/logger.hpp"

#include <unordered_set>

namespace WFX::Http {

// vvv Main Functions vvv
void HttpMiddleware::RegisterMiddleware(MiddlewareName name, MiddlewareEntry mw)
{
    auto&& [it, inserted] = middlewareFactories_.emplace(name, std::move(mw));
    if(!inserted) {
        auto& logger = WFX::Utils::Logger::GetInstance();
        logger.Fatal("[HttpMiddleware]: Duplicate registration attempt for middleware '", name, '\'');
    }
}

void HttpMiddleware::RegisterPerRouteMiddleware(const TrieNode* node, MiddlewareStack mwStack)
{
    auto& logger = WFX::Utils::Logger::GetInstance();
    if(!node)
        logger.Fatal("[HttpMiddleware]: Route node is nullptr for per-route middleware registeration");

    auto&& [it, inserted] = middlewarePerRouteCallbacks_.emplace(node, std::move(mwStack));
    if(!inserted)
        logger.Fatal("[HttpMiddleware]: Duplicate registration attempt for route node '", (void*)node, '\'');
    else
        FixInternalLinks(it->second);
}

MiddlewareResult HttpMiddleware::ExecuteMiddleware(
    const TrieNode* node, HttpRequest& req, Response& res,
    ConnectionContext* ctx, MiddlewareBuffer optBuf
) {
    if(ctx->trackAsync.GetMLevel() == MiddlewareLevel::GLOBAL) {
        // Initially execute the global middleware stack
        auto [success, ptr] = ExecuteHelper(req, res, middlewareGlobalCallbacks_, ctx, optBuf);
        if(!success)
            return {false, ptr};

        // Reset the context to prepare for per route if it exists
        ctx->trackAsync.SetMIndex(0);
        ctx->trackAsync.SetMLevel(MiddlewareLevel::PER_ROUTE);
    }

    // We assume that no node means no per-route middleware
    if(!node)
        return {true, nullptr};

    auto elem = middlewarePerRouteCallbacks_.find(node);
    
    // Node exists but no middleware exist, return true
    if(elem == middlewarePerRouteCallbacks_.end())
        return {true, nullptr};

    // Per route middleware exists, execute it
    return ExecuteHelper(req, res, elem->second, ctx, optBuf);
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
                "' was listed in config but has not been registered. This may be a typo or missing registration"
            );
    }

    FixInternalLinks(middlewareGlobalCallbacks_);
}

void HttpMiddleware::DiscardFactoryMap()
{
    middlewareFactories_.clear();
    middlewareFactories_.rehash(0); // Force deallocation of internal buckets
}

// vvv Helper Functions vvv
MiddlewareResult HttpMiddleware::ExecuteHelper(
    HttpRequest& req, Response& res, MiddlewareStack& stack,
    ConnectionContext* ctx, MiddlewareBuffer optBuf
) {
    std::size_t size = stack.size();
    if(size == 0)
        return {true, nullptr};

    std::uint16_t head = MiddlewareEntry::END;

    auto& trackAsync = ctx->trackAsync;
    auto mType  = trackAsync.GetMType();
    auto mIndex = trackAsync.GetMIndex();

    // Select the correct 'next' pointer for this middleware type
    auto next = MiddlewareEntryNext(mType);

    // Check if we already executed this beforehand, we just need to continue from where we left off
    if(mIndex > 0) {
        head = mIndex;

        // But before we jump to executing middleware, we need to consider previous async middlewares-
        // -return value
        auto lastAction = *trackAsync.GetMAction();
        switch(lastAction) {
            case MiddlewareAction::CONTINUE:
                break; // Proceed normally

            case MiddlewareAction::SKIP_NEXT:
                if(head != MiddlewareEntry::END)
                    head = stack[head].*next;
                break;

            case MiddlewareAction::BREAK:
                return {false, nullptr};
        }

        goto __ContinueMiddleware;
    }

    // Fresh start, find first usable middleware
    head = (stack[0].handled & static_cast<std::uint8_t>(mType)) ? 0 : (stack[0].*next);

    // No middleware for this type
    if(head == MiddlewareEntry::END)
        return {true, nullptr};

__ContinueMiddleware:
    // Walk the linked list via nextSm / nextCbm / nextCem
    std::uint16_t i = head;

    while(i != MiddlewareEntry::END) {
        MiddlewareEntry& entry = stack[i];
        MiddlewareMeta   meta  = {mType, optBuf};

        // Execute
        auto [action, asyncPtr] = ExecuteFunction(ctx, entry, req, res, meta);

        // Async function, so we need to store the next valid middleware index because this async function-
        // -will run in scheduler seperate from this middleware chain, after it completes we need to invoke-
        // -the next valid scheduler
        if(asyncPtr) {
            trackAsync.SetMIndex(entry.*next);
            return {false, asyncPtr};
        }

        // Interpret the result
        switch(action) {
            case MiddlewareAction::CONTINUE:
                // Move to next element of this type
                i = entry.*next;
                break;

            case MiddlewareAction::SKIP_NEXT:
                // Skip one element in this chain
                if(entry.*next != MiddlewareEntry::END)
                    i = stack[entry.*next].*next;
                else
                    i = MiddlewareEntry::END;
                break;

            case MiddlewareAction::BREAK:
                return {false, nullptr};
        }
    }

    return {true, nullptr};
}

MiddlewareFunctionResult HttpMiddleware::ExecuteFunction(
    ConnectionContext* ctx, MiddlewareEntry& entry,
    HttpRequest& req, Response& res, MiddlewareMeta meta
) {
    auto& logger = WFX::Utils::Logger::GetInstance();

    // Sanity check, this shouldn't happen if user properly set handled types
    if(std::holds_alternative<std::monostate>(entry.mw)) {
        logger.Warn(
            "[HttpMiddleware]: Found empty handler while executing middleware for type: ", (int)meta.type,
            " Perhaps you forgot to set middleware handling type?"
        );
        return {MiddlewareAction::CONTINUE, nullptr};
    }

    // Check if its a sync function, it directly returns value
    if(auto* sync = std::get_if<SyncMiddlewareType>(&entry.mw))
        return {(*sync)(req, res, meta), nullptr};

    // For async function, the return value is stored in AsyncPtr '__Action' value
    // Yeah ik, weird
    auto* httpApi = WFX::Shared::GetHttpAPIV1();
    auto& async   = std::get<AsyncMiddlewareType>(entry.mw);

    // Set context (type erased) at http api side before calling async callback
    httpApi->SetGlobalPtrData(static_cast<void*>(ctx));

    auto ptr = async(req, res, meta);
    if(!ptr)
        logger.Fatal(
            "[HttpMiddleware]: Null coroutine detected in executed async middleware. Type: ", (int)meta.type
        );

    ptr->SetReturnPtr(static_cast<void*>(ctx->trackAsync.GetMAction()));
    ptr->Resume();

    // Reset to remove dangling references
    httpApi->SetGlobalPtrData(nullptr);

    // Check if we are done with async, if not return ptr
    if(!ptr->IsFinished()) {
        ptr->SetReturnPtr(nullptr);
        return {{}, ptr};
    }

    // We were able to finish async in sync, coroutine stack should only have 1 element, itself
    // If not, big no no
    if(ctx->coroStack.size() > 1)
        logger.Fatal(
            "[HttpMiddleware]: Coroutine stack imbalance detected after async middleware execution."
            " Type: ", (int)meta.type
        );

    // Clear out the coroutine stack for future middlewares
    ctx->coroStack.clear();

    return {*ctx->trackAsync.GetMAction(), nullptr};
}

void HttpMiddleware::FixInternalLinks(MiddlewareStack& stack)
{
    constexpr std::uint16_t END = MiddlewareEntry::END;
    std::uint16_t size = stack.size();

    // Sanity checks
    if(size == 0)
        return;

    std::uint16_t lastSm  = END;
    std::uint16_t lastCbm = END;
    std::uint16_t lastCem = END;

    for(std::uint16_t i = 0; i < size; ++i) {
        auto h = stack[i].handled;

        // LINEAR
        if(h & static_cast<std::uint8_t>(MiddlewareType::LINEAR)) {
            if(lastSm != END)
                stack[lastSm].nextSm = i;
            lastSm = i;
        }

        // STREAM_CHUNK
        if(h & static_cast<std::uint8_t>(MiddlewareType::STREAM_CHUNK)) {
            if(lastCbm != END)
                stack[lastCbm].nextCbm = i;
            lastCbm = i;
        }

        // STREAM_END
        if(h & static_cast<std::uint8_t>(MiddlewareType::STREAM_END)) {
            if(lastCem != END)
                stack[lastCem].nextCem = i;
            lastCem = i;
        }
    }
}

} // namespace WFX::Http