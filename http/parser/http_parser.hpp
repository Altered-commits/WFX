// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_HTTP_PARSER_HPP
#define WFX_HTTP_PARSER_HPP

#include <cstdint>

namespace WFX::Http {

// Forward declare stuff
struct ClientCtx;
enum class HttpParseState : std::uint8_t;

namespace HttpParser {

HttpParseState Parse(ClientCtx* ctx);

} // namespace HttpParser
} // namespace WFX::Http

#endif // WFX_HTTP_PARSER_HPP