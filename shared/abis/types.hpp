#ifndef WFX_SHARED_ABI_TYPES_HPP
#define WFX_SHARED_ABI_TYPES_HPP

#include "uuid.hpp"

// Fwd declare user side request and response
namespace WFX::Http {

struct Request;
struct Response;

} // namespace WFX::Http

namespace WFX::Shared {

// vvv Middleware Enums vvv
enum class MiddlewareLevel : std::uint8_t {
    GLOBAL = 0,
    PER_ROUTE
};

enum class MiddlewareAction : std::uint8_t {
    CONTINUE = 0,  // Continue to next middleware
    BREAK,         // Break out of middleware chain
    SKIP_NEXT      // Skip the next middleware in chain if any
};

// vvv Async vvv
enum class AsyncStatus : std::uint8_t {
    NONE = 0,
    COMPLETED,     // Mostly for internal use
    TIMER_FAILURE,
    IO_FAILURE,
    INTERNAL_FAILURE
};

struct AsyncResult {
    void*            data;
    std::uint32_t    dataLen;
    MiddlewareAction action;     // CONTINUE for non-middleware
    AsyncStatus      status;
};
static_assert(sizeof(AsyncResult) == 16, "'AsyncResult' must be exactly 16 bytes.");

using AsyncCompleteFn = void(*)(void* userData, AsyncResult result);
using AsyncDestroyFn  = void(*)(void* userData);

struct AsyncData {
    void*           userData;       // |
    AsyncCompleteFn AsyncComplete;  // | -> For cleaner storage on engine side i suppose
    AsyncDestroyFn  AsyncDestroy;   // |
};
static_assert(sizeof(AsyncData) == 24, "'AsyncData' must be exactly 24 bytes.");

// vvv Route Callbacks vvv
using SyncRouteFn  = void (*)(Http::Request, Http::Response);
using AsyncRouteFn = void (*)(Http::Request, Http::Response, AsyncCompleteFn onDone, void* onDoneUd);
using SyncMwFn     = MiddlewareAction (*)(Http::Request, Http::Response);
using AsyncMwFn    = void             (*)(Http::Request, Http::Response, AsyncCompleteFn onDone, void* onDoneUd);

enum class CallbackKind : std::uint8_t {
    SYNC = 0,
    ASYNC
};

struct RouteCallback {
    CallbackKind kind;
    union {
        SyncRouteFn  sync;
        AsyncRouteFn async;
    };

    bool IsEmpty() const noexcept
    {
        return kind == CallbackKind::SYNC ? sync  == nullptr : async == nullptr;
    }
};
static_assert(sizeof(RouteCallback) == 16, "'RouteCallback' must be exactly 16 bytes.");

struct MwCallback {
    CallbackKind kind;
    union {
        SyncMwFn  sync;
        AsyncMwFn async;
    };

    bool IsEmpty() const noexcept
    {
        return kind == CallbackKind::SYNC ? sync  == nullptr : async == nullptr;
    }
};
static_assert(sizeof(MwCallback) == 16, "'MwCallback' must be exactly 16 bytes.");

// vvv Outbound Streaming vvv
enum class StreamAction : std::uint8_t {
    CONTINUE = 0,
    STOP_AND_ALIVE_CONN,
    STOP_AND_CLOSE_CONN
};

struct StreamResult {
    std::size_t  writtenBytes;
    StreamAction action;
};
static_assert(sizeof(StreamResult) == 16, "'StreamResult' must be exactly 16 bytes.");

struct StreamBuffer {
    char*       buffer;
    std::size_t size;
};
static_assert(sizeof(StreamBuffer) == 16, "'StreamBuffer' must be exactly 16 bytes.");

struct StreamGenerator {
    void* ctx;

    StreamResult (*Next)(void* ctx, StreamBuffer buffer);
    void         (*Destroy)(void* ctx);
};
static_assert(sizeof(StreamGenerator) == 24, "'StreamGenerator' must be exactly 24 bytes.");

// vvv Endpoint vvv
enum class EndpointStatus : std::uint8_t {
    // Success
    SUCCESS = 0,        // Endpoint processing finished
    PENDING,            // Endpoint processing in progress

    // Buffer errors
    BUFFER_ERROR,        // Initialization failed, etc
    INSUFFICIENT_BUFFER, // Write / Read buffer insufficient

    // Pool errors
    INVALID_KEY,        // Index out of bounds in the endpoint pool
    POOL_EXHAUSTED,     // All endpoints in use

    // Socket errors
    SOCKET_FAILURE,     // Not created, options not set, etc
    CONNECT_FAILURE,    // Failed to connect to endpoint
    SSL_FAILURE,        // Client not created, Handshake failed, etc

    // Generic errors
    INTERNAL_ERROR      // Something went wrong
};

// Endpoint enum
enum class EndpointTLSConfig : std::uint8_t {
    AUTO = 0,       // TLS automatically on some preconfigured ports
    FORCE_REQUIRE,  // Force TLS (Port doesn't matter)
    FORCE_INSECURE  // Explicitly allow no TLS even on secure ports
};

// vvv Server Metrics vvv
// IMPORTANT: DO NOT CHANGE THE ORDER OF THESE METRICS, 'logger.hpp' depends on the order
struct LogMetrics {
    std::uint64_t trace = 0;
    std::uint64_t debug = 0;
    std::uint64_t info  = 0;
    std::uint64_t warn  = 0;
    std::uint64_t error = 0;
    std::uint64_t fatal = 0;
};
static_assert(sizeof(LogMetrics) == 48, "'LogMetrics' must be exactly 48 bytes");

struct NetworkMetrics {
    std::uint64_t accepts          = 0;
    std::uint64_t reads            = 0;
    std::uint64_t bytesRead        = 0;
    std::uint64_t writes           = 0;
    std::uint64_t bytesWritten     = 0;
    std::uint64_t fileCalls        = 0;
    std::uint64_t fileFallbacks    = 0;
    std::uint64_t fileBytesWritten = 0;
    std::uint64_t activeConns      = 0;
    std::uint64_t requests         = 0;
    std::uint64_t response1xx      = 0;
    std::uint64_t response2xx      = 0;
    std::uint64_t response3xx      = 0;
    std::uint64_t response4xx      = 0;
    std::uint64_t response5xx      = 0;
};
static_assert(sizeof(NetworkMetrics) == 120, "'NetworkMetrics' must be exactly 120 bytes");

// One slot per worker in shared mmap
// Embeds user-facing metric structs directly (add fields there, not here)
// alignas(64) prevents false sharing between adjacent worker slots
struct alignas(64) WorkerMetrics {
    LogMetrics     log     = {};
    NetworkMetrics network = {};
};
static_assert(sizeof(WorkerMetrics) % 64 == 0, "'WorkerMetrics' must be a multiple of 64 bytes");

} // namespace WFX::Shared

#endif // WFX_SHARED_ABI_TYPES_HPP