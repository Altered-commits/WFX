#ifndef WFX_SHARED_HTTP_COMMON_HPP
#define WFX_SHARED_HTTP_COMMON_HPP

#include "async/task.hpp"
#include "utils/uuid/uuid.hpp"
#include "utils/backport/move_only_function.hpp"

#include <string_view>
#include <cstdint>
#include <variant>
#include <vector>

// Forward declare for 'HttpCallbackType' and other stuff using this file
namespace WFX::Http {
    // vvv Structs vvv
    struct TrieNode;
    struct HttpRequest;
    struct HttpResponse;
    struct ConnectionContext;

    // vv Classes vvv
    class Router;
    class HttpMiddleware;
    class HttpConnectionHandler;
} // namespace WFX::Http

// Defined in user side of code (include/http/response.hpp)
class Response;

// Defined in user side of code (include/http/stream_response.hpp)
class StreamResponse;

namespace WFX::Shared {

// Bunch of stuff which will be used in routes and outside of routing as well
using DynamicSegment         = std::variant<std::uint64_t, std::int64_t, std::string_view, ::WFX::Utils::UUID>;
using StaticOrDynamicSegment = std::variant<std::string_view, DynamicSegment>;
using PathSegments           = std::vector<DynamicSegment>;

// vvv Outbound Streaming vvv
enum class StreamAction {
    CONTINUE,
    STOP_AND_ALIVE_CONN,
    STOP_AND_CLOSE_CONN
};

struct StreamResult {
    std::size_t  writtenBytes;
    StreamAction action;
};

struct StreamBuffer {
    char*       buffer;
    std::size_t size;
};

using StreamGenerator = WFX::Utils::MoveOnlyFunction<StreamResult(StreamBuffer)>;

// vvv Middleware (Sync & Async) vvv
enum class MiddlewareAction : std::uint8_t {
    CONTINUE,  // Continue to next middleware
    BREAK,     // Break out of middleware chain
    SKIP_NEXT  // Skip the next middleware in chain if any
};

// So for async routes we need this to determine whether we are executing global-
// -mw or per route middleware
enum class MiddlewareLevel : std::uint8_t {
    GLOBAL,
    PER_ROUTE
};

using SyncMiddlewareType  = MiddlewareAction (*)(WFX::Http::HttpRequest&, Response);
using AsyncMiddlewareType = Async::Task<MiddlewareAction> (*)(WFX::Http::HttpRequest&, Response);
using HttpMiddlewareType  = std::variant<std::monostate, SyncMiddlewareType, AsyncMiddlewareType>;
using HttpMiddlewareStack = std::vector<HttpMiddlewareType>;

// vvv User Callbacks vvv
using AsyncCallbackType = Async::Task<void> (*)(WFX::Http::HttpRequest&, Response);  
using SyncCallbackType  = void (*)(WFX::Http::HttpRequest&, Response);
using HttpCallbackType  = std::variant<std::monostate, SyncCallbackType, AsyncCallbackType>;

// vvv Some commonly used async aliases vvv
using AsyncVoid             = Async::Task<void>;
using AsyncMiddlewareAction = Async::Task<MiddlewareAction>;

// vvv Endpoint vvv
enum class EndpointStatus : std::uint8_t {
    // Success
    SUCCESS,            // Endpoint processing finished
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

#endif // WFX_SHARED_HTTP_COMMON_HPP