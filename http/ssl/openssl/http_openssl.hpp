// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifdef WFX_USE_OPENSSL

#ifndef WFX_HTTP_OPENSSL_HPP
#define WFX_HTTP_OPENSSL_HPP

#include "../http_ssl.hpp"
#include <openssl/ssl.h>

namespace WFX::Http {

class HttpOpenSSL : public HttpWFXSSL {
public:
    // initServer builds the inbound context up front, since serving HTTPS is a boot-time fact and
    // a missing certificate should fail startup rather than the first request.
    // The outbound one is left to EnsureClientContext.
    explicit HttpOpenSSL(bool initServer);
    ~HttpOpenSSL() override;

public: // Main functions
    bool EnsureClientContext() override;
    void* Wrap(SSLSocket fd) override;
    void* WrapClient(SSLSocket fd, const char* host, std::string_view alpnList = {},
                     void** sessionSlot = nullptr) override;
    std::string_view NegotiatedProtocol(void* conn) override;
    SSLReturn Handshake(void* conn) override;
    std::string_view GetHandshakeFailure(void* conn) override;

    SSLResult Read(void* conn, char* buf, int len) override;
    SSLResult Write(void* conn, const char* buf, int len) override;
    SSLResult WriteFile(void* conn, SSLSocket fd, FileOffset offset, std::size_t count) override;

    SSLReturn Shutdown(void* conn) override;
    SSLReturn ForceShutdown(void* conn) override;

    void FreeCachedSession(void* session) override;

private: // Helper functions
    void InitServerContext();
    bool InitClientContext();
    void GlobalOpenSSLInit();
    [[noreturn]] void FatalOpenSSLError(const char* message);
    bool FailClientContext(const char* message); // Logs why, frees the half-built context, returns false

    // Client session resumption: fires when a session becomes available (during the handshake
    // for <=TLS1.2, or after it for TLS1.3's post-handshake ticket), see SSL_CTX_sess_set_new_cb(3).
    // Writes into the sessionSlot tagged on 'ssl' by WrapClient.
    static int NewClientSessionCallback(SSL* ssl, SSL_SESSION* sess);

private:
    SSL_CTX* serverCtx_ = nullptr;
    SSL_CTX* clientCtx_ = nullptr;
    std::uint64_t handshakeError_ = 0; // The last failed Handshake()'s queued error, see GetHandshakeFailure
    bool useKtls_ = false;
    bool clientSessionCacheEnabled_ = false;
    bool clientCtxFailed_ = false; // InitClientContext failed once, see EnsureClientContext
};

} // namespace WFX::Http

#endif // WFX_HTTP_OPENSSL_HPP

#endif // WFX_USE_OPENSSL