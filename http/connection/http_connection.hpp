#ifndef WFX_HTTP_CONNECTION_HANDLER_HPP
#define WFX_HTTP_CONNECTION_HANDLER_HPP

#include "http/request/http_request.hpp"

#include "utils/backport/move_only_function.hpp"
#include "utils/logger/logger.hpp"
#include "utils/crypt/hash.hpp"

#include <string>
#include <memory>
#include <atomic>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <WinSock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "Ws2_32.lib")

    using WFXSocket = SOCKET;
    constexpr WFXSocket WFX_INVALID_SOCKET = INVALID_SOCKET;
#else
    #include <netinet/in.h>  // in_addr, in6_addr
    #include <arpa/inet.h>   // inet_ntop, inet_pton

    using WFXSocket = int; // On Linux/Unix, sockets are file descriptors (ints)
    constexpr WFXSocket WFX_INVALID_SOCKET = -1;
#endif

namespace WFX::Http {

// Cross-Platform compatible Ip Struct
struct WFXIpAddress {
    union {
        in_addr  v4;
        in6_addr v6;
        uint8_t  raw[16]; // For hashing
    } ip;

    uint8_t ipType; // AF_INET or AF_INET6

    // Necessary operations
    WFXIpAddress& operator=(const WFXIpAddress& other);
    bool          operator==(const WFXIpAddress& other) const;

    // Helper functions
    std::string_view GetIpStr()  const;
    const char*      GetIpType() const;
};

// Might be weird to define it here but its important, these states are further used in-
// -both connection backend and parser so yeah
enum class HttpParseState : std::uint8_t {
    PARSE_INCOMPLETE_HEADERS, // Header end sequence (\r\n\r\n) not found yet
    PARSE_INCOMPLETE_BODY,    // Buffering body (Content-Length not fully received)
    
    PARSE_STREAMING_BODY,     // [Future] Streaming mode (body being processed in chunks)
    
    PARSE_EXPECT_100,         // It was a Expect: 100-continue header, accept it
    PARSE_EXPECT_417,         // It was a Expect: 100-continue header, REJECT IT
    PARSE_SUCCESS,            // Successfully received and parsed all data
    PARSE_ERROR,              // Malformed request
    PARSE_DATA_OCCUPIED,      // Data which parser 'parsed' is now being used in Request-Response cycle
    PARSE_IDLE                // After Request-Response cycle, waiting for another request
};

enum class HttpConnectionState : std::uint8_t {
    ACTIVE,            // Connection is actively running
    OCCUPIED,          // Connection is being processed by some callback and should not be closed
    CLOSING_DEFAULT,   // Connection will be closed shortly after processing request
    CLOSING_IMMEDIATE  // Connection must be closed immediately
};

// Forward declare it so compilers won't cry
struct ConnectionContext;

// For 'MoveOnlyFunction'
using WFX::Utils::MoveOnlyFunction;

using ReceiveCallback            = MoveOnlyFunction<void(ConnectionContext&)>;
using AcceptedConnectionCallback = MoveOnlyFunction<void(WFXSocket)>;
using HttpRequestPtr             = std::unique_ptr<HttpRequest>;
using ConnectionState            = std::atomic<HttpConnectionState>;

// Honestly, while it would've been easier to include tick_scheduler.hpp for TickScheduler::TickType
// Eh, idk cluttering this file just for a type, i'm just going to redefine it here for no absolute reason
using HttpTickType = std::uint16_t;

// Quite important, has to be 64 bytes and IS 64 BYTES, THIS CANNOT CHANGE NOW
struct ConnectionContext {
    char*         buffer     = nullptr;
    std::uint32_t bufferSize = 0;
    std::uint32_t dataLength = 0;
    
    // Also used by HttpParser
    std::uint32_t expectedBodyLength = 0;
    
    WFXIpAddress    connInfo;
    HttpRequestPtr  requestInfo;
    ReceiveCallback onReceive;

    // Used by HttpParser mostly
    std::uint8_t    parseState  = 0; // Interpreted by HttpParser as internal state enum
    ConnectionState connState   = HttpConnectionState::ACTIVE; // Track state to prevent race conditions
    std::uint16_t   timeoutTick = 0; // Used to track timeouts for various stuff like 'header' timeout or 'body' timeout
    std::uint32_t   trackBytes  = 0; // Misc. Tracking of bytes wherever necessary

public: // Core functions
    HttpConnectionState GetState();
    void                SetState(HttpConnectionState state);
    bool                TransitionTo(HttpConnectionState state);
};

// Abstraction for Windows and Linux impl
class HttpConnectionHandler {
public:
    virtual ~HttpConnectionHandler() = default;

    // Initialize sockets, bind and listen on given host:port
    virtual bool Initialize(const std::string& host, int port) = 0;

    // Set the receive callback ONCE per socket (can be overwritten if needed)
    virtual void SetReceiveCallback(WFXSocket socket, ReceiveCallback onData) = 0;

    // Read more data if required (Async)
    virtual void ResumeReceive(WFXSocket socket) = 0;

    // Write data to socket (Async)
    virtual int Write(WFXSocket socket, std::string_view buffer) = 0;

    // Write file directly to sockets (Async)
    virtual int WriteFile(WFXSocket socket, std::string&& header, std::string_view path) = 0;

    // Close a client socket
    virtual void Close(WFXSocket socket) = 0;

    // Run the main connection loop (can be used by dev/serve mode)
    virtual void Run(AcceptedConnectionCallback) = 0;

    // Get the current tick. Each of the connection backend will implement TickScheduler
    virtual HttpTickType GetCurrentTick() = 0;

    // Shutdown the main connection loop, cleanup everything
    virtual void Stop() = 0;
};

} // namespace WFX::Http

// Write a std::hash specialization for WFXIpAddress
namespace std {
    using namespace WFX::Utils; // For 'Logger' and 'RandomPool'
    using namespace WFX::Http;  // For 'WFXIpAddress'

    template<>
    struct hash<WFXIpAddress> {
        std::size_t operator()(const WFXIpAddress& addr) const
        {
            static std::uint8_t sipKey[16];
            
            // Run only once
            static const struct InitKeyOnce {
                InitKeyOnce()
                {
                    if(!RandomPool::GetInstance().GetBytes(sipKey, sizeof(sipKey)))
                        Logger::GetInstance().Fatal("[WFXIpAddressHash]: Failed to initialize SipHash key");
                }
            } _initOnce;

            return Hasher::SipHash24(
                addr.ip.raw,
                addr.ipType == AF_INET ? sizeof(in_addr) : sizeof(in6_addr),
                sipKey
            );
        }
    };
} // namespace std

#endif // WFX_HTTP_CONNECTION_HANDLER_HPP