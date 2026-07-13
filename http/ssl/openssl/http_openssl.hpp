// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifdef WFX_HTTP_USE_OPENSSL

#ifndef WFX_HTTP_OPENSSL_HPP
#define WFX_HTTP_OPENSSL_HPP

#include "../http_ssl.hpp"
#include <openssl/types.h>

namespace WFX::Http {

class HttpOpenSSL : public HttpWFXSSL {
public:
    HttpOpenSSL();
    ~HttpOpenSSL() override;

public: // Main functions
    void* Wrap(SSLSocket fd) override;
    void* WrapClient(SSLSocket fd, const char* host, std::string_view alpnList = {}) override;
    std::string_view NegotiatedProtocol(void* conn) override;
    SSLReturn Handshake(void* conn) override;

    SSLResult Read(void* conn, char* buf, int len) override;
    SSLResult Write(void* conn, const char* buf, int len) override;
    SSLResult WriteFile(void* conn, SSLSocket fd, FileOffset offset, std::size_t count) override;

    SSLReturn Shutdown(void* conn) override;
    SSLReturn ForceShutdown(void* conn) override;

private: // Helper functions
    void InitServerContext();
    void InitClientContext();
    void GlobalOpenSSLInit();
    void LogOpenSSLError(const char* message, SSL* ssl = nullptr, bool fatal = true);

private:
    SSL_CTX* serverCtx_ = nullptr;
    SSL_CTX* clientCtx_ = nullptr;
    bool useKtls_ = false;
};

} // namespace WFX::Http

#endif // WFX_HTTP_OPENSSL_HPP

#endif // WFX_HTTP_USE_OPENSSL