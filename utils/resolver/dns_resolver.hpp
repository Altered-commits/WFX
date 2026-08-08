// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_UTILS_DNS_RESOLVER_HPP
#define WFX_UTILS_DNS_RESOLVER_HPP

#include <cstdint>
#include <vector>

#include <sys/socket.h>

namespace WFX::Utils {

// Single source of truth for a resolved address across the engine and DNS resolver
// 'ttlSeconds' is only meaningful immediately after resolution (used to schedule the
// next refresh); the engine ignores it for everything past registration.
struct ResolvedAddr {
    sockaddr_storage addr;
    socklen_t addrLen;
    std::uint32_t ttlSeconds = 0; // 0 if unknown / not resolved via TTL-aware path
};
static_assert(sizeof(ResolvedAddr) == 136, "'ResolvedAddr' size unexpected for this platform");

// Useful alias
using ResolvedAddrs = std::vector<ResolvedAddr>;

namespace DNSResolver {

// Queries A and AAAA records for 'host', returns all addresses found across either
// 'outMinTtlSeconds' is set to the minimum TTL observed across all returned records
// Callers use this to decide the next scheduled refresh time
// Returns false only if BOTH record types fail to resolve at all, UINT32_MAX in outMinTtlSeconds if localhost
bool Resolve(const char* host, std::uint16_t port, ResolvedAddrs& outAddrs, std::uint32_t& outMinTtlSeconds);

} // namespace DNSResolver

} // namespace WFX::Utils

#endif // WFX_UTILS_DNS_RESOLVER_HPP