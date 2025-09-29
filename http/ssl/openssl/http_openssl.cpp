#ifdef WFX_HTTP_USE_OPENSSL

#include "http_openssl.hpp"
#include "config/config.hpp"
#include "http/common/http_global_state.hpp"
#include "utils/logger/logger.hpp"
#include <openssl/ssl.h>
#include <openssl/err.h>

namespace WFX::Http {

using namespace WFX::Utils; // For 'Logger'
using namespace WFX::Core;  // For 'Config'

// vvv Constructors and Destructors vvv
HttpOpenSSL::HttpOpenSSL()
{
    auto& logger    = Logger::GetInstance();
    auto& sslConfig = Config::GetInstance().sslConfig;

    // Start of pain and suffering :(
    GlobalOpenSSLInit();

    // Helper lambda to log OpenSSL error before exiting
    auto LogOpenSSLErrorAndExit = [&](const char* message) {
        std::string allErrors;
        unsigned long errCode;
        char errBuf[256];

        while((errCode = ERR_get_error()) != 0) {
            ERR_error_string_n(errCode, errBuf, sizeof(errBuf));
            if(!allErrors.empty())
                allErrors.append("; ");
            allErrors.append(errBuf);
        }

        if(allErrors.empty())
            logger.Fatal("[HttpOpenSSL]: ", message, ". No specific OpenSSL error code available");
        else
            logger.Fatal("[HttpOpenSSL]: ", message, ". OpenSSL Reason(s): ", allErrors);
    };

    // Use the default TLS method, which negotiates the highest common version
    const SSL_METHOD* method = TLS_server_method();
    ctx = SSL_CTX_new(method);
    if(!ctx)
        logger.Fatal("[HttpOpenSSL]: Failed to create SSL_CTX");

    // Level 2 provides 112-bit security disabling weak ciphers and RSA keys < 2048 bits
    SSL_CTX_set_security_level(ctx, std::clamp(sslConfig.securityLevel, 0, 5));

    // Set the minimum protocol version
    int protoVersion = TLS1_2_VERSION;
    switch(sslConfig.minProtoVersion) {
        case 1:  protoVersion = TLS1_VERSION;   break;
        case 2:  protoVersion = TLS1_2_VERSION; break;
        case 3:  protoVersion = TLS1_3_VERSION; break;
        default: protoVersion = TLS1_2_VERSION;
    }
    if(SSL_CTX_set_min_proto_version(ctx, protoVersion) != 1)
        LogOpenSSLErrorAndExit("Failed to set minimum TLS protocol version");

    // Load certificate and private key
    if(SSL_CTX_use_certificate_chain_file(ctx, sslConfig.certPath.c_str()) <= 0)
        LogOpenSSLErrorAndExit("Failed to load certificate chain file");
    
    if(SSL_CTX_use_PrivateKey_file(ctx, sslConfig.keyPath.c_str(), SSL_FILETYPE_PEM) <= 0)
        LogOpenSSLErrorAndExit("Failed to load private key");
    
    if(!SSL_CTX_check_private_key(ctx))
        LogOpenSSLErrorAndExit("Private key does not match certificate");

    // Server side session caching
    if(sslConfig.enableSessionCache) {
        SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);
        SSL_CTX_sess_set_cache_size(ctx, sslConfig.sessionCacheSize);
    }

    auto& ticketKey = GetGlobalState().sslKey;
    if(SSL_CTX_set_tlsext_ticket_keys(ctx, ticketKey.data(), ticketKey.size()) != 1)
        LogOpenSSLErrorAndExit("Failed to set session ticket keys");
    
    // Set modern cipher preferences
    if(!sslConfig.tls13Ciphers.empty() && SSL_CTX_set_ciphersuites(ctx, sslConfig.tls13Ciphers.c_str()) != 1)
        LogOpenSSLErrorAndExit("Failed to set TLSv1.3 ciphersuites");

    if(!sslConfig.tls12Ciphers.empty() && SSL_CTX_set_cipher_list(ctx, sslConfig.tls12Ciphers.c_str()) != 1)
        LogOpenSSLErrorAndExit("Failed to set TLSv1.2 cipher list");

    // Set remaining essential options
    if(!sslConfig.curves.empty())
        SSL_CTX_set1_curves_list(ctx, sslConfig.curves.c_str());

    SSL_CTX_set_mode(ctx,
        SSL_MODE_RELEASE_BUFFERS |
        SSL_MODE_ENABLE_PARTIAL_WRITE |
        SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER
    );

#ifdef SSL_OP_ENABLE_KTLS
    std::uint64_t options = SSL_OP_NO_COMPRESSION | SSL_OP_CIPHER_SERVER_PREFERENCE;
    if(sslConfig.enableKTLS)
        options |= SSL_OP_ENABLE_KTLS;
    
    std::uint64_t appliedOptions = SSL_CTX_set_options(ctx, options);
    
    // Check if KTLS is really enabled
    if(appliedOptions & SSL_OP_ENABLE_KTLS)
        logger.Info("[HttpOpenSSL]: KTLS enabled for this SSL_CTX");
    else if(sslConfig.enableKTLS)
        logger.Warn("[HttpOpenSSL]: KTLS requested but not enabled (kernel/OpenSSL limitation)");
#else
    if(sslConfig.enableKTLS)
        logger.Warn("[HttpOpenSSL]: KTLS requested but not supported by this OpenSSL build");
#endif

    logger.Info("[HttpOpenSSL]: SSL context initialized successfully");
}

HttpOpenSSL::~HttpOpenSSL()
{
    if(ctx) {
        SSL_CTX_free(ctx);
        ctx = nullptr;
    }

    Logger::GetInstance().Info("[HttpOpenSSL]: Successfully cleaned up SSL context");
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

    return (r == 1);
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

SSLShutdownResult HttpOpenSSL::Shutdown(void* conn)
{
    SSL* ssl = static_cast<SSL*>(conn);
    if(!ssl)
        return SSLShutdownResult::DONE;

    int ret = SSL_shutdown(ssl);

    // SSL_shutdown return values:
    // 1  = success (both sides notified)
    // 0  = shutdown sent, waiting for peer
    // <0 = error, check SSL_get_error()
    if(ret == 1) {
        SSL_free(ssl);
        return SSLShutdownResult::DONE;
    }

    if(ret == 0)
        return SSLShutdownResult::WANT_READ;

    int err = SSL_get_error(ssl, ret);
    if(err == SSL_ERROR_WANT_READ)
        return SSLShutdownResult::WANT_READ;
    if(err == SSL_ERROR_WANT_WRITE)
        return SSLShutdownResult::WANT_WRITE;

    // Any other fatal error
    SSL_free(ssl);
    return SSLShutdownResult::FAILED;
}

// vvv Helper functions vvv
void HttpOpenSSL::GlobalOpenSSLInit()
{
    static std::once_flag initFlag;
    std::call_once(initFlag, []() {
        if(OPENSSL_init_ssl(OPENSSL_INIT_LOAD_CONFIG, nullptr) != 1)
            Logger::GetInstance().Fatal("[HttpOpenSSL]: Initialization failed");
    });
}

} // namespace WFX::Http

#endif // WFX_HTTP_USE_OPENSSL