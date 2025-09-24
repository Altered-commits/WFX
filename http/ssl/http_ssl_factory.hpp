#ifndef WFX_HTTP_SSL_FACTORY_HPP
#define WFX_HTTP_SSL_FACTORY_HPP

// This is simply a helper thingy which will abstract the 'selecting'-
// -of SSL specific functionality for HTTPS handling
#include <memory>

#ifdef WFX_HTTP_USE_OPENSSL
    #include "openssl/http_openssl.hpp"
#else
    #error "WFX_HTTP_USE_OPENSSL macro not found. Only OpenSSL is supported for now"
#endif

namespace WFX::Http {
    // Factory function that returns the correct handler
    inline std::unique_ptr<HttpWFXSSL> CreateSSLHandler()
    {
    #ifdef WFX_HTTP_USE_OPENSSL
        return std::make_unique<HttpOpenSSL>();
    #else
        static_assert(false, "WFX_HTTP_USE_OPENSSL macro not found. Only OpenSSL backend is supported for now");
        return nullptr;
    #endif
    }
}

#endif // WFX_HTTP_SSL_FACTORY_HPP