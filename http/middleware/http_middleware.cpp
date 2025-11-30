#include "http_middleware.hpp"
#include "utils/logger/logger.hpp"

#include <unordered_set>

namespace WFX::Http {

// vvv Main Functions vvv
void HttpMiddleware::RegisterMiddleware(MiddlewareName name, MiddlewareEntry mw)
{
    auto&& [it, inserted] = middlewareFactories_.emplace(name, std::move(mw));
    if(!inserted) {
        auto& logger = WFX::Utils::Logger::GetInstance();
        logger.Warn("[HttpMiddleware]: Duplicate registration attempt for middleware '", name, "'. Ignoring this one");
    }
}

void HttpMiddleware::RegisterPerRouteMiddleware(const TrieNode* node, MiddlewareStack mwStack)
{
    auto& logger = WFX::Utils::Logger::GetInstance();
    if(!node) {
        logger.Warn("[HttpMiddleware]: Route node is nullptr. Ignoring this one");
        return;
    }

    auto&& [it, inserted] = middlewarePerRouteCallbacks_.emplace(node, std::move(mwStack));
    if(!inserted)
        logger.Warn("[HttpMiddleware]: Duplicate registration attempt for route node '", (void*)node, "'. Ignoring this one");
    else
        FixInternalLinks(it->second);
}

bool HttpMiddleware::ExecuteMiddleware(
    const TrieNode* node, HttpRequest& req, Response& res,
    MiddlewareType type, MiddlewareBuffer optBuf
) {
    // Initially execute the global middleware stack
    if(!ExecuteHelper(req, res, middlewareGlobalCallbacks_, type, optBuf))
        return false;

    // We assume that no node means no per-route middleware
    if(!node)
        return true;

    auto elem = middlewarePerRouteCallbacks_.find(node);
    
    // Node exists but no middleware exist, return true
    if(elem == middlewarePerRouteCallbacks_.end())
        return true;

    // Per route middleware exists, execute it
    return ExecuteHelper(req, res, elem->second, type, optBuf);
}

void HttpMiddleware::LoadMiddlewareFromConfig(MiddlewareConfigOrder order)
{
    middlewareGlobalCallbacks_.clear();

    auto& logger = WFX::Utils::Logger::GetInstance();
    std::unordered_set<std::string_view> loadedNames;

    for(const auto& nameStr : order) {
        std::string_view name = nameStr;

        // Duplicate middleware name from config
        if(!loadedNames.insert(name).second) {
            logger.Warn(
                "[HttpMiddleware]: Middleware '",
                name,
                "' is listed multiple times in config. Skipping duplicate"
            );
            continue;
        }

        auto it = middlewareFactories_.find(name);
        if(it != middlewareFactories_.end())
            middlewareGlobalCallbacks_.push_back(std::move(it->second));
        else
            logger.Warn(
                "[HttpMiddleware]: Middleware '",
                name,
                "' was listed in config but has not been registered. This may be a typo or missing registration. Skipped"
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
bool HttpMiddleware::ExecuteHelper(
    HttpRequest& req, Response& res, MiddlewareStack& stack,
    MiddlewareType type, MiddlewareBuffer optBuf
) {
    std::size_t size = stack.size();
    if(size == 0)
        return true;

    // Determine head for the selected middleware type
    std::uint16_t head = MiddlewareEntry::END;

    switch(type) {
        case MiddlewareType::SYNC:
            head = stack[0].sm ? 0 : stack[0].nextSm;
            break;

        case MiddlewareType::CHUNK_BODY:
            head = stack[0].cbm ? 0 : stack[0].nextCbm;
            break;

        case MiddlewareType::CHUNK_END:
            head = stack[0].cem ? 0 : stack[0].nextCem;
            break;
    }

    // No middleware for this type
    if(head == MiddlewareEntry::END)
        return true;

    // Walk the linked list via nextSm / nextCbm / nextCem
    std::uint16_t i = head;

    while(i != MiddlewareEntry::END) {
        MiddlewareEntry& entry = stack[i];
        MiddlewareAction action;

        // Execute
        switch(type) {
            case MiddlewareType::SYNC:       action = entry.sm(req, res);          break;
            case MiddlewareType::CHUNK_BODY: action = entry.cbm(req, res, optBuf); break;
            case MiddlewareType::CHUNK_END:  action = entry.cem(req, res);         break;
        }

        // Interpret the result
        switch(action) {
            case MiddlewareAction::CONTINUE:
                // Move to next element of this type
                if(type == MiddlewareType::SYNC)
                    i = entry.nextSm;
                else if(type == MiddlewareType::CHUNK_BODY)
                    i = entry.nextCbm;
                else
                    i = entry.nextCem;
                break;

            case MiddlewareAction::SKIP_NEXT:
                // Skip one element in this chain
                if(type == MiddlewareType::SYNC && entry.nextSm != MiddlewareEntry::END)
                    i = stack[entry.nextSm].nextSm;
                else if(type == MiddlewareType::CHUNK_BODY && entry.nextCbm != MiddlewareEntry::END)
                    i = stack[entry.nextCbm].nextCbm;
                else if(type == MiddlewareType::CHUNK_END && entry.nextCem != MiddlewareEntry::END)
                    i = stack[entry.nextCem].nextCem;
                else
                    i = MiddlewareEntry::END;
                break;

            case MiddlewareAction::BREAK:
                return false;
        }
    }

    return true;
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
        auto& s = stack[i];

        if(s.sm) {
            if(lastSm != END)
                stack[lastSm].nextSm = i;
            lastSm = i;
        }

        if(s.cbm) {
            if(lastCbm != END)
                stack[lastCbm].nextCbm = i;
            lastCbm = i;
        }

        if(s.cem) {
            if(lastCem != END)
                stack[lastCem].nextCem = i;
            lastCem = i;
        }
    }
}

} // namespace WFX::Http