// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_HTTP_SSL_HPP
#define WFX_HTTP_SSL_HPP

#include <sys/types.h>
using SSLSocket = int;
using FileOffset = off_t;
using ReturnType = ssize_t;

#include <cstdint>
#include <string_view>

namespace WFX::Http {

// Common return values for Read / Write errors
enum class SSLReturn : std::uint8_t { SUCCESS, WANT_READ, WANT_WRITE, CLOSED, SYSCALL, FATAL, NO_IMPL };

struct SSLResult {
    SSLReturn error;
    ReturnType res;
};

// Interface around SSL implementations
struct HttpWFXSSL {
    virtual ~HttpWFXSSL() = default;

    // Builds the outbound (client) context if it isn't up yet, returns false if it can't be
    // Idempotent, and independent of the inbound one: talking TLS to an upstream needs no
    // certificate, so it must not require the server itself to serve HTTPS.
    // In-band upgrades decide this at runtime, which is why it stays callable after construction.
    virtual bool EnsureClientContext() = 0;

    // Wrap a socket and return opaque handle
    virtual void* Wrap(SSLSocket fd) = 0;

    // alpnList is a wire-encoded ALPN protocol list (same encoding as the engine's hardcoded
    // default); empty = offer hardcoded default (http/1.1 only).
    //
    // sessionSlot: caller-owned opaque per-endpoint storage for TLS session resumption
    // May be read (offer for reuse) and/or written (store newly negotiated session)
    // nullptr disables resumption. Caller must free via FreeCachedSession()
    virtual void* WrapClient(SSLSocket fd, const char* host, std::string_view alpnList = {},
                             void** sessionSlot = nullptr) = 0;

    // Empty if the handshake hasn't completed or the peer didn't negotiate ALPN at all
    virtual std::string_view NegotiatedProtocol(void* conn) = 0;

    // Handshake; returns true if done
    virtual SSLReturn Handshake(void* conn) = 0;

    // Read/Write functions
    virtual SSLResult Read(void* conn, char* buf, int len) = 0;
    virtual SSLResult Write(void* conn, const char* buf, int len) = 0;
    virtual SSLResult WriteFile(void* conn, SSLSocket fd, FileOffset offset, std::size_t count) = 0;

    // Shutdown and Free connection
    virtual SSLReturn Shutdown(void* conn) = 0;
    virtual SSLReturn ForceShutdown(void* conn) = 0;

    // Releases a session written into a WrapClient() sessionSlot. No-op on nullptr
    virtual void FreeCachedSession(void* session) = 0;
};

} // namespace WFX::Http

#endif // WFX_HTTP_SSL_HPP