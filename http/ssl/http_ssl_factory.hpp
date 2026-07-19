// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_HTTP_SSL_FACTORY_HPP
#define WFX_HTTP_SSL_FACTORY_HPP

// This is simply a helper thingy which will abstract the 'selecting'-
// -of SSL specific functionality for HTTPS handling
#include <memory>

#ifdef WFX_USE_OPENSSL
#include "openssl/http_openssl.hpp"
#else
#error "WFX_USE_OPENSSL macro not found. Only OpenSSL is supported for now"
#endif

namespace WFX::Http {

// Factory function that returns the correct handler
// initServer builds the inbound context immediately; the outbound one is created on demand-
// -through EnsureClientContext, so outbound TLS never depends on the server serving HTTPS
inline std::unique_ptr<HttpWFXSSL> CreateSSLHandler(bool initServer)
{
#ifdef WFX_USE_OPENSSL
    return std::make_unique<HttpOpenSSL>(initServer);
#else
    static_assert(false, "WFX_USE_OPENSSL macro not found. Only OpenSSL backend is supported for now");
    return nullptr;
#endif
}

} // namespace WFX::Http

#endif // WFX_HTTP_SSL_FACTORY_HPP