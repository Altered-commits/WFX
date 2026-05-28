#ifndef WFX_HTTP_CONNECTION_HANDLER_HPP
#define WFX_HTTP_CONNECTION_HANDLER_HPP

#include "shared/abis/types.hpp"
#include "utils/hash/hash.hpp"
#include "utils/rw_buffer/rw_buffer.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")

using WFXSocket = SOCKET;
constexpr WFXSocket WFX_INVALID_SOCKET = INVALID_SOCKET;
#else
#include <netinet/in.h> // in_addr, in6_addr
#include <arpa/inet.h>  // inet_ntop, inet_pton

using WFXSocket = int; // On Linux/Unix, sockets are file descriptors (ints)
constexpr WFXSocket WFX_INVALID_SOCKET = -1;
#endif

namespace WFX::Http {

// Fwd declare stuff
struct HttpRequest;
struct HttpResponse;

// Cross-Platform compatible Ip Struct
struct WFXIpAddress {
    union {
        in_addr v4;
        in6_addr v6;
        std::uint8_t raw[16]; // For hashing
    } ip;

    std::uint16_t port = 0;
    std::uint8_t type = 0; // AF_INET or AF_INET6

    // Operator functions
    WFXIpAddress& operator=(const WFXIpAddress& other);
    bool operator==(const WFXIpAddress& other) const;

    // Helper functions
    std::string_view GetIpStr() const;
    const char* GetIpType() const;
    bool ToSockAddr(sockaddr_storage& out, socklen_t& len) const;
};
static_assert(sizeof(WFXIpAddress) == 20, "'WFXIpAddress' must be exactly 20 bytes");

// Might be weird to define it here but its important, these states are further used in-
// -both connection backend and parser so yeah
enum class HttpParseState : std::uint8_t {
    PARSE_INCOMPLETE_HEADERS, // Header end sequence (\r\n\r\n) not found yet
    PARSE_INCOMPLETE_BODY,    // Buffering body (Content-Length not fully received)

    PARSE_STREAMING_BODY, // [Future] Streaming mode (body being processed in chunks)

    PARSE_EXPECT_100, // It was a Expect: 100-continue header, accept it
    PARSE_EXPECT_417, // It was a Expect: 100-continue header, REJECT IT
    PARSE_SUCCESS,    // Successfully received and parsed all data
    PARSE_ERROR,      // Malformed request
    PARSE_IDLE        // After Request-Response cycle, waiting for another request
};

enum class EventType : std::uint8_t {
    EVENT_CONNECT,
    EVENT_ACCEPT,
    EVENT_HANDSHAKE, // For SSL
    EVENT_RECV,
    EVENT_SEND,
    EVENT_SEND_FILE,
    EVENT_SHUTDOWN // For SSL
};

enum class ConnectionState : std::uint8_t { CONNECTION_ALIVE, CONNECTION_CLOSE };

enum class EndpointState : std::uint8_t {
    ENDPOINT_NONE,     // Not an endpoint type
    ENDPOINT_INSECURE, // Basic connection (HTTP, etc)
    ENDPOINT_SECURE,   // SSL connection (HTTPS, etc)
};

// Forward declare it so compilers won't cry
struct ConnectionContext;

using ReceiveCallback = std::function<void(ConnectionContext*)>;

struct FileInfo {
#if defined(_WIN32)
    HANDLE handle{0};          // HANDLE is pointer-sized
    std::uint64_t fileSize{0}; // 64-bit for large files
    std::uint64_t offset{0};   // current send offset
#else
    int fd = -1;        // Linux file descriptor
    off_t fileSize = 0; // File size
    off_t offset = 0;   // current send offset
#endif
};

// Used inside of AsyncTrack if needed by 'HandleSuccess'
// Just an optimization so if we do have async code, we don't need to start from top again
enum ExecutionLevel : std::uint8_t { MIDDLEWARE, RESPONSE };

// For async tracking without having to make engine / middleware async themselves
struct AsyncTrack {
    std::uint8_t mAction;
    std::uint8_t levels;
    std::uint16_t mIndex;

    // Get the address of the 8-bit action field directly
    Shared::MiddlewareAction* GetMAction()
    {
        return reinterpret_cast<Shared::MiddlewareAction*>(&mAction);
    }

    // Execution Level: Upper 4 bits
    ExecutionLevel GetELevel() const
    {
        return static_cast<ExecutionLevel>((levels >> 4) & 0x0F);
    }
    void SetELevel(ExecutionLevel v)
    {
        levels = (levels & 0x0F) | ((static_cast<std::uint8_t>(v) & 0x0F) << 4);
    }

    // Middleware Level: Lower 4 bits
    Shared::MiddlewareLevel GetMLevel() const
    {
        return static_cast<Shared::MiddlewareLevel>(levels & 0x0F);
    }
    void SetMLevel(Shared::MiddlewareLevel v)
    {
        levels = (levels & 0xF0) | (static_cast<std::uint8_t>(v) & 0x0F);
    }

    std::uint16_t GetMIndex() const
    {
        return mIndex;
    }
    void SetMIndex(std::uint16_t idx)
    {
        mIndex = idx;
    }
};

// Simply to assert that eventType must exist in anything related to connection-
// -and must be the first member as well (offset == 0)
struct ConnectionTag {
    EventType eventType = EventType::EVENT_ACCEPT; // 1 byte
};

struct ConnectionContext : public ConnectionTag {
    // ------------------------------------------  // 1 byte from ConnectionTag
    bool handshakeDone = false; // 1 byte

