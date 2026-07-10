// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "dns_resolver.hpp"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <resolv.h>
#include <arpa/nameser.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

namespace WFX::Utils {
namespace DNSResolver {

// Linux / macOS, both use the BSD resolver API

// Builds a single ResolvedAddr from an already-known IP (literal or loopback alias)
// No DNS involved, nothing to ever refresh, so ttlSeconds is set to UINT32_MAX
static void AppendLiteralAddr(int family, const void* rawAddr, std::uint16_t port, ResolvedAddrs& outAddrs)
{
    ResolvedAddr entry{};
    entry.ttlSeconds = UINT32_MAX;

    if(family == AF_INET) {
        sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_port = htons(port);

        memcpy(&sin.sin_addr, rawAddr, sizeof(in_addr));
        memcpy(&entry.addr, &sin, sizeof(sin));

        entry.addrLen = sizeof(sockaddr_in);
    }
    else {
        sockaddr_in6 sin6{};
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port = htons(port);

        memcpy(&sin6.sin6_addr, rawAddr, sizeof(in6_addr));
        memcpy(&entry.addr, &sin6, sizeof(sin6));

        entry.addrLen = sizeof(sockaddr_in6);
    }

    outAddrs.push_back(entry);
}

// Handles literal IPs and the well-known 'localhost' hostname without touching DNS at all
// 'localhost' specifically is NOT guaranteed to be forwardable to a real DNS server. It-
// -typically only resolves via /etc/hosts at the NSS layer, which res_query bypasses-
// -entirely. Returns true and populates outAddrs if 'host' was handled here, false if-
// -the caller should fall through to a real DNS query
static bool TryResolveLocal(const char* host, std::uint16_t port, ResolvedAddrs& outAddrs)
{
    in_addr v4{};
    in6_addr v6{};

    if(inet_pton(AF_INET, host, &v4) == 1) {
        AppendLiteralAddr(AF_INET, &v4, port, outAddrs);
        return true;
    }

    if(inet_pton(AF_INET6, host, &v6) == 1) {
        AppendLiteralAddr(AF_INET6, &v6, port, outAddrs);
        return true;
    }

    if(std::strcmp(host, "localhost") == 0) {
        // Standard loopback addresses per RFC 6761. Both families, since dual-stack-
        // -is the common case and round-robin/connect-failure-skip handles either-
        // -being unreachable on a given system
        in_addr loopback4{};
        loopback4.s_addr = htonl(INADDR_LOOPBACK);
        AppendLiteralAddr(AF_INET, &loopback4, port, outAddrs);

        in6_addr loopback6 = IN6ADDR_LOOPBACK_INIT;
        AppendLiteralAddr(AF_INET6, &loopback6, port, outAddrs);

        return true;
    }

    return false;
}

static bool QueryRecordType(const char* host, int recordType, std::uint16_t port, ResolvedAddrs& outAddrs)
{
    unsigned char response[NS_PACKETSZ];
    int len = res_query(host, ns_c_in, recordType, response, sizeof(response));
    if(len < 0)
        return false;

    ns_msg handle;
    if(ns_initparse(response, len, &handle) < 0)
        return false;

    int count = ns_msg_count(handle, ns_s_an);
    bool foundAny = false;

    for(int i = 0; i < count; i++) {
        ns_rr rr;
        if(ns_parserr(&handle, ns_s_an, i, &rr) < 0)
            continue;

        if(ns_rr_type(rr) != recordType)
            continue;

        ResolvedAddr entry{};
        entry.ttlSeconds = ns_rr_ttl(rr);

        if(recordType == ns_t_a) {
            if(ns_rr_rdlen(rr) != sizeof(in_addr))
                continue;

            sockaddr_in sin{};
            sin.sin_family = AF_INET;
            sin.sin_port = htons(port);

            memcpy(&sin.sin_addr, ns_rr_rdata(rr), sizeof(in_addr));
            memcpy(&entry.addr, &sin, sizeof(sin));

            entry.addrLen = sizeof(sockaddr_in);
        }
        else {
            if(ns_rr_rdlen(rr) != sizeof(in6_addr))
                continue;

            sockaddr_in6 sin6{};
            sin6.sin6_family = AF_INET6;
            sin6.sin6_port = htons(port);

            memcpy(&sin6.sin6_addr, ns_rr_rdata(rr), sizeof(in6_addr));
            memcpy(&entry.addr, &sin6, sizeof(sin6));

            entry.addrLen = sizeof(sockaddr_in6);
        }

        outAddrs.push_back(entry);
        foundAny = true;
    }

    return foundAny;
}

bool Resolve(const char* host, std::uint16_t port, ResolvedAddrs& outAddrs, std::uint32_t& outMinTtlSeconds)
{
    outAddrs.clear();
    outMinTtlSeconds = 0;

    if(TryResolveLocal(host, port, outAddrs)) {
        outMinTtlSeconds = UINT32_MAX;
        return true;
    }

    bool gotA = QueryRecordType(host, ns_t_a, port, outAddrs);
    bool gotAAAA = QueryRecordType(host, ns_t_aaaa, port, outAddrs);

    if(!gotA && !gotAAAA)
        return false;

    std::uint32_t minTtl = UINT32_MAX;
    for(auto& a : outAddrs)
        minTtl = std::min(minTtl, a.ttlSeconds);

    outMinTtlSeconds = minTtl;
    return true;
}

} // namespace DNSResolver
} // namespace WFX::Utils