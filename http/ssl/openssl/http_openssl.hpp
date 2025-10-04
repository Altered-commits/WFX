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
    void*     Wrap(SSLSocket fd)                                                        override;
    SSLReturn Handshake(void* conn)                                                     override;
    
    SSLResult Read(void* conn, char* buf, int len)                                      override;
    SSLResult Write(void* conn, const char* buf, int len)                               override;
    SSLResult WriteFile(void* conn, SSLSocket fd, FileOffset offset, std::size_t count) override;
    
    SSLReturn Shutdown(void* conn)                                                      override;
    SSLReturn ForceShutdown(void* conn)                                                 override;

private: // Helper functions
    void GlobalOpenSSLInit();
    void LogOpenSSLErrorAndExit(const char* message);

private:
    SSL_CTX* ctx = nullptr;
};

} // namespace WFX::Http

#endif // WFX_HTTP_OPENSSL_HPP

#endif // WFX_HTTP_USE_OPENSSL