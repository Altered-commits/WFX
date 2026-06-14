// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifdef WFX_HTTP_USE_OPENSSL

#include "http_openssl.hpp"
#include "config/config.hpp"
#include "utils/hash/hash.hpp"
#include "utils/diagnostics/logger.hpp"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <algorithm>

namespace WFX::Http {

using namespace WFX::Utils; // For 'Logger'
using namespace WFX::Core;  // For 'Config'

// vvv Constants vvv
static constexpr unsigned char ALPN_PROTOS[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

// vvv Constructors and Destructors vvv
HttpOpenSSL::HttpOpenSSL()
{
    auto& logger = GetLogger();
    auto& sslConfig = GetConfig().sslConfig;

    GlobalOpenSSLInit();

    InitServerContext();
    InitClientContext();

    logger.Info("[HttpOpenSSL]: SSL context initialized successfully");
}

HttpOpenSSL::~HttpOpenSSL()
{
    if(serverCtx) {
        SSL_CTX_free(serverCtx);
        serverCtx = nullptr;
    }
    if(clientCtx) {
        SSL_CTX_free(clientCtx);
        clientCtx = nullptr;
    }

    GetLogger().Info("[HttpOpenSSL]: Successfully cleaned up SSL context");
}

void HttpOpenSSL::InitServerContext()
{
    auto& logger = GetLogger();
    auto& sslConfig = GetConfig().sslConfig;

    serverCtx = SSL_CTX_new(TLS_method());
    if(!serverCtx)
        logger.Fatal("[HttpOpenSSL]: Failed to create server SSL_CTX");

    SSL_CTX_set_security_level(serverCtx, std::clamp(sslConfig.securityLevel, 0, 5));

    // Minimum protocol version
    int protoVersion = TLS1_2_VERSION;
    switch(sslConfig.minProtoVersion) {
        case 1:
            protoVersion = TLS1_VERSION;
            break;
        case 2:
            protoVersion = TLS1_2_VERSION;
            break;
        case 3:
            protoVersion = TLS1_3_VERSION;
            break;
        default:
            protoVersion = TLS1_2_VERSION;
            break;
    }
    if(SSL_CTX_set_min_proto_version(serverCtx, protoVersion) != 1)
        LogOpenSSLError("Failed to set minimum TLS protocol version for server ctx");

    // Server cert and key. ONLY on serverCtx, loading this on clientCtx would-
    // -pollute the verify store and break outbound client certificate verification
    if(SSL_CTX_use_certificate_chain_file(serverCtx, sslConfig.certPath.c_str()) <= 0)
        LogOpenSSLError("Failed to load certificate chain file");

    if(SSL_CTX_use_PrivateKey_file(serverCtx, sslConfig.keyPath.c_str(), SSL_FILETYPE_PEM) <= 0)
        LogOpenSSLError("Failed to load private key");

    if(!SSL_CTX_check_private_key(serverCtx))
        LogOpenSSLError("Private key does not match certificate");

    // Server-side session cache: server automatically stores and looks up sessions
    // SSL_SESS_CACHE_SERVER is the correct mode for inbound connections
    if(sslConfig.enableServerSessionCache) {
        SSL_CTX_set_session_cache_mode(serverCtx, SSL_SESS_CACHE_SERVER);
        SSL_CTX_sess_set_cache_size(serverCtx, sslConfig.serverSessionCacheSize);
    }
    else
        SSL_CTX_set_session_cache_mode(serverCtx, SSL_SESS_CACHE_OFF);

    // Session tickets: server-side only, server encrypts the ticket blob sent to client
    // Client just stores and presents it back. Client has no ticket key to configure
    auto& ticketKey = GetRandomPool().GetSSLKey();
    if(SSL_CTX_set_tlsext_ticket_keys(serverCtx, ticketKey.data(), ticketKey.size()) != 1)
        LogOpenSSLError("Failed to set session ticket keys");

    // Cipher preferences
    if(!sslConfig.tls13Ciphers.empty() && SSL_CTX_set_ciphersuites(serverCtx, sslConfig.tls13Ciphers.c_str()) != 1)
        LogOpenSSLError("Failed to set TLSv1.3 ciphersuites for server ctx");

    if(!sslConfig.tls12Ciphers.empty() && SSL_CTX_set_cipher_list(serverCtx, sslConfig.tls12Ciphers.c_str()) != 1)
        LogOpenSSLError("Failed to set TLSv1.2 cipher list for server ctx");

    if(!sslConfig.curves.empty())
        SSL_CTX_set1_curves_list(serverCtx, sslConfig.curves.c_str());

    SSL_CTX_set_read_ahead(serverCtx, 0);
    SSL_CTX_set_mode(serverCtx,
                     SSL_MODE_RELEASE_BUFFERS | SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    // SSL_OP_CIPHER_SERVER_PREFERENCE: server-side only, meaningless on client
    // SSL_OP_NO_COMPRESSION: both sides benefit but server enforcing is sufficient
    // SSL_OP_ENABLE_KTLS: server-side only, enables SSL_sendfile for file serving
    std::uint64_t options = SSL_OP_NO_COMPRESSION | SSL_OP_CIPHER_SERVER_PREFERENCE;

#ifdef SSL_OP_ENABLE_KTLS
    if(sslConfig.enableKTLS)
        options |= SSL_OP_ENABLE_KTLS;
#else
    if(sslConfig.enableKTLS)
        logger.Warn("[HttpOpenSSL]: KTLS requested but not supported by this OpenSSL build");
#endif

    std::uint64_t appliedOptions = SSL_CTX_set_options(serverCtx, options);

#ifdef SSL_OP_ENABLE_KTLS
    if(appliedOptions & SSL_OP_ENABLE_KTLS) {
        useKtls = true;
        logger.Info("[HttpOpenSSL]: KTLS enabled for server SSL_CTX");
    }
    else if(sslConfig.enableKTLS)
        logger.Warn("[HttpOpenSSL]: KTLS requested but not enabled (kernel/OpenSSL limitation)");
#else
    (void)appliedOptions;
#endif

    // ALPN select callback: server-side only
    // SSL_CTX_set_alpn_select_cb is called when server needs to pick from client's offered list
    // SSL_CTX_set_alpn_protos is client-side only (advertises to server, no effect on server)
    SSL_CTX_set_alpn_select_cb(
        serverCtx,
        [](SSL*, const unsigned char** out, unsigned char* outlen, const unsigned char* in, unsigned int inlen,
           void*) -> int {
            if(SSL_select_next_proto(const_cast<unsigned char**>(out), outlen, ALPN_PROTOS, sizeof(ALPN_PROTOS), in,
                                     inlen) == OPENSSL_NPN_NEGOTIATED)
                return SSL_TLSEXT_ERR_OK;

            // Client didn't offer http/1.1 but don't reject. Accept without ALPN
            // This keeps plain TLS clients (curl, etc.) working
            return SSL_TLSEXT_ERR_NOACK;
        },
        nullptr);
}

void HttpOpenSSL::InitClientContext()
{
    auto& logger = GetLogger();
    auto& sslConfig = GetConfig().sslConfig;

    clientCtx = SSL_CTX_new(TLS_method());
    if(!clientCtx)
        logger.Fatal("[HttpOpenSSL]: Failed to create client SSL_CTX");

    // Always load system CAs so public internet endpoints verify correctly
    // If user also specifies ca_cert_path (for internal/self-signed CAs like mkcert),-
    // -it is added on top
    if(SSL_CTX_set_default_verify_paths(clientCtx) != 1)
        LogOpenSSLError("Failed to load default system CA certificates for client ctx");

    if(!sslConfig.caCertPath.empty()) {
        if(SSL_CTX_load_verify_locations(clientCtx, sslConfig.caCertPath.c_str(), nullptr) != 1)
            LogOpenSSLError("Failed to load configured CA certificate for client ctx");
    }

    // Minimum security version
    SSL_CTX_set_security_level(clientCtx, std::clamp(sslConfig.securityLevel, 0, 5));

    // Minimum protocol version
    int protoVersion = TLS1_2_VERSION;
    switch(sslConfig.minProtoVersion) {
        case 1:
            protoVersion = TLS1_VERSION;
            break;
        case 2:
            protoVersion = TLS1_2_VERSION;
            break;
        case 3:
            protoVersion = TLS1_3_VERSION;
            break;
        default:
            protoVersion = TLS1_2_VERSION;
            break;
    }
    if(SSL_CTX_set_min_proto_version(clientCtx, protoVersion) != 1)
        LogOpenSSLError("Failed to set minimum TLS protocol version for client ctx");

    // Client-side session cache: SSL_SESS_CACHE_CLIENT stores sessions for reuse
    // Unlike server cache, client must explicitly call SSL_set_session() to reuse
    // The engine does not do automatic reuse, but having the cache ready is correct
    if(sslConfig.enableClientSessionCache) {
        SSL_CTX_set_session_cache_mode(clientCtx, SSL_SESS_CACHE_CLIENT);
        SSL_CTX_sess_set_cache_size(clientCtx, sslConfig.clientSessionCacheSize);
    }
    else
        SSL_CTX_set_session_cache_mode(clientCtx, SSL_SESS_CACHE_OFF);

    // Client advertises these to the server as supported suites
    // Server's SSL_OP_CIPHER_SERVER_PREFERENCE will override order on the server side-
    // -but we still want to advertise only strong suites
    if(!sslConfig.tls13Ciphers.empty() && SSL_CTX_set_ciphersuites(clientCtx, sslConfig.tls13Ciphers.c_str()) != 1)
        LogOpenSSLError("Failed to set TLSv1.3 ciphersuites for client ctx");

    if(!sslConfig.tls12Ciphers.empty() && SSL_CTX_set_cipher_list(clientCtx, sslConfig.tls12Ciphers.c_str()) != 1)
        LogOpenSSLError("Failed to set TLSv1.2 cipher list for client ctx");

    if(!sslConfig.curves.empty())
        SSL_CTX_set1_curves_list(clientCtx, sslConfig.curves.c_str());

    SSL_CTX_set_read_ahead(clientCtx, 0);
    SSL_CTX_set_mode(clientCtx,
                     SSL_MODE_RELEASE_BUFFERS | SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    // SSL_OP_NO_COMPRESSION: applies to both sides, client enforcing prevents
    // CRIME attack on outbound connections regardless of what server advertises
    // SSL_OP_CIPHER_SERVER_PREFERENCE: NOT set, meaningless on client side,-
    // -only the server uses this to override the negotiated cipher order
    // SSL_OP_ENABLE_KTLS: NOT set, KTLS is for SSL_sendfile which is server-side only
    SSL_CTX_set_options(clientCtx, SSL_OP_NO_COMPRESSION);

    logger.Info("[HttpOpenSSL]: Client SSL context initialized successfully");
}

// vvv Main Functions vvv
void* HttpOpenSSL::Wrap(SSLSocket sock)
{
    SSL* ssl = SSL_new(serverCtx);
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

    // We are the server and we are accepting connections
    // So tell OpenSSL this connection is supposed to be accepted
    SSL_set_accept_state(ssl);

    return ssl;
}

void* HttpOpenSSL::WrapClient(SSLSocket sock, const char* host)
{
    SSL* ssl = SSL_new(clientCtx);
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
    // Instruct OpenSSL to initiate the 'ClientHello'
    SSL_set_connect_state(ssl);

    // Enforce strict peer verification. Without this, encryption is useless against MitM attack
    SSL_set_verify(ssl, SSL_VERIFY_PEER, nullptr);

    X509_VERIFY_PARAM* param = SSL_get0_param(ssl);

    // RFC 6066 states SNI must ONLY be sent for hostnames, never literal IPs
    // We attempt to parse 'hostname' as an IP. If it succeeds, its an IP
    if(X509_VERIFY_PARAM_set1_ip_asc(param, host) == 1) {
        // Target is an IP Address:
        // Do NOT send SNI. Verification is strictly checked against the IP
    }
    else {
        // Target is a Domain Name:
        // Set Server Name Indication (SNI)
        if(SSL_set_tlsext_host_name(ssl, host) != 1) {
            SSL_free(ssl);
            return nullptr;
        }

        // X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS prevents attackers from using
        // wildcard certs (e.g., *.*.com) to impersonate your target.
        X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);

        // Register the expected domain name
        if(X509_VERIFY_PARAM_set1_host(param, host, 0) != 1) {
            SSL_free(ssl);
            return nullptr;
        }
    }

    // ALPN: SSL_set_alpn_protos is client side per-connection advertisement
    if(SSL_set_alpn_protos(ssl, ALPN_PROTOS, sizeof(ALPN_PROTOS)) != 0) {
        SSL_free(ssl);
        return nullptr;
    }

    return ssl;
}

SSLReturn HttpOpenSSL::Handshake(void* conn)
{
    SSL* ssl = static_cast<SSL*>(conn);
    int ret = SSL_do_handshake(ssl);

    // Handshake complete
    if(ret == 1)
        return SSLReturn::SUCCESS;

    int err = SSL_get_error(ssl, ret);
    switch(err) {
        case SSL_ERROR_WANT_READ:
            return SSLReturn::WANT_READ;
        case SSL_ERROR_WANT_WRITE:
            return SSLReturn::WANT_WRITE;
        case SSL_ERROR_ZERO_RETURN:
            return SSLReturn::CLOSED;
        case SSL_ERROR_SYSCALL:
            return SSLReturn::SYSCALL;
        default:
            return SSLReturn::FATAL;
    }
}

SSLResult HttpOpenSSL::Read(void* conn, char* buf, int len)
{
    SSL* ssl = static_cast<SSL*>(conn);
    int ret = SSL_read(ssl, buf, len);

    if(ret > 0)
        return {SSLReturn::SUCCESS, ret};

    int err = SSL_get_error(ssl, ret);
    switch(err) {
        case SSL_ERROR_WANT_READ:
            return {SSLReturn::WANT_READ, 0};
        case SSL_ERROR_WANT_WRITE:
            return {SSLReturn::WANT_WRITE, 0};
        case SSL_ERROR_ZERO_RETURN:
            return {SSLReturn::CLOSED, 0};
        case SSL_ERROR_SYSCALL:
            return {SSLReturn::SYSCALL, 0};
        default:
            return {SSLReturn::FATAL, 0};
    }
}

SSLResult HttpOpenSSL::Write(void* conn, const char* buf, int len)
{
    SSL* ssl = static_cast<SSL*>(conn);
    int ret = SSL_write(ssl, buf, len);

    if(ret > 0)
        return {SSLReturn::SUCCESS, ret};

    int err = SSL_get_error(ssl, ret);
    switch(err) {
        case SSL_ERROR_WANT_READ:
            return {SSLReturn::WANT_READ, 0};
        case SSL_ERROR_WANT_WRITE:
            return {SSLReturn::WANT_WRITE, 0};
        case SSL_ERROR_ZERO_RETURN:
            return {SSLReturn::CLOSED, 0};
        case SSL_ERROR_SYSCALL:
            return {SSLReturn::SYSCALL, 0};
        default:
            return {SSLReturn::FATAL, 0};
    }
}

SSLResult HttpOpenSSL::WriteFile(void* conn, SSLSocket fd, FileOffset offset, std::size_t count)
{
    // Windows version does not contain SSL_sendfile, we need to use Write to send files
#ifdef _WIN32
    return {SSLReturn::NO_IMPL, 0};
#else
    // SSL_sendfile can only be used with ktls enabled
    // Return 'NO_IMPL' to tell backend to switch to using Write
    if(!useKtls)
        return {SSLReturn::NO_IMPL, 0};

    SSL* ssl = static_cast<SSL*>(conn);
    ssize_t ret = SSL_sendfile(ssl, fd, offset, count, 0);

    if(ret > 0)
        return {SSLReturn::SUCCESS, ret};

    int err = SSL_get_error(ssl, static_cast<int>(ret));
    switch(err) {
        case SSL_ERROR_WANT_READ:
            return {SSLReturn::WANT_READ, 0};
        case SSL_ERROR_WANT_WRITE:
            return {SSLReturn::WANT_WRITE, 0};
        case SSL_ERROR_ZERO_RETURN:
            return {SSLReturn::CLOSED, 0};
        case SSL_ERROR_SYSCALL:
            return {SSLReturn::SYSCALL, 0};
        default:
            return {SSLReturn::FATAL, 0};
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

SSLReturn HttpOpenSSL::ForceShutdown(void* conn)
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
    static bool initialized = false;
    if(initialized)
        return;

    if(OPENSSL_init_ssl(OPENSSL_INIT_LOAD_CONFIG, nullptr) != 1)
        GetLogger().Fatal("[HttpOpenSSL]: Initialization failed");

    initialized = true;
}

void HttpOpenSSL::LogOpenSSLError(const char* message, SSL* ssl, bool fatal)
{
    std::string details;

    // Certificate verification failures are the most useful thing to report
    if(ssl) {
        const long verifyResult = SSL_get_verify_result(ssl);

        if(verifyResult != X509_V_OK) {
            const char* verifyReason = X509_verify_cert_error_string(verifyResult);
            details = verifyReason ? verifyReason : "unknown certificate verification failure";
        }
    }

    // Fall back to OpenSSL error queue if no verification issue exists
    if(details.empty()) {
        if(unsigned long err = ERR_peek_last_error(); err != 0) {
            if(const char* reason = ERR_reason_error_string(err))
                details = reason;
            else
                details = "unknown OpenSSL error";
        }
    }

    // Always clear the thread-local error queue
    while(ERR_get_error() != 0) {
    }

    std::string fullMessage(message);

    if(!details.empty()) {
        fullMessage += ". ";
        fullMessage += details;
    }

    if(fatal)
        GetLogger().Fatal("[HttpOpenSSL]: ", fullMessage);
    else
        GetLogger().Error("[HttpOpenSSL]: ", fullMessage);
}

} // namespace WFX::Http

#endif // WFX_HTTP_USE_OPENSSL