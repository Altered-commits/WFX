// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "dns_resolver.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <vector>
#include <strings.h>
#include <netdb.h>
#include <resolv.h>
#include <arpa/nameser.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

namespace WFX::Utils {
namespace DNSResolver {

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

// Reads what /etc/hosts gives 'host', the database NSS consults before DNS and res_query never
// touches. libc owns the walking and the parsing
// Returns true if at least one entry named it
static bool TryResolveHostsFile(const char* host, std::uint16_t port, ResolvedAddrs& outAddrs)
{
    // Holds one entry: its address, canonical name, aliases and the pointer arrays over them.
    // ERANGE grows the buffer instead of dropping the entry, which would answer wrongly in silence.
    // A name is 255 bytes at most (RFC 1035), so the ceiling is already some thirty of them on one line
    static constexpr std::size_t K_ENTRY_BUF = 1024;
    static constexpr std::size_t K_ENTRY_BUF_MAX = 8 * 1024;

    // The _r buffer above is this thread's, but the position gethostent_r reads from is the
    // process's: two walks at once each see part of the file and a name can go missing. Resolve()
    // runs on several DNS threads, so the walk is serialized. It sits here and not around Resolve()
    // on purpose, since that would hold res_query's network round trip too, and those are exactly
    // what MAX_DNS_THREADS is there to overlap
    static std::mutex GlobalHostsMutex;
    const std::lock_guard<std::mutex> lock(GlobalHostsMutex);

    std::vector<char> buf(K_ENTRY_BUF);
    hostent entry{};
    hostent* found = nullptr;
    int hostErrno = 0;
    bool foundAny = false;

    // Keeps the file open across the walk instead of reopening it per entry
    sethostent(1);

    while(true) {
        const int ret = gethostent_r(&entry, buf.data(), buf.size(), &found, &hostErrno);

        // The entry is left unread on ERANGE, so the bigger buffer picks up the same one
        if(ret == ERANGE && buf.size() < K_ENTRY_BUF_MAX) {
            buf.resize(buf.size() * 2);
            continue;
        }

        if(ret != 0 || found == nullptr)
            break;

        // The canonical name and every alias name this one address. Hostnames compare without
        // case (RFC 4343)
        bool named = strcasecmp(found->h_name, host) == 0;
        for(char** alias = found->h_aliases; *alias != nullptr && !named; ++alias)
            named = strcasecmp(*alias, host) == 0;

        if(!named)
            continue;

        for(char** addr = found->h_addr_list; *addr != nullptr; ++addr) {
            AppendLiteralAddr(found->h_addrtype, *addr, port, outAddrs);
            foundAny = true;
        }
    }

    endhostent();
    return foundAny;
}

// Handles everything that resolves without a query, in the order NSS would: a literal address,
// then /etc/hosts, then 'localhost'. The file matters because res_query speaks to a nameserver and
// nothing else, so a name living only there (a container alias, a pinned host) is invisible to it
// Returns true and populates outAddrs if 'host' was handled here, false to fall through to DNS
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

    if(TryResolveHostsFile(host, port, outAddrs))
        return true;

    // RFC 6761 promises loopback whatever the resolver says, so this stands in for a hosts
    // file that never mentioned it. Both families, since dual-stack is the common case and
    // round-robin/connect-failure-skip handles either being unreachable on a given system.
    if(std::strcmp(host, "localhost") == 0) {
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
    const int len = res_query(host, ns_c_in, recordType, response, sizeof(response));
    if(len < 0)
        return false;

    ns_msg handle;
    if(ns_initparse(response, len, &handle) < 0)
        return false;

    const int count = ns_msg_count(handle, ns_s_an);
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

    const bool gotA = QueryRecordType(host, ns_t_a, port, outAddrs);
    const bool gotAAAA = QueryRecordType(host, ns_t_aaaa, port, outAddrs);

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