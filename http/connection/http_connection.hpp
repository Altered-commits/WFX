#ifndef WFX_HTTP_CONNECTION_HANDLER_HPP
#define WFX_HTTP_CONNECTION_HANDLER_HPP

#include "http/request/http_request.hpp"
#include "http/common/http_route_common.hpp"
#include "utils/backport/move_only_function.hpp"
#include "utils/crypt/hash.hpp"
#include "utils/rw_buffer/rw_buffer.hpp"
#include "async/interface.hpp"

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
    EVENT_HANDSHAKE, // For SSL
    EVENT_RECV,
    EVENT_SEND,
    EVENT_SEND_FILE,
    EVENT_SHUTDOWN  // For SSL
};

enum class ConnectionState : std::uint8_t {
    CONNECTION_ALIVE,
    CONNECTION_CLOSE
};

// Forward declare it so compilers won't cry
struct ConnectionContext;

using CoroutineStack     = std::vector<Async::CoroutinePtr>;
using ReceiveCallback    = std::function<void(ConnectionContext*)>;
using CompletionCallback = std::function<void(ConnectionContext*)>;

struct FileInfo {
#if defined(_WIN32)
    HANDLE        handle{0};     // HANDLE is pointer-sized
    std::uint64_t fileSize{0};   // 64-bit for large files
    std::uint64_t offset{0};     // current send offset
#else
    int   fd       = -1;     // Linux file descriptor
    off_t fileSize = 0;      // File size
    off_t offset   = 0;      // current send offset
#endif
};

// Used inside of AsyncTrack if needed by 'HandleSuccess'
// Just an optimization so if we do have async code, we don't need to start from top again
enum ExecutionLevel : std::uint8_t {
    MIDDLEWARE,
    RESPONSE
};

// For async tracking without having to make engine / middleware async themselves
struct AsyncTrack {
    std::uint32_t trackBytes = 0;

    // Top 8 bits (bits 31..24) - addressable MiddlewareAction
    MiddlewareAction* GetMAction() { return reinterpret_cast<MiddlewareAction*>(&trackBytes); }

    // Bits 23..22 (2 bits) - Execution level
    ExecutionLevel GetELevel() const { return static_cast<ExecutionLevel>((trackBytes >> 22) & 0x3u); }
    void SetELevel(ExecutionLevel v) {
        trackBytes = (trackBytes & ~(0x3u << 22)) | ((static_cast<std::uint32_t>(v) & 0x3u) << 22);
    }

    // Bits 21..19 (3 bits) - Middleware type
    MiddlewareType GetMType() const { return static_cast<MiddlewareType>((trackBytes >> 19) & 0x7u); }
    void SetMType(MiddlewareType v) {
        trackBytes = (trackBytes & ~(0x7u << 19)) | ((static_cast<std::uint32_t>(v) & 0x7u) << 19);
    }

    // Bits 18..17 (2 bits) - Middleware level
    MiddlewareLevel GetMLevel() const { return static_cast<MiddlewareLevel>((trackBytes >> 17) & 0x3u); }
    void SetMLevel(MiddlewareLevel v) {
        trackBytes = (trackBytes & ~(0x3u << 17)) | ((static_cast<std::uint32_t>(v) & 0x3u) << 17);
    }

    // Bits 15..0 (16 bits) - Index
    std::uint16_t GetMIndex() const { return static_cast<std::uint16_t>(trackBytes & 0xFFFFu); }
    void SetMIndex(std::uint16_t idx) { trackBytes = (trackBytes & ~0xFFFFu) | (idx & 0xFFFFu); }
};

// Simply to assert that eventType must exist in anything related to connection-
// -and must be the first member as well (offset == 0)
struct ConnectionTag {
    EventType eventType = EventType::EVENT_ACCEPT;       // 1 byte
};

struct ConnectionContext : public ConnectionTag {
    // ------------------------------------------  // 1 byte from ConnectionTag
    bool handshakeDone = false;                    // 1 byte

    union {
        struct {
            std::uint16_t parseState            : 3;   // --
            std::uint16_t connectionState       : 2;   //  |
            std::uint16_t isStreamOperation     : 1;   //  |
            std::uint16_t isFileOperation       : 1;   //  |
            std::uint16_t isAsyncTimerOperation : 1;   //  |
            std::uint16_t isShuttingDown        : 1;   //  |
            std::uint16_t streamChunked         : 1;   //  |
            std::uint16_t __FPad                : 6;   //  V
        };                                             // 2 byte
        std::uint16_t __Flags = 0;
    };

    union {
        AsyncTrack    trackAsync;                  // |
        std::uint32_t trackBytes = 0;              // |-> 4 bytes (Used in HTTP parsing then async tracking if needed)
    };

    void*                sslConn       = nullptr;  // 8 bytes
    WFX::Utils::RWBuffer rwBuffer;                 // 16 bytes

    WFXSocket       socket             = -1;       // 4 | 8 bytes
    std::uint32_t   generationId       = 1;        // 4 bytes (0 is specially reserved)
                                                   // 4 bytes padded
    StreamGenerator streamGenerator    = {};       // 8 bytes
    HttpRequest*    requestInfo        = nullptr;  // 8 bytes
    HttpResponse*   responseInfo       = nullptr;  // 8 bytes (Async functions require larger scope)
    FileInfo*       fileInfo           = nullptr;  // 8 bytes
    WFXIpAddress    connInfo;                      // 20 bytes
    std::uint32_t   expectedBodyLength = 0;        // 4 bytes
    CoroutineStack  coroStack;                     // 24 bytes

public: // Helper functions
    void ResetContext();
    void ClearContext();

    void SetParseState(HttpParseState newState);
    void SetConnectionState(ConnectionState newState);

    HttpParseState  GetParseState()      const;
    ConnectionState GetConnectionState() const;

    bool IsAsyncOperation();
    bool TryFinishCoroutines();
};
static_assert(sizeof(ConnectionContext) <= 128, "ConnectionContext must STRICTLY be less than or equal to 128 bytes.");

// Abstraction for Windows and Linux impl
class HttpConnectionHandler {
public:
    virtual ~HttpConnectionHandler() = default;

    // Initialize sockets, bind and listen on given host:port
    virtual void Initialize(const std::string& host, int port) = 0;

    // Set the receive callback ONCE per socket (can be overwritten if needed)
    virtual void SetEngineCallbacks(ReceiveCallback onData, CompletionCallback onComplete) = 0;

    // Read more data if required (Async)
    virtual void ResumeReceive(ConnectionContext* ctx) = 0;

    // Write data to socket (Async)
    virtual void Write(ConnectionContext* ctx, std::string_view buffer = {}) = 0;

    // Write file directly to sockets (Async)
    virtual void WriteFile(ConnectionContext* ctx, std::string path) = 0;

    // Stream data to socket via a generator function (Async)
    virtual void Stream(ConnectionContext* ctx, StreamGenerator generator, bool streamChunked = true) = 0;

    // Close a client socket
    virtual void Close(ConnectionContext* ctx, bool forceClose = false) = 0;

    // Run the main connection loop (can be used by dev/serve mode)
    virtual void Run() = 0;

    // Refresh the connection's expiry time
    virtual void RefreshExpiry(ConnectionContext* ctx, std::uint16_t timeoutSeconds) = 0;

    // Refresh the connection's async timer
    virtual bool RefreshAsyncTimer(ConnectionContext* ctx, std::uint32_t delayMilliseconds) = 0;

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