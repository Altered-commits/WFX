#ifndef WFX_HTTP_SSL_HPP
#define WFX_HTTP_SSL_HPP

// Windows socket is diferent from Linux's socket definiton
// I have it defined in connection/http_connection.hpp but doing it here-
// -again, not the best way to do it but, yeah
#ifdef _WIN32
    #include <winsock2.h>
    using SSLSocket = SOCKET;
#else
    using SSLSocket = int;
#endif // _WIN32

#include <cstdint>

namespace WFX::Http {

// Common return values for Read / Write errors
enum class SSLError : std::uint8_t {
    SUCCESS,
    WANT_READ,
    WANT_WRITE,
    CLOSED,
    SYSCALL,
    FATAL
};

struct SSLResult {
    SSLError error;
    int      res;
};

// Interface around SSL implementations
class HttpWFXSSL {
public:
    virtual ~HttpWFXSSL() = default;

    // Wrap a socket and return opaque handle
    virtual void* Wrap(SSLSocket fd) = 0;

    // Handshake; returns true if done
    virtual bool Handshake(void* conn) = 0;

    // Read/Write functions
    virtual SSLResult Read(void* conn, char* buf, int len) = 0;
    virtual SSLResult Write(void* conn, const char* buf, int len) = 0;

    // Shutdown and Free connection
    virtual void Shutdown(void* conn) = 0;
};

} // namespace WFX::Http

#endif // WFX_HTTP_SSL_HPP