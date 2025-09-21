#include "http_middleware.hpp"
#include "utils/logger/logger.hpp"

#include <unordered_set>

namespace WFX::Http {

HttpMiddleware& HttpMiddleware::GetInstance()
{
    static HttpMiddleware middleware;
    return middleware;
}

void HttpMiddleware::RegisterMiddleware(MiddlewareName name, MiddlewareCallbackType mw)
{
    auto [it, inserted] = middlewareFactories_.emplace(name, std::move(mw));
    if(!inserted) {
        auto& logger = WFX::Utils::Logger::GetInstance();
        logger.Warn("[HttpMiddleware]: Duplicate registration attempt for middleware '", name, "'. Ignoring this one.");
    }
}

bool HttpMiddleware::ExecuteMiddleware(HttpRequest& req, Response& res)
{
    for(std::size_t i = 0; i < middlewareCallbacks_.size(); ++i)
    {
        MiddlewareAction action = middlewareCallbacks_[i](req, res);

        switch(action)
        {
            // Continue to next middleware
            case MiddlewareAction::CONTINUE:
                break;

            // Skip the next middleware
            case MiddlewareAction::SKIP_NEXT:
                ++i;
                break;

            // Stop middleware chain
            case MiddlewareAction::BREAK:
                return false;
        }
    }

    return true;
}

void HttpMiddleware::LoadMiddlewareFromConfig(MiddlewareOrder order)
{
    middlewareCallbacks_.clear();

    auto& logger = WFX::Utils::Logger::GetInstance();
    std::unordered_set<std::string_view> loadedNames;

    for(const auto& nameStr : order) {
        std::string_view name = nameStr.c_str();

        // Duplicate middleware name from config
        if(!loadedNames.insert(name).second) {
            logger.Warn(
                "[HttpMiddleware]: Middleware '",
                name,
                "' is listed multiple times in config. Skipping duplicate."
            );
            continue;
        }

        auto it = middlewareFactories_.find(name);
        if(it != middlewareFactories_.end())
            middlewareCallbacks_.push_back(std::move(it->second));
        else
            logger.Warn(
                "[HttpMiddleware]: Middleware '",
                name,
                "' was listed in config but has not been registered. This may be a typo or missing registration. Skipped."
            );
    }
}

void HttpMiddleware::DiscardFactoryMap()
{
    middlewareFactories_.clear();
    middlewareFactories_.rehash(0); // Force deallocation of internal buckets
}

} // namespace WFX::Http