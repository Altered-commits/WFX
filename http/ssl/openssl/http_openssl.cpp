#ifdef WFX_HTTP_USE_OPENSSL

#include "http_openssl.hpp"
#include "config/config.hpp"
#include "utils/logger/logger.hpp"
#include <openssl/err.h>

namespace WFX::Http {

using namespace WFX::Utils; // For 'Logger'
using namespace WFX::Core;  // For 'Config'

// vvv Constructors and Destructors
HttpOpenSSL::HttpOpenSSL()
{
    auto& logger    = Logger::GetInstance();
    auto& sslConfig = Config::GetInstance().sslConfig;

    // Initialize fuck ton of things
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    const SSL_METHOD* method = TLS_server_method();
    ctx = SSL_CTX_new(method);
    if(!ctx)
        logger.Fatal("[HttpOpenSSL]: Failed to create SSL_CTX");

    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    
    if(SSL_CTX_use_certificate_file(ctx, sslConfig.certPath.c_str(), SSL_FILETYPE_PEM) <= 0)
        logger.Fatal("[HttpOpenSSL]: Failed to load certificate");
    
    if(SSL_CTX_use_PrivateKey_file(ctx, sslConfig.keyPath.c_str(), SSL_FILETYPE_PEM) <= 0)
        logger.Fatal("[HttpOpenSSL]: Failed to load private key");
    
    if(!SSL_CTX_check_private_key(ctx))
        logger.Fatal("[HttpOpenSSL]: Private key does not match certificate");

    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_cipher_list(ctx, "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256");
}

HttpOpenSSL::~HttpOpenSSL()
{
    if(ctx)
        SSL_CTX_free(ctx);
}

// vvv Main Functions vvv
void* HttpOpenSSL::Wrap(SSLSocket sock)
{
    SSL* ssl = SSL_new(ctx);
    if(!ssl)
        return nullptr;

#ifdef _WIN32
    BIO* bio = BIO_new_socket(sock, BIO_NOCLOSE);
    if(!bio) {
        SSL_free(ssl);
        return nullptr;
    }
    SSL_set_bio(ssl, bio, bio);
#else
    if(!SSL_set_fd(ssl, sock)) {
        SSL_free(ssl);
        return nullptr;
    }
#endif

    return ssl;
}

bool HttpOpenSSL::Handshake(void* conn)
{
    SSL* ssl = static_cast<SSL*>(conn);

    int r = SSL_accept(ssl);
    if(r == 1)
        return true;

    int err = SSL_get_error(ssl, r);
    return err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE;
}

SSLResult HttpOpenSSL::Read(void* conn, char* buf, int len)
{
    SSL* ssl = static_cast<SSL*>(conn);
    int ret = SSL_read(ssl, buf, len);

    if(ret > 0)
        return { SSLError::SUCCESS, ret };

    int err = SSL_get_error(ssl, ret);
    switch(err) {
        case SSL_ERROR_WANT_READ:   return { SSLError::WANT_READ,  0 };
        case SSL_ERROR_WANT_WRITE:  return { SSLError::WANT_WRITE, 0 };
        case SSL_ERROR_ZERO_RETURN: return { SSLError::CLOSED,     0 };
        case SSL_ERROR_SYSCALL:     return { SSLError::SYSCALL,    0 };
        default:                    return { SSLError::FATAL,      0 };
    }
}

SSLResult HttpOpenSSL::Write(void* conn, const char* buf, int len)
{
    SSL* ssl = static_cast<SSL*>(conn);
    int ret = SSL_write(ssl, buf, len);

    if(ret > 0)
        return { SSLError::SUCCESS, ret };

    int err = SSL_get_error(ssl, ret);
    switch(err) {
        case SSL_ERROR_WANT_READ:   return { SSLError::WANT_READ,  0 };
        case SSL_ERROR_WANT_WRITE:  return { SSLError::WANT_WRITE, 0 };
        case SSL_ERROR_ZERO_RETURN: return { SSLError::CLOSED,     0 };
        case SSL_ERROR_SYSCALL:     return { SSLError::SYSCALL,    0 };
        default:                    return { SSLError::FATAL,      0 };
    }
}

void HttpOpenSSL::Shutdown(void* conn)
{
    SSL* ssl = static_cast<SSL*>(conn);
    SSL_shutdown(ssl);
    SSL_free(ssl);
}

} // namespace WFX::Http

#endif // WFX_HTTP_USE_OPENSSL