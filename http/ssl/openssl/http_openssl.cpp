#ifdef WFX_HTTP_USE_OPENSSL

#include "http_openssl.hpp"
#include "config/config.hpp"
#include "http/common/http_global_state.hpp"
#include "utils/logger/logger.hpp"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <algorithm>

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
        LogOpenSSLError("Failed to set minimum TLS protocol version");

    // Load certificate and private key
    if(SSL_CTX_use_certificate_chain_file(ctx, sslConfig.certPath.c_str()) <= 0)
        LogOpenSSLError("Failed to load certificate chain file");
    
    if(SSL_CTX_use_PrivateKey_file(ctx, sslConfig.keyPath.c_str(), SSL_FILETYPE_PEM) <= 0)
        LogOpenSSLError("Failed to load private key");
    
    if(!SSL_CTX_check_private_key(ctx))
        LogOpenSSLError("Private key does not match certificate");

    // Server side session caching
    if(sslConfig.enableSessionCache) {
        SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);
        SSL_CTX_sess_set_cache_size(ctx, sslConfig.sessionCacheSize);
    }

    auto& ticketKey = GetGlobalState().sslKey;
    if(SSL_CTX_set_tlsext_ticket_keys(ctx, ticketKey.data(), ticketKey.size()) != 1)
        LogOpenSSLError("Failed to set session ticket keys");
    
    // Set modern cipher preferences
    if(!sslConfig.tls13Ciphers.empty() && SSL_CTX_set_ciphersuites(ctx, sslConfig.tls13Ciphers.c_str()) != 1)
        LogOpenSSLError("Failed to set TLSv1.3 ciphersuites");

    if(!sslConfig.tls12Ciphers.empty() && SSL_CTX_set_cipher_list(ctx, sslConfig.tls12Ciphers.c_str()) != 1)
        LogOpenSSLError("Failed to set TLSv1.2 cipher list");

    // Set remaining essential options
    if(!sslConfig.curves.empty())
        SSL_CTX_set1_curves_list(ctx, sslConfig.curves.c_str());

    // Disable OpenSSL's internal read ahead buffer. We manage our own buffers
    SSL_CTX_set_read_ahead(ctx, 0);

    SSL_CTX_set_mode(ctx,
        SSL_MODE_RELEASE_BUFFERS |
        SSL_MODE_ENABLE_PARTIAL_WRITE |
        SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER
    );

    std::uint64_t options = SSL_OP_NO_COMPRESSION | SSL_OP_CIPHER_SERVER_PREFERENCE;

#ifdef SSL_OP_ENABLE_KTLS
    if(sslConfig.enableKTLS)
        options |= SSL_OP_ENABLE_KTLS;
#else
    if(sslConfig.enableKTLS)
        logger.Warn("[HttpOpenSSL]: KTLS requested but not supported by this OpenSSL build");
#endif

    std::uint64_t appliedOptions = SSL_CTX_set_options(ctx, options);

#ifdef SSL_OP_ENABLE_KTLS
    // Check if KTLS is really enabled
    if(appliedOptions & SSL_OP_ENABLE_KTLS) {
        useKtls = true;
        logger.Info("[HttpOpenSSL]: KTLS enabled for this SSL_CTX");
    }
    else if(sslConfig.enableKTLS)
        logger.Warn("[HttpOpenSSL]: KTLS requested but not enabled (kernel/OpenSSL limitation)");
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

SSLReturn HttpOpenSSL::Handshake(void* conn)
{
    SSL* ssl = static_cast<SSL*>(conn);
    int  ret = SSL_accept(ssl);

    // Handshake complete
    if(ret == 1)
        return SSLReturn::SUCCESS;

    int err = SSL_get_error(ssl, ret);
    switch(err) {
        case SSL_ERROR_WANT_READ:   return SSLReturn::WANT_READ;
        case SSL_ERROR_WANT_WRITE:  return SSLReturn::WANT_WRITE;
        case SSL_ERROR_ZERO_RETURN: return SSLReturn::CLOSED;
        case SSL_ERROR_SYSCALL:     return SSLReturn::SYSCALL;
        default:                    return SSLReturn::FATAL;
    }
}

SSLResult HttpOpenSSL::Read(void* conn, char* buf, int len)
{
    SSL* ssl = static_cast<SSL*>(conn);
    int  ret = SSL_read(ssl, buf, len);

    if(ret > 0)
        return { SSLReturn::SUCCESS, ret };

    int err = SSL_get_error(ssl, ret);
    switch(err) {
        case SSL_ERROR_WANT_READ:   return { SSLReturn::WANT_READ,  0 };
        case SSL_ERROR_WANT_WRITE:  return { SSLReturn::WANT_WRITE, 0 };
        case SSL_ERROR_ZERO_RETURN: return { SSLReturn::CLOSED,     0 };
        case SSL_ERROR_SYSCALL:     return { SSLReturn::SYSCALL,    0 };
        default:                    return { SSLReturn::FATAL,      0 };
    }
}

