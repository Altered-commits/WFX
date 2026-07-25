// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_HTTP_ROUTER_HPP
#define WFX_HTTP_ROUTER_HPP

#include "route_trie.hpp"
#include "shared/abis/constants.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace WFX::Http {

// What a metrics index refers to, held once per process rather than per worker: registration runs-
// -identically in every worker, so only the counters need to be shared
//
// The path is joined with its group prefixes and owned here, because a grouped route's full path-
// -(WFX_GROUP_START prefix + WFX_GET path) never exists as one contiguous literal. The join happens-
// -once at registration, so two routes named /list under different groups stay distinct
struct RouteIdentity {
    std::string path;
    Shared::HttpMethod method = Shared::HttpMethod::GET;
};

// Lightweight view handed back to callers, valid as long as the Router lives
struct RouteView {
    std::string_view path;
    Shared::HttpMethod method = Shared::HttpMethod::GET;
};

class Router {
public:
    Router();
    ~Router() = default;

public:
    const TrieNode* RegisterRoute(Shared::HttpMethod method, std::string_view path, Shared::RouteCallback handler);
    const TrieNode* MatchRoute(Shared::HttpMethod method, std::string_view path, PathSegments& outParams) const;

    void PushRouteGroup(std::string_view prefix);
    void PopRouteGroup();

    // Number of registered routes, indexed dense from 0
    std::uint16_t RouteCount() const noexcept
    {
        return static_cast<std::uint16_t>(identities_.size());
    }

    RouteView RouteAt(std::uint16_t idx) const noexcept
    {
        if(idx >= identities_.size())
            return RouteView{};

        return RouteView{identities_[idx].path, identities_[idx].method};
    }

private:
    RouteTrie getRoutes_;
    RouteTrie postRoutes_;

    // Indexed by TrieNode::metricsIdx
    std::vector<RouteIdentity> identities_;

    // Group prefixes joined so far, e.g. "/api/v1" inside nested WFX_GROUP_START blocks. The stack-
    // -records the length to truncate back to on each PopRouteGroup
    std::string groupPrefix_;
    std::vector<std::size_t> groupLenStack_;

    // No need for copy and move constructors
    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;
    Router(Router&&) = delete;
    Router& operator=(Router&&) = delete;
};

} // namespace WFX::Http

#endif // WFX_HTTP_ROUTER_HPP