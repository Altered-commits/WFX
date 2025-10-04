#ifndef WFX_HTTP_SSL_HPP
#define WFX_HTTP_SSL_HPP

// Windows socket is diferent from Linux's socket definiton
// I have it defined in connection/http_connection.hpp but doing it here-
// -again, not the best way to do it but, yeah
#ifdef _WIN32
    #include <winsock2.h>
    using SSLSocket  = SOCKET;
    using FileOffset = std::int64_t;
    using ReturnType = std::int64_t;
#else
    #include <sys/types.h>
    using SSLSocket = int;
    using FileOffset = off_t;
    using ReturnType = ssize_t;
#endif // _WIN32

#include <cstdint>

namespace WFX::Http {

// Common return values for Read / Write errors
enum class SSLReturn : std::uint8_t {
    SUCCESS,
    WANT_READ,
    WANT_WRITE,
    CLOSED,
    SYSCALL,
    FATAL
};

struct SSLResult {
    SSLReturn  error;
    ReturnType res;
};

// Interface around SSL implementations
class HttpWFXSSL {
public:
    virtual ~HttpWFXSSL() = default;

    // Wrap a socket and return opaque handle
    virtual void* Wrap(SSLSocket fd) = 0;

    // Handshake; returns true if done
    virtual SSLReturn Handshake(void* conn) = 0;

    // Read/Write functions
    virtual SSLResult Read(void* conn, char* buf, int len)                                      = 0;
    virtual SSLResult Write(void* conn, const char* buf, int len)                               = 0;
    virtual SSLResult WriteFile(void* conn, SSLSocket fd, FileOffset offset, std::size_t count) = 0;

    // Shutdown and Free connection
    virtual SSLReturn Shutdown(void* conn)      = 0;
    virtual SSLReturn ForceShutdown(void* conn) = 0;
};

} // namespace WFX::Http

#endif // WFX_HTTP_SSL_HPP