SSLResult HttpOpenSSL::Write(void* conn, const char* buf, int len)
{
    SSL* ssl = static_cast<SSL*>(conn);
    int  ret = SSL_write(ssl, buf, len);

    if(ret > 0)
        return { SSLReturn::SUCCESS, ret };

    int err = SSL_get_error(ssl, ret);
    switch(err) {
        case SSL_ERROR_WANT_READ:   return { SSLReturn::WANT_READ,  0 };
        case SSL_ERROR_WANT_WRITE:  return { SSLReturn::WANT_WRITE, 0 };
        case SSL_ERROR_ZERO_RETURN: return { SSLReturn::CLOSED,     0 };
        case SSL_ERROR_SYSCALL:     return { SSLReturn::SYSCALL,    0 };
        default:                    return { SSLReturn::FATAL,      0 };
    }
}

SSLResult HttpOpenSSL::WriteFile(void* conn, SSLSocket fd, FileOffset offset, std::size_t count)
{
    // Windows version does not contain SSL_sendfile, we need to use Write to send files
#ifdef _WIN32
    static_assert(false, "Implement HttpOpenSSL.WriteFile function for Windows");
#else
    // SSL_sendfile can only be used with ktls enabled
    if(!useKtls) {
        Logger::GetInstance()
            .Error("[HttpOpenSSL]: Enable KTLS, WriteFile does not have a backup implementation rn");
        return { SSLReturn::FATAL, 0 };
    }

    SSL*    ssl = static_cast<SSL*>(conn);
    ssize_t ret = SSL_sendfile(ssl, fd, offset, count, 0);

    if(ret > 0)
        return { SSLReturn::SUCCESS, ret };

    int err = SSL_get_error(ssl, static_cast<int>(ret));
    switch(err) {
        case SSL_ERROR_WANT_READ:   return { SSLReturn::WANT_READ,  0 };
        case SSL_ERROR_WANT_WRITE:  return { SSLReturn::WANT_WRITE, 0 };
        case SSL_ERROR_ZERO_RETURN: return { SSLReturn::CLOSED,     0 };
        case SSL_ERROR_SYSCALL:     return { SSLReturn::SYSCALL,    0 };
        default:                    return { SSLReturn::FATAL,      0 };
    }
#endif
}

SSLReturn HttpOpenSSL::Shutdown(void* conn)
{
    SSL* ssl = static_cast<SSL*>(conn);
    if(!ssl)
        return SSLReturn::SUCCESS;

    int ret = SSL_shutdown(ssl);

    // SSL_shutdown return values:
    // 1  = success (both sides notified)
    // 0  = shutdown sent, waiting for peer
    // <0 = error, check SSL_get_error()
    if(ret == 1) {
        SSL_free(ssl);
        return SSLReturn::SUCCESS;
    }

    if(ret == 0)
        return SSLReturn::WANT_READ;

    int err = SSL_get_error(ssl, ret);
    if(err == SSL_ERROR_WANT_READ)
        return SSLReturn::WANT_READ;
    if(err == SSL_ERROR_WANT_WRITE)
        return SSLReturn::WANT_WRITE;

    // Any other fatal error
    SSL_free(ssl);
    return SSLReturn::FATAL;
}

SSLReturn HttpOpenSSL::ForceShutdown(void *conn)
{
    SSL* ssl = static_cast<SSL*>(conn);
    if(!ssl)
        return SSLReturn::SUCCESS;

    // Skip proper TLS shutdown
    SSL_set_shutdown(ssl, SSL_SENT_SHUTDOWN | SSL_RECEIVED_SHUTDOWN);

    // Free SSL object and indicate abrupt shutdown
    SSL_free(ssl);
    return SSLReturn::FATAL;
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

void HttpOpenSSL::LogOpenSSLError(const char* message, bool fatal)
{
    std::string allErrors;
    unsigned long errCode;
    char errBuf[256];

    while((errCode = ERR_get_error()) != 0) {
        ERR_error_string_n(errCode, errBuf, sizeof(errBuf));
        if(!allErrors.empty())
            allErrors.append("; ");
        allErrors.append(errBuf);
    }

    std::string fullMessage = allErrors.empty()
        ? std::string(message) + ". No specific OpenSSL error code available"
        : std::string(message) + ". OpenSSL Reason(s): " + allErrors;

    if(fatal)
        Logger::GetInstance().Fatal("[HttpOpenSSL]: ", fullMessage);
    else
        Logger::GetInstance().Error("[HttpOpenSSL]: ", fullMessage);
}

} // namespace WFX::Http

#endif // WFX_HTTP_USE_OPENSSL