    union {
        struct {
            std::uint16_t endpointStatus : 4;        // --
            std::uint16_t parseState : 3;            //  |
            std::uint16_t connectionState : 2;       //  |
            std::uint16_t endpointState : 2;         //  |
            std::uint16_t isStreamOperation : 1;     //  |
            std::uint16_t isFileOperation : 1;       //  |
            std::uint16_t isAsyncTimerOperation : 1; //  |
            std::uint16_t isShuttingDown : 1;        //  |
            std::uint16_t streamChunked : 1;         //  V
        }; // 2 bytes
        std::uint16_t __Flags = 0;
    };

    union {
        AsyncTrack trackAsync;        // |
        std::uint32_t trackBytes = 0; // |-> 4 bytes (Used in HTTP parsing then async tracking if needed)
    };

    std::uint32_t expectedBodyLength = 0; // 4 bytes
    std::uint16_t generationId = 1;       // 2 bytes (0 is specially reserved)
    std::uint16_t endpointIdx = 0;        // 2 bytes

    void* sslConn = nullptr;                      // 8 bytes
    HttpRequest* requestInfo = nullptr;           // 8 bytes
    HttpResponse* responseInfo = nullptr;         // 8 bytes (Async functions require larger scope)
    Utils::RWBuffer rwBuffer;                     // 16 bytes
    Shared::AsyncData asyncData = {};             // 24 bytes
    FileInfo fileInfo = {};                       // 24 bytes
    Shared::StreamGenerator streamGenerator = {}; // 24 bytes

    WFXIpAddress connInfo = {};            // 20 bytes
    WFXSocket socket = WFX_INVALID_SOCKET; // 4 | 8 bytes
                                           // Padded if sizeof(WFXSocket) == 4

    ConnectionContext* clientContext = nullptr;   // 8 bytes (Set only on endpoint contexts)
    ConnectionContext* endpointContext = nullptr; // 8 bytes (Set only on client contexts)

public: // Helper functions
    void ResetContext();
    void ClearContext();
    void CleanupStreamGenerator();

    void SetParseState(HttpParseState newState);
    void SetConnectionState(ConnectionState newState);
    void SetEndpointState(EndpointState newState);
    void SetEndpointStatus(Shared::EndpointStatus newStatus);

    HttpParseState GetParseState() const;
    ConnectionState GetConnectionState() const;
    EndpointState GetEndpointState() const;
    Shared::EndpointStatus GetEndpointStatus() const;

    bool IsEndpoint() const;
    bool IsAsyncOperation() const;
};
static_assert(sizeof(ConnectionContext) <= 196, "ConnectionContext must STRICTLY be less than or equal to 196 bytes.");

struct EndpointContext {
    std::string host;
    sockaddr_storage addr = {0};
    socklen_t addrLen = 0;
};
static_assert(sizeof(EndpointContext) <= 256, "EndpointContext must STRICTLY be less than or equal to 256 bytes.");

// Abstraction for OS impl
struct HttpConnectionHandler {
    virtual ~HttpConnectionHandler() = default;

    // Initialize sockets, bind and listen on given host:port
    virtual void Initialize(const std::string& host, std::uint16_t port) = 0;

    // Set the receive callback ONCE per socket (can be overwritten if needed)
    virtual void SetEngineCallback(ReceiveCallback onData) = 0;

    // Create new endpoint on backend
    virtual std::uint16_t AllocateEndpoint(std::string_view host, std::string_view port, std::uint32_t cLimit,
                                           std::uint32_t ifLimit, bool useTLS) = 0;

    // Read more data if required (Async)
    virtual void ResumeReceive(ConnectionContext* ctx) = 0;

    // Write data to socket (Async)
    virtual void Write(ConnectionContext* ctx, std::string_view buffer = {}) = 0;

    // Write file directly to sockets (Async)
    virtual void WriteFile(ConnectionContext* ctx, std::string path) = 0;

    // Write data directly to an endpoint (Async)
    virtual Shared::EndpointStatus WriteEndpoint(ConnectionContext* ctx, std::uint32_t endpointIndex,
                                                 const std::byte* ptr, std::uint32_t size) = 0;

    // Stream data to socket via a generator function (Async)
    virtual void Stream(ConnectionContext* ctx, Shared::StreamGenerator generator, bool streamChunked = true) = 0;

    // Close a client socket
    virtual void Close(ConnectionContext* ctx, bool forceClose = false) = 0;

    // Refresh the connection's expiry time
    virtual void RefreshExpiry(ConnectionContext* ctx, std::uint16_t timeoutSeconds) = 0;

    // Refresh the connection's async timer
    virtual bool RefreshAsyncTimer(ConnectionContext* ctx, std::uint32_t delayMilliseconds,
                                   Shared::AsyncData asyncData) = 0;

    // Run the main connection loop
    virtual void Run() = 0;

    // Shutdown the main connection loop, cleanup everything
    virtual void Stop() = 0;
};

} // namespace WFX::Http

// Write a std::hash specialization for WFXIpAddress
namespace std {
using WFX::Http::WFXIpAddress;
using WFX::Utils::GetLogger;
using WFX::Utils::GetRandomPool;
using WFX::Utils::Hasher::SipHash24;

template <> struct hash<WFXIpAddress> {
    std::size_t operator()(const WFXIpAddress& addr) const
    {
        static std::uint8_t sipKey[16];

        // Run only once
        static const struct InitKeyOnce {
            InitKeyOnce()
            {
                if(!GetRandomPool().GetBytes(sipKey, sizeof(sipKey)))
                    GetLogger().Fatal("[WFXIpAddress-Hash]: Failed to initialize SipHash key");
            }
        } _initOnce;

        return SipHash24(addr.ip.raw, addr.type == AF_INET ? sizeof(in_addr) : sizeof(in6_addr), sipKey);
    }
};
} // namespace std

#endif // WFX_HTTP_CONNECTION_HANDLER_HPP