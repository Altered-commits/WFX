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
        MiddlewareAction action;     // CONTINUE for non-middleware
        ConnectResult connectResult; // Set by Promise<ConnectResult> on final_suspend
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
    SOCKET_FAILURE,  // Not created, options not set, etc
    CONNECT_FAILURE, // Failed to connect to endpoint
    SSL_FAILURE,     // Client not created, Handshake failed, etc

    // Generic errors
    INTERNAL_ERROR,    // Something went wrong
    SERIALIZE_ERROR,   // desc.serialize returned SerializeResult::ERROR
    EPOLL_ERROR,       // RegisterEpoll(MOD) failed on reused slot
    HANDSHAKE_TIMEOUT, // onConnect coroutine exceeded connectTimeoutMs
    DNS_FAILURE,       // All DNS resolution retries exhausted
};

enum class EndpointTLSConfig : std::uint8_t {
    AUTO = 0,              // TLS automatically on some preconfigured ports
    FORCE_REQUIRE,         // Force TLS (port doesn't matter)
    FORCE_INSECURE,        // Explicitly allow no TLS even on secure ports
    NONE = FORCE_INSECURE, // Alias: explicit opt-out of TLS, same as FORCE_INSECURE
};

enum class ConnectResult : std::uint8_t {
    READY, // Handshake complete, slot may enter pool
    RETRY, // Transient failure, engine will reconnect with backoff
    FATAL  // Permanent failure, slot is discarded
};

enum class DisconnectReason : std::uint8_t {
    POOL_DRAIN, // Slot drained intentionally (e.g. DNS refresh, shutdown)
    TIMEOUT,    // Idle or handshake timeout elapsed
    ERROR       // I/O or protocol error
};

enum class SlotSendResult : std::uint8_t {
    PENDING, // Write queued, awaitable will resume on flush
    OK,      // Written and flushed immediately
    ERROR    // Fatal write failure
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

using EndpointSlotSendFn = SlotSendResult (*)(void* endpointCtx, const void* data, std::uint32_t size,
                                              AsyncData onComplete);
using EndpointSlotReceiveFn = void (*)(void* endpointCtx, AsyncData onComplete);
using EndpointSlotCloseFn = void (*)(void* endpointCtx);

struct EndpointSlotHandle {
    void* impl;
    EndpointSlotSendFn Send;
    EndpointSlotReceiveFn Receive;
    EndpointSlotCloseFn Close;
};
static_assert(sizeof(EndpointSlotHandle) == 32, "'EndpointSlotHandle' must be exactly 32 bytes.");
static_assert(std::is_standard_layout_v<EndpointSlotHandle>, "'EndpointSlotHandle' must be standard layout");

// 'ctx' is userCtx for slot, 'slotState' for parse/output
using EndpointSerializeFn = SerializeResult (*)(void* slotState, const void* req, char* buf, std::uint32_t bufLen,
                                                std::uint32_t* written);
using EndpointParseFn = ParseResult (*)(void* slotState, void* parseState, const char* buf, std::uint32_t len,
                                        std::uint32_t* consumed, void* outObj);
using EndpointOnConnectFn = void (*)(EndpointSlotHandle handle, void* slotState, AsyncCompleteFn onDone,
                                     void* onDoneUd);
using EndpointOnDisconnectFn = void (*)(void* slotState, DisconnectReason reason);
using EndpointCreateStateFn = void* (*)(void* ctx);
using EndpointDestroyStateFn = void (*)(void* state);
using EndpointResetStateFn = void (*)(void* parseState);
using EndpointCoalesceKeyFn = std::uint64_t (*)(const void* req);

struct EndpointDesc {
    EndpointSerializeFn serialize;
    EndpointParseFn parse;
    EndpointOnConnectFn onConnect;            // nullable, skipped for simple protocols (Redis, etc)
    EndpointOnDisconnectFn onDisconnect;      // nullable
    EndpointCreateStateFn createSlotState;    // nullable
    EndpointDestroyStateFn destroySlotState;  // nullable
    EndpointCreateStateFn createParseState;   // nullable, takes slotState as ctx
    EndpointDestroyStateFn destroyParseState; // nullable
    EndpointResetStateFn resetParseState;     // nullable, called between requests if non-null
    EndpointCreateStateFn createOutput;       // nullable, takes slotState as ctx
    EndpointDestroyStateFn destroyOutput;     // nullable
    EndpointCoalesceKeyFn coalesceKey;        // nullable, no coalescing if null
    void* userCtx;                            // injected into createSlotState
};
static_assert(sizeof(EndpointDesc) == 104, "'EndpointDesc' must be exactly 104 bytes.");
static_assert(std::is_standard_layout_v<EndpointDesc>, "'EndpointDesc' must be standard layout");

struct EndpointConfig {
    std::uint32_t connLimit;            // Max simultaneous connections in the slot pool
    std::uint32_t dnsRefreshSeconds;    // 0 = respect actual DNS TTL, N = override with N seconds
    std::uint32_t connectTimeoutMs;     // TCP+TLS+onConnect must complete within this window
    std::uint32_t idleTimeoutSeconds;   // Idle slots are closed after this many seconds
    std::uint32_t maxReconnectAttempts; // Max backoff attempts before slot is marked FATAL
    std::uint32_t reconnectBackoffBase; // Initial backoff seconds
    std::uint32_t reconnectBackoffMax;  // Backoff cap seconds
    std::uint32_t prewarm;              // Slots to connect eagerly on first epoll loop iteration
    EndpointTLSConfig tlsConfig;        // TLS mode for this endpoint
};
static_assert(sizeof(EndpointConfig) == 36, "'EndpointConfig' must be exactly 36 bytes.");
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
    std::uint64_t activeConns = 0;
    std::uint64_t requests = 0;
    std::uint64_t response1xx = 0;
    std::uint64_t response2xx = 0;
    std::uint64_t response3xx = 0;
    std::uint64_t response4xx = 0;
    std::uint64_t response5xx = 0;
};
static_assert(sizeof(NetworkMetrics) == 120, "'NetworkMetrics' must be exactly 120 bytes");
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