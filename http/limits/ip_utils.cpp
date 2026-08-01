// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "ip_utils.hpp"
#include "utils/string/string.hpp"

#include <arpa/inet.h>
#include <charconv>
#include <cstring>

namespace WFX::Http {
namespace IpUtils {

bool ParseIpAddress(std::string_view text, WFXIpAddress& out)
{
    // std::string_view isn't null-terminated, inet_pton needs a C string
    char addrBuf[INET6_ADDRSTRLEN] = {};
    if(text.empty() || text.size() >= sizeof(addrBuf))
        return false;

    std::memcpy(addrBuf, text.data(), text.size());

    WFXIpAddress parsed;

    if(inet_pton(AF_INET, addrBuf, &parsed.ip.v4) == 1)
        parsed.type = AF_INET;
    else if(inet_pton(AF_INET6, addrBuf, &parsed.ip.v6) == 1) {
        // ::ffff:a.b.c.d is the same address as a.b.c.d wherever it can appear (a dual-stack peer,-
        // -or a proxy writing it into a forwarding header): collapse it to AF_INET here, or every-
        // -CIDR match and the /24-vs-/64 NormalizeIp split below keys on the wrong family for it
        if(IN6_IS_ADDR_V4MAPPED(&parsed.ip.v6)) {
            in_addr v4;
            std::memcpy(&v4, &parsed.ip.v6.s6_addr[12], sizeof(v4));
            parsed.ip.v4 = v4;
            parsed.type = AF_INET;
        }
        else
            parsed.type = AF_INET6;
    }
    else
        return false;

    out = parsed;
    return true;
}

bool ParseCidr(std::string_view text, CidrBlock& out)
{
    const std::size_t slash = text.find('/');
    if(slash == std::string_view::npos)
        return false;

    const std::string_view addrPart = text.substr(0, slash);
    const std::string_view prefixPart = text.substr(slash + 1);

    CidrBlock parsed;
    if(!ParseIpAddress(addrPart, parsed.base))
        return false;

    const std::uint8_t maxPrefix = (parsed.base.type == AF_INET) ? 32 : 128;

    unsigned prefixVal = 0;
    const auto [ptr, ec] = std::from_chars(prefixPart.data(), prefixPart.data() + prefixPart.size(), prefixVal);
    if(ec != std::errc{} || ptr != prefixPart.data() + prefixPart.size() || prefixVal > maxPrefix)
        return false;

    parsed.prefixLen = static_cast<std::uint8_t>(prefixVal);
    out = parsed;

    return true;
}

bool CidrMatches(const WFXIpAddress& ip, const CidrBlock& block)
{
    if(ip.type != block.base.type)
        return false;

    const std::uint8_t fullBytes = block.prefixLen / 8;
    const std::uint8_t remBits = block.prefixLen % 8;

    if(fullBytes > 0 && std::memcmp(ip.ip.raw, block.base.ip.raw, fullBytes) != 0)
        return false;

    // Partial byte: compare only the top 'remBits' bits of the next byte
    if(remBits > 0) {
        const std::uint8_t mask = static_cast<std::uint8_t>(0xFF << (8 - remBits));
        if((ip.ip.raw[fullBytes] & mask) != (block.base.ip.raw[fullBytes] & mask))
            return false;
    }

    return true;
}

WFXIpAddress NormalizeIp(const WFXIpAddress& ip)
{
    WFXIpAddress out = ip;

    // Both limiters funnel every key through here, and WFXIpAddress::operator== (and by-
    // -extension HashShard's key equality) includes 'port'. A limiter identity is never-
    // -port-specific, so the canonical key this function produces always zeroes it, rather-
    // -than trusting every caller to have already left it at 0
    out.port = 0;

    if(ip.type == AF_INET)
        out.ip.v4.s_addr &= htonl(0xFFFFFF00); // Mask out subnet /24
    else
        memset(&out.ip.v6.s6_addr[8], 0, 8); // Mask out subnet /64

    return out;
}

static bool IsTrustedProxy(const WFXIpAddress& ip, const Core::IPConfig& ipConfig)
{
    for(const auto& cidrText : ipConfig.trustedProxies) {
        CidrBlock block;
        if(ParseCidr(cidrText, block) && CidrMatches(ip, block))
            return true;
    }

    return false;
}

WFXIpAddress ResolveClientIp(const WFXIpAddress& peerIp, const RequestHeaders& headers, const Core::IPConfig& ipConfig)
{
    if(ipConfig.realIpHeader.empty() || !IsTrustedProxy(peerIp, ipConfig))
        return peerIp;

    const std::string_view headerValue = headers.GetHeader(ipConfig.realIpHeader);
    if(headerValue.empty())
        return peerIp;

    std::string_view candidate = headerValue;

    // X-Forwarded-For-style chains ("client, proxy1, proxy2"): walk right-to-left past hops that-
    // -are themselves trusted proxies, stopping at the first untrusted (real client) hop. If the-
    // -whole chain is trusted proxies, the leftmost (closest-to-client) hop is used as a best effort
    if(ipConfig.realIpRecursive) {
        while(true) {
            const std::size_t comma = candidate.rfind(',');
            const std::string_view hop =
                Utils::StringUtils::TrimView(comma == std::string_view::npos ? candidate : candidate.substr(comma + 1));

            WFXIpAddress hopIp;
            if(!ParseIpAddress(hop, hopIp) || !IsTrustedProxy(hopIp, ipConfig)) {
                candidate = hop;
                break;
            }

            // Whole chain trusted, this was its last (leftmost) hop: 'candidate' must become the-
            // -already-TRIMMED 'hop', or a leading/trailing space on this token survives into the-
            // -ParseIpAddress call below (inet_pton rejects whitespace), silently falling back to-
            // -peerIp instead of the intended best-effort leftmost hop
            if(comma == std::string_view::npos) {
                candidate = hop;
                break;
            }

            candidate = candidate.substr(0, comma);
        }
    }
    else
        candidate = Utils::StringUtils::TrimView(candidate);

    WFXIpAddress resolved;
    if(!ParseIpAddress(candidate, resolved))
        return peerIp;

    return resolved;
}

} // namespace IpUtils
} // namespace WFX::Http
