// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "connection_limiter.hpp"

#include "config/config.hpp"

namespace WFX::Http {

using namespace WFX::Core; // For 'Config'

ConnectionLimiter::ConnectionLimiter()
{
    connLimits_.Init(512);
}

bool ConnectionLimiter::AllowConnection(const WFXIpAddress& ip)
{
    auto* entry = connLimits_.GetOrInsert(IpUtils::NormalizeIp(ip), {});
    if(entry) {
        auto& cfg = GetConfig().ipConfig;

        if(entry->connectionCount >= cfg.maxConnectionsPerIp)
            return false;

        ++entry->connectionCount;
        return true;
    }

    return false;
}

void ConnectionLimiter::ReleaseConnection(const WFXIpAddress& ip)
{
    const WFXIpAddress key = IpUtils::NormalizeIp(ip);
    bool shouldErase = false;
    auto* entry = connLimits_.Get(key);

    if(entry) {
        // connectionCount is unsigned: never decrement past zero, or an unbalanced release-
        // -would wrap it to ~4 billion and permanently ban the IP. Erase once it reaches zero
        entry->connectionCount -= (entry->connectionCount > 0);
        shouldErase = (entry->connectionCount == 0);
    }

    if(shouldErase)
        connLimits_.Erase(key);
}

} // namespace WFX::Http
