// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_ABI_TYPES_HPP
#define WFX_SHARED_ABI_TYPES_HPP

#include "uuid.hpp"

// Fwd declare user side request and response
namespace WFX::Http {

struct Request;
struct Response;

} // namespace WFX::Http

namespace WFX::Shared {

// Forward declare stuff
enum class ConnectResult : std::uint8_t;
enum class EndpointStatus : std::uint8_t;

// vvv Middleware Enums vvv
enum class MiddlewareLevel : std::uint8_t { GLOBAL = 0, PER_ROUTE };

enum class MiddlewareAction : std::uint8_t {
    CONTINUE = 0, // Continue to next middleware
    BREAK,        // Break out of middleware chain
    SKIP_NEXT     // Skip the next middleware in chain if any
};

// vvv Async vvv
enum class AsyncStatus : std::uint8_t {
    NONE = 0,
    COMPLETED, // Mostly for internal use
    TIMER_FAILURE,
    IO_FAILURE,
    INTERNAL_FAILURE
};

struct AsyncResult {
    void* data;
    std::uint32_t dataLen;
    union {
        MiddlewareAction action;       // CONTINUE for non-middleware
        ConnectResult connectResult;   // Set by Promise<ConnectResult> on final_suspend
        EndpointStatus endpointStatus; // Set by ReleaseEndpoint and HandleEndpointReceive on client failure
        std::uint8_t unused;           // Filler when none of the above apply (e.g. endpoint success)
    };
    AsyncStatus status;
};
static_assert(sizeof(AsyncResult) == 16, "'AsyncResult' must be exactly 16 bytes.");
static_assert(std::is_standard_layout_v<AsyncResult>, "'AsyncResult' must be standard layout");

using AsyncCompleteFn = void (*)(void* userData, AsyncResult result);
using AsyncDestroyFn = void (*)(void* userData);

struct AsyncData {
    void* userData;                // |
    AsyncCompleteFn AsyncComplete; // | -> For cleaner storage on engine side i suppose
    AsyncDestroyFn AsyncDestroy;   // |
};
static_assert(sizeof(AsyncData) == 24, "'AsyncData' must be exactly 24 bytes.");
static_assert(std::is_standard_layout_v<AsyncData>, "'AsyncData' must be standard layout");

// vvv Route Callbacks vvv
using SyncRouteFn = void (*)(Http::Request, Http::Response);
using AsyncRouteFn = void (*)(Http::Request, Http::Response, AsyncCompleteFn onDone, void* onDoneUd);
using SyncMwFn = MiddlewareAction (*)(Http::Request, Http::Response);
using AsyncMwFn = void (*)(Http::Request, Http::Response, AsyncCompleteFn onDone, void* onDoneUd);

enum class CallbackKind : std::uint8_t { SYNC = 0, ASYNC };

struct RouteCallback {
    CallbackKind kind;
    union {
        SyncRouteFn sync;
        AsyncRouteFn async;
    };

    bool IsEmpty() const noexcept
    {
        return kind == CallbackKind::SYNC ? sync == nullptr : async == nullptr;
    }
};
static_assert(sizeof(RouteCallback) == 16, "'RouteCallback' must be exactly 16 bytes.");
static_assert(std::is_standard_layout_v<RouteCallback>, "'RouteCallback' must be standard layout");

struct MwCallback {
    CallbackKind kind;
    union {
        SyncMwFn sync;
        AsyncMwFn async;
    };

    bool IsEmpty() const noexcept
    {
        return kind == CallbackKind::SYNC ? sync == nullptr : async == nullptr;
    }
};
static_assert(sizeof(MwCallback) == 16, "'MwCallback' must be exactly 16 bytes.");
static_assert(std::is_standard_layout_v<MwCallback>, "'MwCallback' must be standard layout");

// vvv Outbound Streaming vvv
enum class StreamAction : std::uint8_t { CONTINUE = 0, STOP_AND_ALIVE_CONN, STOP_AND_CLOSE_CONN };

struct StreamResult {
    std::size_t writtenBytes;
    StreamAction action;
};
static_assert(sizeof(StreamResult) == 16, "'StreamResult' must be exactly 16 bytes.");
static_assert(std::is_standard_layout_v<StreamResult>, "'StreamResult' must be standard layout");

struct StreamBuffer {
    char* buffer;
    std::size_t size;
};
static_assert(sizeof(StreamBuffer) == 16, "'StreamBuffer' must be exactly 16 bytes.");
static_assert(std::is_standard_layout_v<StreamBuffer>, "'StreamBuffer' must be standard layout");

struct StreamGenerator {
    void* ctx;
    StreamResult (*Next)(void* ctx, StreamBuffer buffer);
    void (*Destroy)(void* ctx);
};
static_assert(sizeof(StreamGenerator) == 24, "'StreamGenerator' must be exactly 24 bytes.");
static_assert(std::is_standard_layout_v<StreamGenerator>, "'StreamGenerator' must be standard layout");

// vvv Endpoint vvv
enum class EndpointStatus : std::uint8_t {
    // Success
    SUCCESS = 0, // Endpoint processing finished
    PENDING,     // Endpoint processing in progress

