// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_HTTP_CONNECTION_LIMITER_HPP
#define WFX_HTTP_CONNECTION_LIMITER_HPP

#include "http/connection/http_connection.hpp"
#include "http/limits/ip_utils.hpp"
#include "utils/hash_map/hash_shard.hpp"

namespace WFX::Http {

struct ConnectionLimiterEntry {
    std::uint32_t connectionCount = 0;
};

// Peer-IP keyed: caps concurrent connections per (normalized) source IP. Entry lifecycle is tied-
// -1:1 to the connection count itself, created in AllowConnection, erased once the last connection-
// -from that IP closes via ReleaseConnection
class ConnectionLimiter {
public:
    ConnectionLimiter();
    ~ConnectionLimiter() = default;

public:
    // Called on new connection attempt
    bool AllowConnection(const WFXIpAddress& ip);

    // Called when a connection closes
    void ReleaseConnection(const WFXIpAddress& ip);

private:
    ConnectionLimiter(const ConnectionLimiter&) = delete;
    ConnectionLimiter& operator=(const ConnectionLimiter&) = delete;
    ConnectionLimiter(ConnectionLimiter&&) = delete;
    ConnectionLimiter& operator=(ConnectionLimiter&&) = delete;

private:
    Utils::HashShard<WFXIpAddress, ConnectionLimiterEntry> connLimits_;
};

} // namespace WFX::Http

#endif // WFX_HTTP_CONNECTION_LIMITER_HPP
