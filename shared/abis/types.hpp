// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_ABI_TYPES_HPP
#define WFX_SHARED_ABI_TYPES_HPP

#include "constants.hpp"
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

// Shared result for every slot-level operation available inside an onConnect coroutine
// (Send, Receive, UpgradeToTLS). One enum rather than one per operation: the failure modes
// are the same underlying set, and each operation can only produce a subset anyway.
enum class SlotStatus : std::uint8_t {
    OK,           // Operation completed
    BUFFER_ERROR, // Slot's read/write buffer couldn't be allocated or grown
    EPOLL_ERROR,  // Re-arming the slot with epoll failed
    IO_ERROR,     // Socket read/write failed
    TLS_ERROR,    // TLS wrap or handshake failed
    INVALID_STATE // Not valid for this slot right now (e.g. upgrading an already-secure one)
};

struct AsyncResult {
    void* data;
    std::uint32_t dataLen;
    union {
        MiddlewareAction action;       // CONTINUE for non-middleware
        ConnectResult connectResult;   // Set by Promise<ConnectResult> on final_suspend
        EndpointStatus endpointStatus; // Set by ReleaseEndpoint and HandleEndpointReceive on client failure
        SlotStatus slotStatus;         // Set by SlotSend/SlotReceive/SlotUpgradeTls on failure
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
    AsyncCompleteFn asyncComplete; // | -> For cleaner storage on engine side i suppose
    AsyncDestroyFn asyncDestroy;   // |
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
    StreamResult (*next)(void* ctx, StreamBuffer buffer);
    void (*destroy)(void* ctx);
};
static_assert(sizeof(StreamGenerator) == 24, "'StreamGenerator' must be exactly 24 bytes.");
static_assert(std::is_standard_layout_v<StreamGenerator>, "'StreamGenerator' must be standard layout");

// vvv Endpoint vvv
enum class EndpointStatus : std::uint8_t {
    // Success
    SUCCESS = 0,     // Endpoint processing finished
    PENDING,         // Endpoint processing in progress
    CHUNK_AVAILABLE, // Stream Next() satisfied from buffered bytes, chunk ready without suspending

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

enum class SerializeResult : std::uint8_t {
    OK,               // Serialized successfully
    BUFFER_TOO_SMALL, // Output buffer insufficient, engine may retry with larger buffer
    ERROR             // Unrecoverable serialization failure
};

// CHUNK_* deliver one piece of a response and keep the request in flight. outObj is borrowed
// until the next Next(), so the protocol must reset it on the following parse call, not append:
// reusing one object is what bounds memory to a single chunk regardless of total size.
enum class ParseResult : std::uint8_t {
    INCOMPLETE,          // Need more bytes, call again when data arrives
    CHUNK_READY,         // Chunk ready; more bytes arrive unprompted (MySQL rows, HTTP chunked, etc)
    CHUNK_READY_FETCH,   // Chunk ready; engine re-serializes to ask for the next batch (Postgres, Cassandra, etc)
    COMPLETE_KEEP_ALIVE, // Full message received, slot returns to pool
    COMPLETE_CLOSE,      // Full message received, slot must close after delivery
    ERROR                // Unrecoverable parse failure
};

// Closes a side connection opened via the endpoint API's openSideConnection. nullptr on a
// primary handle (onConnect's/onAbort's own slot); only a side-connection handle gets a real one.
using EndpointSlotCloseFn = void (*)(void* endpointCtx);

// Returns the ALPN protocol negotiated on this slot's TLS connection
// Empty if not TLS or the handshake hasn't completed yet
using EndpointNegotiatedProtocolFn = StringView (*)(void* endpointCtx);

// Send/Receive go through the endpoint API (SlotSend/SlotReceive) directly; SlotHandle::Send/Receive
// call those rather than a per-slot function pointer here.
struct EndpointSlotHandle {
    void* impl;
    EndpointSlotCloseFn close;
    EndpointNegotiatedProtocolFn negotiatedProtocol;
};
static_assert(sizeof(EndpointSlotHandle) == 24, "'EndpointSlotHandle' must be exactly 24 bytes.");
static_assert(std::is_standard_layout_v<EndpointSlotHandle>, "'EndpointSlotHandle' must be standard layout");

// Wire codec, the two every protocol implements. serialize's streamKey is only set when
// hasCapacity is (e.g. an HTTP/2 stream id), else left 0; parse's completedKey mirrors it.
// isEof forbids parse from returning INCOMPLETE, no more bytes are coming
using EndpointSerializeFn = SerializeResult (*)(void* slotState, const void* req, char* buf, std::uint32_t bufLen,
                                                std::uint32_t* written, std::uint64_t* streamKey);
using EndpointParseFn = ParseResult (*)(void* slotState, void* parseState, const char* buf, std::uint32_t len,
                                        std::uint32_t* consumed, void* outObj, bool isEof, std::uint64_t* completedKey);

// Connection lifecycle. onConnect runs before the slot enters the pool (auth handshakes) and
// must eventually call onDone with a ConnectResult; onDisconnect runs on teardown, before
// slotState is destroyed.
using EndpointOnConnectFn = void (*)(EndpointSlotHandle handle, void* slotState, AsyncCompleteFn onDone,
                                     void* onDoneUd);
using EndpointOnDisconnectFn = void (*)(void* slotState, DisconnectReason reason);

// Fires when a single-slot request's client disconnects, instead of force-closing the slot. The
// slot itself is left alone (still mid-request, real response completes normally). handle is
// read-only + OpenSideConnection only (see AbortSlotHandle); Send/Receive on it directly is illegal
// here. Never fires for multiplexed slots or a request already mid-Stream().
using EndpointOnAbortFn = void (*)(EndpointSlotHandle handle, void* slotState, AsyncCompleteFn onDone, void* onDoneUd);

// State allocation for slot state, parse state and output. create's ctx is userCtx for slot
// state, slotState for per-request state. reset clears parse state between keep-alive requests;
// without it the engine destroys and recreates instead.
using EndpointCreateStateFn = void* (*)(void* ctx);
using EndpointDestroyStateFn = void (*)(void* state);
using EndpointResetStateFn = void (*)(void* parseState);

// Coalescing, where identical in-flight requests share one backend round trip. coalesceKey
// returns 0 to opt a request out; cloneOutput gives each waiter its own owned copy and is
// REQUIRED whenever coalesceKey is set.
using EndpointCoalesceKeyFn = std::uint64_t (*)(const void* req);
using EndpointCloneOutputFn = void* (*)(void* slotState, const void* srcOutput);

// Multiplexing, several concurrent requests over one connection. Non-null hasCapacity enables
// it, and the engine hands a busy slot more work whenever it returns true. takeStreamOutput
// claims a stream's output (null if unfinished) and is REQUIRED alongside it.
using EndpointHasCapacityFn = bool (*)(void* slotState);
using EndpointTakeStreamOutputFn = void* (*)(void* slotState, std::uint64_t key);

// Server-initiated data with nothing awaiting it (Postgres NOTIFY, Redis pub/sub), so a plain
// callback rather than a coroutine resume. Set *consumed to a complete message's length
// (0 = need more bytes); return false if undecodable, which closes the slot.
using EndpointOnPushFn = bool (*)(void* slotState, const char* buf, std::uint32_t len, std::uint32_t* consumed);

// Reports the protocol's own response status so the engine can bucket upstream results per
// endpoint. Only protocols that have such a concept implement it; everything else leaves it null
// and the status counters stay zero.
// Returning 0 means "no status for this response" and is not counted
using EndpointStatusCodeFn = std::uint16_t (*)(const void* outObj);

struct EndpointDesc {
    EndpointSerializeFn serialize;
    EndpointParseFn parse;
    EndpointOnConnectFn onConnect;               // nullable, skipped for simple protocols (Redis, etc)
    EndpointOnAbortFn onAbort;                   // nullable, single-slot requests only
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
    EndpointOnPushFn onPush;                     // nullable, null closes the slot on unsolicited bytes
    EndpointStatusCodeFn statusCode;             // nullable, feeds the per-endpoint status counters
    void* userCtx;                               // injected into createSlotState
};
static_assert(sizeof(EndpointDesc) == 152, "'EndpointDesc' must be exactly 152 bytes.");
static_assert(std::is_standard_layout_v<EndpointDesc>, "'EndpointDesc' must be standard layout");

struct EndpointConfig {
    std::uint32_t connLimit;             // Max simultaneous connections in the slot pool (see exactSlots)
    std::uint32_t auxConnLimit;          // Max simultaneous side connections; 0 = disabled (see exactSlots)
    std::uint32_t dnsRefreshSeconds;     // 0 = respect actual DNS TTL, N = override with N seconds
    std::uint16_t connectTimeoutSeconds; // TCP+TLS+onConnect must complete within this window
    std::uint16_t requestTimeoutSeconds; // Send+receive cycle must complete within this window
    std::uint32_t idleTimeoutSeconds;    // Idle slots are closed after this many seconds
    std::uint16_t maxReconnectAttempts;  // Max backoff attempts before slot is marked FATAL
    std::uint16_t reconnectBackoffBase;  // Initial backoff seconds
    std::uint16_t reconnectBackoffMax;   // Backoff cap seconds
    EndpointTLSConfig tlsConfig;         // TLS mode for this endpoint
    bool exactSlots;                     // Allocate connLimit/auxConnLimit exactly, else round up to 64
    std::uint32_t prewarm;               // Slots to connect eagerly on first epoll loop iteration
    std::uint32_t maxConcurrentStreams;  // Cap on requests sharing one slot; 0/1 = exclusive slot
    StringView alpnProtocols;            // Wire-encoded ALPN list; empty = offer http/1.1 only
};
static_assert(sizeof(EndpointConfig) == 56, "'EndpointConfig' must be exactly 56 bytes.");
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

// vvv Per-route and per-endpoint metrics vvv
// Counters only, kept deliberately small: these are always mapped, so every field here is paid
// for in production. Anything only interesting while chasing a specific bug belongs in a log
// line, not a permanent counter.
// Slots are indexed densely from 0, so the mmap only faults in the pages actually used, and the
// route name is not stored here: it is registration data, identical in every worker, and is
// attached when the metrics are read.
struct RouteMetrics {
    std::uint64_t requests = 0;
    std::uint64_t status1xx = 0;
    std::uint64_t status2xx = 0;
    std::uint64_t status3xx = 0;
    std::uint64_t status4xx = 0;
    std::uint64_t status5xx = 0;
    std::uint64_t bytesOut = 0;
};
static_assert(sizeof(RouteMetrics) == 56, "'RouteMetrics' must be exactly 56 bytes");
static_assert(std::is_standard_layout_v<RouteMetrics>, "'RouteMetrics' must be standard layout");

// Errors are split only where the split changes what you would do about it: unreachable host, bad
// TLS, slow upstream and a pool too small are four different fixes. Everything else shares one
// bucket, since narrowing it further is a debugging job rather than a monitoring one.
// The status counters are filled from EndpointDesc::statusCode, so they stay zero for protocols
// that have no such concept.
struct EndpointMetrics {
    std::uint64_t requests = 0;
    std::uint64_t completed = 0;
    std::uint64_t status1xx = 0;
    std::uint64_t status2xx = 0;
    std::uint64_t status3xx = 0;
    std::uint64_t status4xx = 0;
    std::uint64_t status5xx = 0;
    std::uint64_t connectFailures = 0;
    std::uint64_t tlsFailures = 0;
    std::uint64_t requestTimeouts = 0;
    std::uint64_t poolExhausted = 0;
    std::uint64_t otherErrors = 0;
    std::uint64_t reconnects = 0;
    std::uint64_t coalesceHits = 0;
    std::uint64_t bytesOut = 0;
    std::uint64_t bytesIn = 0;
    std::uint64_t slotsInUse = 0; // Gauge, current leases against connLimit
};
static_assert(sizeof(EndpointMetrics) == 136, "'EndpointMetrics' must be exactly 136 bytes");
static_assert(std::is_standard_layout_v<EndpointMetrics>, "'EndpointMetrics' must be standard layout");

// Latency lives in its own array, mapped only when [Metrics] latency is on
// Production wants to know what happened, a perf run wants to know how long it took, and the
// second question costs two clock reads per request and far more memory than the counters.
//
// 24 power-of-two ranges of 8 linear sub-buckets cover 1us to ~16s. A value reported at its
// bucket midpoint is within 6.25% of the truth, which is tight enough to tell a p99 regression
// from noise. Recording is a shift and an increment, percentiles are rebuilt at scrape time.
inline constexpr std::uint32_t LATENCY_BUCKET_COUNT = 192;

struct LatencyMetrics {
    std::uint64_t sumUs = 0;
    std::uint64_t buckets[LATENCY_BUCKET_COUNT] = {};
};
static_assert(sizeof(LatencyMetrics) == 1544, "'LatencyMetrics' must be exactly 1544 bytes");
static_assert(std::is_standard_layout_v<LatencyMetrics>, "'LatencyMetrics' must be standard layout");

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
};
static_assert(sizeof(NetworkMetrics) == 80, "'NetworkMetrics' must be exactly 80 bytes");
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

// Counters plus the identity they belong to, assembled at read time: the counters come from the
// mmap, the identity from registration data. path and host are views into engine-owned strings
// that live for the whole process, so they stay valid for the scraping handler's lifetime.
struct RouteMetricsView {
    StringView path;
    HttpMethod method = HttpMethod::GET;
    RouteMetrics metrics;
};
static_assert(sizeof(RouteMetricsView) == 80, "'RouteMetricsView' must be exactly 80 bytes");
static_assert(std::is_standard_layout_v<RouteMetricsView>, "'RouteMetricsView' must be standard layout");

struct EndpointMetricsView {
    StringView host;
    EndpointMetrics metrics;
};
static_assert(sizeof(EndpointMetricsView) == 152, "'EndpointMetricsView' must be exactly 152 bytes");
static_assert(std::is_standard_layout_v<EndpointMetricsView>, "'EndpointMetricsView' must be standard layout");

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