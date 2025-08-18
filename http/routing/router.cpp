#include "router.hpp"
#include "utils/logger/logger.hpp"

namespace WFX::Http {

using namespace WFX::Utils; // For 'Logger'

Router& Router::GetInstance()
{
    static Router router;
    return router;
}

void Router::RegisterRoute(HttpMethod method, std::string_view path, HttpCallbackType handler)
{
    if(path.empty() || path[0] != '/')
        Logger::GetInstance().Fatal("[Router]: Path is either empty or does not start with '/'.");

    switch(method)
    {
        case HttpMethod::GET:
            getRoutes_.Insert(path, std::move(handler));
            break;
        case HttpMethod::POST:
            postRoutes_.Insert(path, std::move(handler));
            break;
        default:
            Logger::GetInstance().Fatal(
                "[Router]: Unsupported HTTP method found in RegisterRoute. Use HttpMethod::GET or HttpMethod::POST."
            );
    }
}

const HttpCallbackType* Router::MatchRoute(HttpMethod method, std::string_view path, PathSegments& outSegments) const
{
    // We will always assume that the segments we receive will contain data as-
    // -we are working in a keep-alive supported architecture, so the previous data needs-
    // -to be cleaned out
    outSegments.clear();

    // Strip query string before matching
    std::string_view queryStrippedPath = path.substr(0, path.find('?'));

    switch(method)
    {
        case HttpMethod::GET:
            return getRoutes_.Match(queryStrippedPath, outSegments);
        case HttpMethod::POST:
            return postRoutes_.Match(queryStrippedPath, outSegments);
        default:
            return nullptr;
    }
}

void Router::PushRouteGroup(std::string_view prefix)
{
    getRoutes_.PushGroup(prefix);
    postRoutes_.PushGroup(prefix);
}

void Router::PopRouteGroup()
{
    getRoutes_.PopGroup();
    postRoutes_.PopGroup();
}

} // namespace WFX::Http
