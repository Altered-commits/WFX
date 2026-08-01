// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_HTTP_IP_UTILS_HPP
#define WFX_HTTP_IP_UTILS_HPP

#include "http/connection/http_connection.hpp"
#include "http/headers/http_headers.hpp"
#include "config/config.hpp"
#include <string_view>

namespace WFX::Http {
namespace IpUtils {

struct CidrBlock {
    WFXIpAddress base;          // port is unused here, only 'ip'/'type' matter
    std::uint8_t prefixLen = 0; // 0-32 for AF_INET, 0-128 for AF_INET6
};

// False on anything that isn't a bare, valid IPv4 or IPv6 address (no '/prefix' suffix)
// 'out.port' is left at 0, only 'ip'/'type' are populated
bool ParseIpAddress(std::string_view text, WFXIpAddress& out);

// False on any malformed input: missing '/', non-numeric prefix, or a prefix out of range for-
// -whichever family the address parses as. 'out' is left untouched on failure
bool ParseCidr(std::string_view text, CidrBlock& out);

// False immediately on a family mismatch (an IPv4 peer never matches an IPv6 block or vice versa)
bool CidrMatches(const WFXIpAddress& ip, const CidrBlock& block);

// Masks an IP down to its subnet (/24 for v4, /64 for v6), shared by every per-IP limiter so-
// -entries key on subnet rather than individual address
WFXIpAddress NormalizeIp(const WFXIpAddress& ip);

// Resolves the IP to key rate limiting on. Returns 'peerIp' unchanged unless every condition for-
// -trusting a header is met: a header is configured, 'peerIp' itself matches one of-
// -'ipConfig.trustedProxies', the header is present, and it parses to a valid address. Any failed-
// -condition falls back to 'peerIp' - fails safe by construction, never partially trusts a header
WFXIpAddress ResolveClientIp(const WFXIpAddress& peerIp, const RequestHeaders& headers, const Core::IPConfig& ipConfig);

} // namespace IpUtils
} // namespace WFX::Http

#endif // WFX_HTTP_IP_UTILS_HPP
