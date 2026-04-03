#ifndef WFX_SHARED_ABI_TYPES_HPP
#define WFX_SHARED_ABI_TYPES_HPP

#include <cstdint>

namespace WFX::Shared {

// vvv Middleware (Sync & Async) vvv
// So for async routes we need this to determine whether we are executing global-
// -mw or per route middleware
enum class MiddlewareLevel : std::uint8_t {
    GLOBAL = 0,
    PER_ROUTE
};

enum class MiddlewareAction : std::uint8_t {
    CONTINUE = 0,  // Continue to next middleware
    BREAK,         // Break out of middleware chain
    SKIP_NEXT      // Skip the next middleware in chain if any
};

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

} // namespace WFX::Shared

#endif // WFX_SHARED_ABI_TYPES_HPP