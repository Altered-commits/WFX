// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "router.hpp"
#include "config/config.hpp"
#include "utils/diagnostics/logger.hpp"

namespace WFX::Http {

using namespace WFX::Utils;  // For 'Logger'
using namespace WFX::Shared; // For every single abi type

// Appends one path segment onto an accumulator with exactly one '/' between them, so joining a
// group prefix and a route path never doubles or drops a slash.
static void JoinPath(std::string& acc, std::string_view seg)
{
    if(!acc.empty() && acc.back() == '/')
        acc.pop_back();

    if(seg.empty())
        return;

    if(seg.front() != '/')
        acc.push_back('/');

    acc.append(seg);
}

Router::Router() = default;

const TrieNode* Router::RegisterRoute(HttpMethod method, std::string_view path, RouteCallback handler)
{
    if(path.empty() || path[0] != '/')
        GetLogger().Fatal("[Router]: Path is either empty or does not start with '/'.");

    const auto methodIdx = static_cast<std::uint8_t>(method);
    if(methodIdx >= ROUTABLE_METHOD_COUNT)
        GetLogger().Fatal("[Router]: Unsupported HTTP method found in RegisterRoute.");

    TrieNode* node = routesByMethod_[methodIdx].Insert(path, handler);

    // Re-registering the same method and path lands on the same node, which already owns an index
    if(node->metricsIdx == METRICS_IDX_UNASSIGNED) {
        const std::uint16_t maxRoutes = Core::GetConfig().metricsConfig.maxRoutes;

        if(identities_.size() >= maxRoutes)
            GetLogger().Fatal("[Router]: Cannot register '", path, "', all ", maxRoutes,
                              " route metric slots are taken. Raise '[Metrics] max_routes' in wfx.toml.");

        node->metricsIdx = static_cast<std::uint16_t>(identities_.size());

        // Join the active group prefix so a route inside WFX_GROUP_START reports its full path
        std::string fullPath = groupPrefix_;
        JoinPath(fullPath, path);
        identities_.push_back({std::move(fullPath), method});
    }

    return node;
}

const TrieNode* Router::MatchRoute(HttpMethod method, std::string_view path, PathSegments& outSegments) const
{
    const auto methodIdx = static_cast<std::uint8_t>(method);
    if(methodIdx >= ROUTABLE_METHOD_COUNT)
        return nullptr;

    // Strip query string before matching
    const std::string_view queryStrippedPath = path.substr(0, path.find('?'));
    return routesByMethod_[methodIdx].Match(queryStrippedPath, outSegments);
}

std::vector<HttpMethod> Router::MethodsAt(std::string_view path) const
{
    const std::string_view queryStrippedPath = path.substr(0, path.find('?'));

    std::vector<HttpMethod> methods;
    PathSegments scratch;

    for(std::size_t i = 0; i < ROUTABLE_METHOD_COUNT; ++i) {
        scratch.clear();
        if(routesByMethod_[i].Match(queryStrippedPath, scratch))
            methods.push_back(static_cast<HttpMethod>(i));
    }

    return methods;
}

void Router::PushRouteGroup(std::string_view prefix)
{
    groupLenStack_.push_back(groupPrefix_.size());
    JoinPath(groupPrefix_, prefix);

    for(auto& trie : routesByMethod_)
        trie.PushGroup(prefix);
}

void Router::PopRouteGroup()
{
    if(!groupLenStack_.empty()) {
        groupPrefix_.resize(groupLenStack_.back());
        groupLenStack_.pop_back();
    }

    for(auto& trie : routesByMethod_)
        trie.PopGroup();
}

} // namespace WFX::Http