    // Buffer errors
    BUFFER_ERROR,        // Initialization failed, etc
    INSUFFICIENT_BUFFER, // Write / Read buffer insufficient

    // Pool errors
    INVALID_KEY,    // Index out of bounds in the endpoint pool
    POOL_EXHAUSTED, // All endpoints in use

    // Socket errors
    SOCKET_FAILURE,  // Local socket creation or configuration failed (before ever reaching connect())
    CONNECT_FAILURE, // connect() itself failed
    SSL_FAILURE,     // Client not created, Handshake failed, etc

    // Generic errors
    INTERNAL_ERROR,    // Something went wrong
    SERIALIZE_ERROR,   // desc.serialize returned SerializeResult::ERROR
    EPOLL_ERROR,       // RegisterEpoll(MOD) failed on reused slot
    HANDSHAKE_TIMEOUT, // TCP connect / TLS handshake / onConnect coroutine exceeded connectTimeoutSeconds
    REQUEST_TIMEOUT,   // Did not get response within requestTimeoutSeconds
};

enum class EndpointTLSConfig : std::uint8_t {
    AUTO = 0,       // TLS automatically on some preconfigured ports
    FORCE_REQUIRE,  // Force TLS (port doesn't matter)
    FORCE_INSECURE, // Explicitly allow no TLS even on secure ports
};

enum class ConnectResult : std::uint8_t {
    READY, // Handshake complete, slot may enter pool
    RETRY, // Transient failure, engine will reconnect with backoff
    FATAL  // Permanent failure, slot is discarded
};

enum class DisconnectReason : std::uint8_t {
    TIMEOUT,           // Idle or in-flight request timeout elapsed
    HANDSHAKE_TIMEOUT, // Timeout elapsed while still connecting (TCP connect / TLS handshake / onConnect)
    ERROR              // I/O or protocol error, or any other close not covered above
};

enum class SlotSendResult : std::uint8_t {
    OK,   // Written and flushed immediately
    ERROR // Fatal write failure
};

enum class SlotReceiveStatus : std::uint8_t {
    OK,   // Data arrived, buffer pointer and length are valid
    ERROR // Fatal read failure
};

enum class SerializeResult : std::uint8_t {
    OK,               // Serialized successfully
    BUFFER_TOO_SMALL, // Output buffer insufficient, engine may retry with larger buffer
    ERROR             // Unrecoverable serialization failure
};

enum class ParseResult : std::uint8_t {
    INCOMPLETE,          // Need more bytes, call again when data arrives
    COMPLETE_KEEP_ALIVE, // Full message received, slot returns to pool
    COMPLETE_CLOSE,      // Full message received, slot must close after delivery
    ERROR                // Unrecoverable parse failure
};

// Reserved slot-close hook, currently unused (nullptr) by the engine
// Present for ABI symmetry with the rest of EndpointSlotHandle
using EndpointSlotCloseFn = void (*)(void* endpointCtx);
// Returns the ALPN protocol negotiated on this slot's TLS connection
// Empty if not TLS or the handshake hasn't completed yet
using EndpointNegotiatedProtocolFn = StringView (*)(void* endpointCtx);

// Send/Receive go through the endpoint API (SlotSend/SlotReceive) directly,-
// -SlotHandle::Send/Receive call those rather than a per-slot function pointer here
struct EndpointSlotHandle {
    void* impl;
    EndpointSlotCloseFn Close;
    EndpointNegotiatedProtocolFn NegotiatedProtocol;
};
static_assert(sizeof(EndpointSlotHandle) == 24, "'EndpointSlotHandle' must be exactly 24 bytes.");
static_assert(std::is_standard_layout_v<EndpointSlotHandle>, "'EndpointSlotHandle' must be standard layout");

// Writes req's wire encoding into buf; streamKey is only used when hasCapacity is set-
// -(the key this request should be tracked under, e.g. an HTTP/2 stream id), else left at 0
using EndpointSerializeFn = SerializeResult (*)(void* slotState, const void* req, char* buf, std::uint32_t bufLen,
                                                std::uint32_t* written, std::uint64_t* streamKey);
// Reads arriving bytes for one slot; isEof forbids returning INCOMPLETE (no more bytes coming)
// completedKey is 0 unless hasCapacity is set, in which case non-zero means that stream is done
using EndpointParseFn = ParseResult (*)(void* slotState, void* parseState, const char* buf, std::uint32_t len,
                                        std::uint32_t* consumed, void* outObj, bool isEof, std::uint64_t* completedKey);
// Runs once per connection before it enters the pool (auth handshakes, etc.)
// Must eventually call onDone with a ConnectResult via onDoneUd
using EndpointOnConnectFn = void (*)(EndpointSlotHandle handle, void* slotState, AsyncCompleteFn onDone,
                                     void* onDoneUd);
// Fires when a slot is torn down, before slotState is destroyed
// reason distinguishes drain / idle-timeout / error
using EndpointOnDisconnectFn = void (*)(void* slotState, DisconnectReason reason);
// Allocates per-slot or per-request state (ctx is userCtx for slots, slotState for requests)
// Paired with the matching EndpointDestroyStateFn
using EndpointCreateStateFn = void* (*)(void* ctx);
// Frees state allocated by the matching EndpointCreateStateFn
// Called on request completion or slot teardown
using EndpointDestroyStateFn = void (*)(void* state);
// Clears parse state for reuse between keep-alive requests on the same slot
// Only called when non-null; otherwise the engine destroys and recreates it
using EndpointResetStateFn = void (*)(void* parseState);
// Computes a dedup key for req; identical keys share one in-flight backend request
// Return 0 to opt this request out of coalescing
using EndpointCoalesceKeyFn = std::uint64_t (*)(const void* req);
// Returns a freshly allocated deep clone of srcOutput, or null on allocation failure
// Required when coalesceKey is set (each coalesced waiter receives its own owned clone)
using EndpointCloneOutputFn = void* (*)(void* slotState, const void* srcOutput);
// Null = slot is exclusively single-request (today's behavior)
// Non-null = engine may hand this busy slot another request when this returns true
using EndpointHasCapacityFn = bool (*)(void* slotState);
// Claims a multiplexed stream's finished output (completed or being abandoned early)
// Returns null if unfinished; protocol must free any partial internal state for that key
using EndpointTakeStreamOutputFn = void* (*)(void* slotState, std::uint64_t key);

struct EndpointDesc {
    EndpointSerializeFn serialize;
    EndpointParseFn parse;
    EndpointOnConnectFn onConnect;               // nullable, skipped for simple protocols (Redis, etc)
    EndpointOnDisconnectFn onDisconnect;         // nullable
    EndpointCreateStateFn createSlotState;       // nullable
    EndpointDestroyStateFn destroySlotState;     // nullable
    EndpointCreateStateFn createParseState;      // nullable, takes slotState as ctx
    EndpointDestroyStateFn destroyParseState;    // nullable
    EndpointResetStateFn resetParseState;        // nullable, called between requests if non-null
    EndpointCreateStateFn createOutput;          // nullable, takes slotState as ctx
    EndpointDestroyStateFn destroyOutput;        // nullable
    EndpointCoalesceKeyFn coalesceKey;           // nullable, no coalescing if null
    EndpointCloneOutputFn cloneOutput;           // nullable, REQUIRED when coalesceKey is set
    EndpointHasCapacityFn hasCapacity;           // nullable, non-null enables multiplexed slot sharing
    EndpointTakeStreamOutputFn takeStreamOutput; // nullable, REQUIRED when hasCapacity is set
    void* userCtx;                               // injected into createSlotState
};
static_assert(sizeof(EndpointDesc) == 128, "'EndpointDesc' must be exactly 128 bytes.");
static_assert(std::is_standard_layout_v<EndpointDesc>, "'EndpointDesc' must be standard layout");

struct EndpointConfig {
    std::uint32_t connLimit;             // Max simultaneous connections in the slot pool
    std::uint32_t dnsRefreshSeconds;     // 0 = respect actual DNS TTL, N = override with N seconds
    std::uint16_t connectTimeoutSeconds; // TCP+TLS+onConnect must complete within this window
    std::uint16_t requestTimeoutSeconds; // Send+receive cycle must complete within this window
    std::uint32_t idleTimeoutSeconds;    // Idle slots are closed after this many seconds
    std::uint16_t maxReconnectAttempts;  // Max backoff attempts before slot is marked FATAL
    std::uint16_t reconnectBackoffBase;  // Initial backoff seconds
    std::uint16_t reconnectBackoffMax;   // Backoff cap seconds
    EndpointTLSConfig tlsConfig;         // TLS mode for this endpoint
    std::uint32_t prewarm;               // Slots to connect eagerly on first epoll loop iteration
    std::uint32_t maxConcurrentStreams;  // Cap on requests sharing one slot; 0/1 = exclusive slot
    StringView alpnProtocols;            // Wire-encoded ALPN list; empty = offer http/1.1 only
};
static_assert(sizeof(EndpointConfig) == 48, "'EndpointConfig' must be exactly 48 bytes.");
static_assert(std::is_standard_layout_v<EndpointConfig>, "'EndpointConfig' must be standard layout");

// vvv Server Metrics vvv
// IMPORTANT: DO NOT CHANGE THE ORDER OF THESE METRICS, 'logger.hpp' depends on the order
struct LogMetrics {
    std::uint64_t trace = 0;
    std::uint64_t debug = 0;
    std::uint64_t info = 0;
    std::uint64_t warn = 0;
    std::uint64_t error = 0;
    std::uint64_t fatal = 0;
};
static_assert(sizeof(LogMetrics) == 48, "'LogMetrics' must be exactly 48 bytes");
static_assert(std::is_standard_layout_v<LogMetrics>, "'LogMetrics' must be standard layout");

// Updated per request-response cycle
struct NetworkMetrics {
    std::uint64_t accepts = 0;
    std::uint64_t reads = 0;
    std::uint64_t bytesRead = 0;
    std::uint64_t writes = 0;
    std::uint64_t bytesWritten = 0;
    std::uint64_t fileCalls = 0;
    std::uint64_t fileFallbacks = 0;
    std::uint64_t fileBytesWritten = 0;
    std::uint64_t activeClientConns = 0;
    std::uint64_t activeEndpointConns = 0;
    std::uint64_t requests = 0;
    std::uint64_t response1xx = 0;
    std::uint64_t response2xx = 0;
    std::uint64_t response3xx = 0;
    std::uint64_t response4xx = 0;
    std::uint64_t response5xx = 0;
};
static_assert(sizeof(NetworkMetrics) == 128, "'NetworkMetrics' must be exactly 128 bytes");
static_assert(std::is_standard_layout_v<NetworkMetrics>, "'NetworkMetrics' must be standard layout");

// Written by master process, reflects live state of each worker slot
struct SelfMetrics {
    std::uint64_t rssBytes = 0;        // Resident set size in bytes
    std::uint64_t vmBytes = 0;         // Virtual memory size in bytes
    std::uint32_t restarts = 0;        // How many times this slot has been restarted
    std::uint32_t crashes = 0;         // How many times this slot died unexpectedly (signal or non-zero exit)
    std::uint32_t backoffAttempts = 0; // Current backoff attempt count (resets on successful start)
    std::int32_t pid = -1;             // OS agnostic pid
    std::int64_t startedAt = 0;        // Unix timestamp of last worker start
    std::int64_t nextRetryAt = 0;      // Unix timestamp of next allowed restart attempt
};
static_assert(sizeof(SelfMetrics) == 48, "'SelfMetrics' must be exactly 48 bytes");
static_assert(std::is_standard_layout_v<SelfMetrics>, "'SelfMetrics' must be standard layout");

// One slot per worker in shared mmap
// Embeds user-facing metric structs directly (add fields there, not here)
// alignas(64) prevents false sharing between adjacent worker slots
struct alignas(64) WorkerMetrics {
    LogMetrics log = {};
    NetworkMetrics network = {};
    SelfMetrics self = {};
};
static_assert(sizeof(WorkerMetrics) % 64 == 0, "'WorkerMetrics' must be a multiple of 64 bytes");
static_assert(std::is_standard_layout_v<WorkerMetrics>, "'WorkerMetrics' must be standard layout");

} // namespace WFX::Shared

#endif // WFX_SHARED_ABI_TYPES_HPP