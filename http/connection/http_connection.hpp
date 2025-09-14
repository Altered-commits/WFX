#ifndef WFX_HTTP_CONNECTION_HANDLER_HPP
#define WFX_HTTP_CONNECTION_HANDLER_HPP

#include "http/request/http_request.hpp"
#include "utils/backport/move_only_function.hpp"
#include "utils/crypt/hash.hpp"
#include "utils/rw_buffer/rw_buffer.hpp"

#include <string>

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

    uint8_t ipType = 255; // AF_INET or AF_INET6

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
    PARSE_IDLE                // After Request-Response cycle, waiting for another request
};

enum class EventType : std::uint8_t {
    EVENT_ACCEPT,
    EVENT_RECV,
    EVENT_SEND,
    EVENT_SEND_FILE
};

enum class ConnectionState : std::uint8_t {
    CONNECTION_ALIVE,
    CONNECTION_CLOSE
};

// Forward declare it so compilers won't cry
struct ConnectionContext;

using WFX::Utils::MoveOnlyFunction;

using ReceiveCallback            = MoveOnlyFunction<void(ConnectionContext*)>;
using AcceptedConnectionCallback = MoveOnlyFunction<void()>;
using HttpRequestPtr             = std::unique_ptr<HttpRequest>;

// Honestly, while it would've been easier to include tick_scheduler.hpp for TickScheduler::TickType
// Eh, idk cluttering this file just for a type, i'm just going to redefine it here for no absolute reason
using HttpTickType = std::uint16_t;

struct ConnectionContext {
    // First 8 bytes, eventType must strictly be at offset of 0
    EventType eventType = EventType::EVENT_ACCEPT;        // 1 byte

    struct {
        std::uint8_t parseState      : 4; // 0–15 (Enough for HttpParseState)
        std::uint8_t connectionState : 4; // 0–15 (Enough for ConnectionState)
    };                                                   // 1 byte

    std::uint16_t timeoutTick = 0;                       // 2 bytes
    std::uint32_t trackBytes  = 0;                       // 4 bytes

    // 16-byte buffer
    WFX::Utils::RWBuffer rwBuffer;

    HttpRequestPtr requestInfo;                        // 8 bytes
    WFXSocket      socket;                             // 4 bytes
    std::uint32_t  expectedBodyLength = 0;             // 4 bytes
    WFXIpAddress   connInfo;                           // 20 bytes

public: // Helper functions
    void ResetContext();

    void SetParseState(HttpParseState newState);
    void SetConnectionState(ConnectionState newState);

    HttpParseState  GetParseState()      const;
    ConnectionState GetConnectionState() const;
};
static_assert(offsetof(ConnectionContext, eventType) == 0, "[ConnectionContext] 'eventType' must strictly be at an offset of 0.");
static_assert(sizeof(ConnectionContext) <= 80, "ConnectionContext must STRICTLY be less than or equal to 80 bytes.");

// Abstraction for Windows and Linux impl
class HttpConnectionHandler {
public:
    virtual ~HttpConnectionHandler() = default;

    // Initialize sockets, bind and listen on given host:port
    virtual void Initialize(const std::string& host, int port) = 0;

    // Set the receive callback ONCE per socket (can be overwritten if needed)
    virtual void SetReceiveCallback(ReceiveCallback onData) = 0;

    // Read more data if required (Async)
    virtual void ResumeReceive(ConnectionContext* ctx) = 0;

    // Write data to socket (Async)
    virtual void Write(ConnectionContext* ctx, std::string_view buffer = {}) = 0;

    // Write file directly to sockets (Async)
    virtual void WriteFile(ConnectionContext* ctx, std::string_view path) = 0;

    // Close a client socket
    virtual void Close(ConnectionContext* ctx) = 0;

    // Run the main connection loop (can be used by dev/serve mode)
    virtual void Run() = 0;

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