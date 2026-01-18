#ifndef WFX_HTTP_ROUTE_COMMON_HPP
#define WFX_HTTP_ROUTE_COMMON_HPP

#include "async/task.hpp"
#include "utils/uuid/uuid.hpp"
#include "utils/backport/move_only_function.hpp"

#include <string_view>
#include <cstdint>
#include <variant>
#include <vector>

// Forward declare for 'HttpCallbackType' and other stuff using this file
namespace WFX::Http {
    struct HttpRequest;
    struct HttpResponse;
} // namespace WFX::Http

// Defined in user side of code (include/http/response.hpp)
class Response;

// Defined in user side of code (include/http/stream_response.hpp)
class StreamResponse;

// Bunch of stuff which will be used in routes and outside of routing as well
using DynamicSegment         = std::variant<std::uint64_t, std::int64_t, std::string_view, WFX::Utils::UUID>;
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

#endif // WFX_HTTP_ROUTE_COMMON_HPP