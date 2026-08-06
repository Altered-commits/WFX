// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "router.hpp"
#include "config/config.hpp"
#include "utils/diagnostics/logger.hpp"
#include "shared/utils/compiler_macro.hpp"

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

    TrieNode* node = nullptr;

    switch(method) {
        case HttpMethod::GET:
            node = getRoutes_.Insert(path, handler);
            break;

        case HttpMethod::POST:
            node = postRoutes_.Insert(path, handler);
            break;

        default:
            GetLogger().Fatal(
                "[Router]: Unsupported HTTP method found in RegisterRoute. Use HttpMethod::GET or HttpMethod::POST.");
            WFX_UNREACHABLE;
    }

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
    // Strip query string before matching
    const std::string_view queryStrippedPath = path.substr(0, path.find('?'));

    switch(method) {
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
    groupLenStack_.push_back(groupPrefix_.size());
    JoinPath(groupPrefix_, prefix);

    getRoutes_.PushGroup(prefix);
    postRoutes_.PushGroup(prefix);
}

void Router::PopRouteGroup()
{
    if(!groupLenStack_.empty()) {
        groupPrefix_.resize(groupLenStack_.back());
        groupLenStack_.pop_back();
    }

    getRoutes_.PopGroup();
    postRoutes_.PopGroup();
}

} // namespace WFX::Http
