// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_HTTP_CONNECTION_HANDLER_HPP
#define WFX_HTTP_CONNECTION_HANDLER_HPP

#include "shared/abis/types.hpp"
#include "utils/hash/hash.hpp"
#include "utils/rw_buffer/rw_buffer.hpp"
#include "utils/resolver/dns_resolver.hpp"

#include <netinet/in.h> // in_addr, in6_addr
#include <arpa/inet.h>  // inet_ntop, inet_pton

using WFXSocket = int; // On Linux/Unix, sockets are file descriptors (ints)
constexpr WFXSocket WFX_INVALID_SOCKET = -1;

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
    // vvv Client slot states vvv
    EVENT_ACCEPT,    // Accepted, pre-SSL-handshake setup
    EVENT_HANDSHAKE, // SSL handshake with client in progress
    EVENT_RECV,      // Reading client request
    EVENT_SEND,      // Writing response to client
    EVENT_SEND_FILE, // sendfile transfer in progress
    EVENT_SHUTDOWN,  // SSL shutdown with client in progress

    // vvv Endpoint slot states vvv
    EVENT_CONNECT,            // TCP connect to backend in progress
    EVENT_ENDPOINT_HANDSHAKE, // SSL handshake with backend in progress
    EVENT_ENDPOINT_ONCONNECT, // onConnect coroutine running, slot not yet pooled
    EVENT_ENDPOINT_SEND,      // Writing serialized request to backend
    EVENT_ENDPOINT_RECV,      // Reading and parsing response from backend
    EVENT_ENDPOINT_SHUTDOWN,  // SSL shutdown with backend in progress
};

enum class ConnectionState : std::uint8_t { CONNECTION_CLOSE, CONNECTION_ALIVE };

enum class EndpointState : std::uint8_t {
    ENDPOINT_NONE,     // Not an endpoint type
    ENDPOINT_INSECURE, // Basic connection (HTTP, etc)
    ENDPOINT_SECURE,   // SSL connection (HTTPS, etc)
};

// Forward declare it so compilers won't cry
struct ClientCtx;

using ReceiveCallback = std::function<void(ClientCtx*)>;

struct FileInfo {
    int fd = -1;        // Linux file descriptor
    off_t fileSize = 0; // File size
    off_t offset = 0;   // current send offset
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

// Per-stream entry for a multiplexed slot (mirrors CoalesceWaiter's shape below, including-
// -the generationId staleness guard). Owned by EndpointCtx::pendingStreams; erased on-
// -completion (parse() reports a non-zero completedKey for it). Only populated when the-
// -endpoint's EndpointDesc::hasCapacity is set; non-multiplexed slots never touch this
// Completion delivery reuses clientCtx->asyncData (set once by SendPayload), same as the-
// -single-slot path and CoalesceWaiter, so no separate AsyncData copy is stored here. No-
// -outputObj field: the protocol owns per-stream output internally and hands it over via-
// -EndpointDesc::takeStreamOutput when the stream completes (or is abandoned early)
struct PendingStream {
    ClientCtx* clientCtx = nullptr;
    void* parseState = nullptr;
    std::uint64_t coalesceKey = 0; // Per-stream, unlike EndpointCtx::coalesceKey which is per-slot
    std::uint16_t generationId = 0;
};
using PendingStreamMap = std::unordered_map<std::uint64_t, PendingStream>;

struct EndpointCtx : public ConnectionTag {
    // ------------------------------------------ 1 byte from ConnectionTag
    union {
        struct {
            std::uint8_t connectionState : 2;
            std::uint8_t endpointState : 2;
            std::uint8_t isShuttingDown : 1;
            std::uint8_t inOnConnectPhase : 1;
            std::uint8_t isPooledIdle : 1;
            std::uint8_t isAwaitingReconnect : 1;
        };
        std::uint8_t flags = 0;
    }; // 1 bytes

    std::uint16_t generationId = 1;      // 2 bytes
    std::uint16_t endpointIdx = 0;       // 2 bytes
    std::uint16_t reconnectAttempts = 0; // 2 bytes, backoff attempt count, fits in existing padding

    ClientCtx* clientCtx = nullptr; // 8 bytes
    void* sslConn = nullptr;        // 8 bytes
    void* slotState = nullptr;      // 8 bytes, persists across requests
    void* parseStateObj = nullptr;  // 8 bytes, reset between requests
    void* outputObj = nullptr;      // 8 bytes, per-request, nulled before callback
    std::uint64_t coalesceKey = 0;  // 8 bytes, per-request, set by SendPayload

    PendingStreamMap* pendingStreams = nullptr; // 8 bytes, lazily allocated, only used when hasCapacity is set
    const void* pendingConnectReq = nullptr;    // 8 bytes, request waiting on a fresh onConnect handshake

    Utils::RWBuffer rwBuffer;         // 16 bytes
    Shared::AsyncData asyncData = {}; // 24 bytes

    WFXSocket socket = WFX_INVALID_SOCKET; // 4 or 8 bytes depending on OS

public:
    void Reset();

    void SetConnectionState(ConnectionState newState);
    void SetEndpointState(EndpointState newState);

    ConnectionState GetConnectionState() const;
    EndpointState GetEndpointState() const;

    bool IsAsyncOperation() const;
};
static_assert(sizeof(EndpointCtx) <= 128, "'EndpointCtx' must be <= 128 bytes");

struct ClientCtx : public ConnectionTag {
    // ------------------------------------------ 1 byte from ConnectionTag
    bool handshakeDone = false; // 1 byte

    union {
        struct {
            std::uint16_t parseState : 3;
            std::uint16_t connectionState : 2;
            std::uint16_t isStreamOperation : 1;
            std::uint16_t isFileOperation : 1;
            std::uint16_t isAsyncTimerOperation : 1;
            std::uint16_t isShuttingDown : 1;
            std::uint16_t streamChunked : 1;
            std::uint16_t reserved : 6;
        };
        std::uint16_t flags = 0;
    }; // 2 bytes

    union {
        AsyncTrack trackAsync;
        std::uint32_t trackBytes = 0;
    }; // 4 bytes

    std::uint32_t expectedBodyLength = 0; // 4 bytes
    std::uint16_t generationId = 1;       // 2 bytes
    std::uint16_t padding = 0;            // 2 bytes

    EndpointCtx* endpointCtx = nullptr; // 8 bytes
    std::uint64_t streamKey = 0;        // 8 bytes, key into endpointCtx->pendingStreams; 0 = not multiplexed
    void* sslConn = nullptr;            // 8 bytes

    HttpRequest* requestInfo = nullptr;   // 8 bytes
    HttpResponse* responseInfo = nullptr; // 8 bytes

    Utils::RWBuffer rwBuffer;                     // 16 bytes
    Shared::AsyncData asyncData = {};             // 24 bytes
    FileInfo fileInfo = {};                       // 24 bytes
    Shared::StreamGenerator streamGenerator = {}; // 24 bytes

    WFXIpAddress connInfo = {};            // 20 bytes
    WFXSocket socket = WFX_INVALID_SOCKET; // 4 | 8 bytes depending on OS

public:
    void Reset();
    void Clear();
    void CleanupStreamGenerator();

    void SetParseState(HttpParseState newState);
    void SetConnectionState(ConnectionState newState);

    HttpParseState GetParseState() const;
    ConnectionState GetConnectionState() const;

    bool IsAsyncOperation() const;
};
static_assert(sizeof(ClientCtx) <= 168, "'ClientCtx' must be <= 168 bytes");

// Per-waiter entry for a coalesced in-flight request
// Both pointers are non-owning (lifetime is tied to the owning CoalesceEntry)
struct CoalesceWaiter {
    ClientCtx* clientCtx;
    std::uint16_t generationId; // saved at registration; stale if clientCtx was freed and bumped
};

// One entry per in-flight coalesce key
// 'inflight' is the slot doing the actual backend request
// 'waiters' are clients parked until that slot's response arrives
struct CoalesceEntry {
    EndpointCtx* inflight = nullptr;
    std::vector<CoalesceWaiter> waiters;
};

struct EndpointMetadata {
    std::string hostname;
    Utils::ResolvedAddrs addrs;
    std::uint64_t dnsNextRefreshSeconds = 0;
    std::uint16_t nextAddrIdx = 0;
    std::uint16_t port = 0;
    std::uint32_t timerBase = 0;            // Specific to timer wheel
    std::uint32_t lastMultiplexHintIdx = 0; // Last slot idx that had multiplex capacity, tried first next time
    Shared::EndpointDesc desc = {0};
    Shared::EndpointConfig config = {0};
    std::unordered_map<std::uint64_t, CoalesceEntry> coalescePending; // TODO: Gotta switch to our hashmap
};
static_assert(sizeof(EndpointMetadata) <= 512, "'EndpointMetadata' must be <= 512 bytes");

// Abstraction for OS impl
struct HttpConnectionHandler {
    virtual ~HttpConnectionHandler() = default;

    virtual void Initialize(const std::string& host, std::uint16_t port) = 0;
    virtual void SetEngineCallback(ReceiveCallback onData) = 0;
    virtual std::uint16_t AllocateEndpoint(const char* host, Shared::EndpointDesc desc,
                                           Shared::EndpointConfig config) = 0;

    // vvv Client operations vvv
    virtual void ResumeReceive(ClientCtx* ctx) = 0;
    virtual void Write(ClientCtx* ctx, std::string_view buffer = {}) = 0;
    virtual void WriteFile(ClientCtx* ctx, std::string path) = 0;
    virtual void Stream(ClientCtx* ctx, Shared::StreamGenerator generator, bool streamChunked = true) = 0;
    virtual void Close(ClientCtx* ctx, bool forceClose = false) = 0;
    virtual void RefreshExpiry(ClientCtx* ctx, std::uint16_t timeoutSeconds) = 0;
    virtual bool RefreshAsyncTimer(ClientCtx* ctx, std::uint32_t delayMs, Shared::AsyncData asyncData) = 0;

    // vvv Endpoint operations vvv
    virtual Shared::EndpointStatus SendPayload(ClientCtx* clientCtx, std::uint16_t endpointIdx, const void* req,
                                               Shared::AsyncData asyncData) = 0;
    virtual void SlotSend(EndpointCtx* slotCtx, const void* data, std::uint32_t size, Shared::AsyncData asyncData) = 0;
    virtual void SlotReceive(EndpointCtx* slotCtx, Shared::AsyncData asyncData) = 0;
    // Empty if the slot isn't TLS or the handshake hasn't completed yet. Not HTTP-specific: any-
    // -ALPN-aware protocol can call this from onConnect to decide how to speak on this connection
    virtual Shared::StringView NegotiatedProtocol(EndpointCtx* slotCtx) = 0;
    virtual void Close(EndpointCtx* ctx, bool forceClose = false,
                       Shared::DisconnectReason reason = Shared::DisconnectReason::ERROR) = 0;
    virtual void RefreshExpiry(EndpointCtx* ctx, std::uint16_t timeoutSeconds) = 0;

    // vvv Engine control vvv
    virtual void Run() = 0;
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