#ifndef WFX_HTTP_ROUTE_COMMON_HPP
#define WFX_HTTP_ROUTE_COMMON_HPP

#include "async/interface.hpp"
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

// vvv Middleware (Streaming & Sync) vvv
/*
 * NOTE: I expect that middleware will not be more than uint16_t max value (aka 65535)
 *       Cuz it makes no sense for someone to have that many middlewares realistically
 */
enum class MiddlewareAction {
    CONTINUE,  // Continue to next middleware
    BREAK,     // Break out of middleware chain
    SKIP_NEXT  // Skip the next middleware in chain if any
};

enum class MiddlewareType {
    SYNC,       // For normal use case
    CHUNK_BODY, // Streaming inbound chunks
    CHUNK_END   // Streaming inbound last chunk
};

struct MiddlewareBuffer {
    const char* buffer;
    std::size_t size;
};

using SyncMiddlewareFn      = MiddlewareAction (*)(WFX::Http::HttpRequest&, Response&);
using ChunkBodyMiddlewareFn = MiddlewareAction (*)(WFX::Http::HttpRequest&, Response&, MiddlewareBuffer);
using ChunkEndMiddlewareFn  = SyncMiddlewareFn;

struct MiddlewareEntry {
public: // Helpers
    constexpr static std::uint16_t END = UINT16_MAX;

public: // Main
    SyncMiddlewareFn      sm  = nullptr;
    ChunkBodyMiddlewareFn cbm = nullptr;
    ChunkEndMiddlewareFn  cem = nullptr;

    // Values set by middleware handler, used for fast traversal for a specific type of group
    // uint16_t max value is considered to be invalid value (it can be used to signify end)
    std::uint16_t nextSm  = END;    // Index of next sync capable middleware
    std::uint16_t nextCbm = END;    // Index of next chunk capable middleware
    std::uint16_t nextCem = END;    // Index of next chunk end capable middleware
};

using MiddlewareStack = std::vector<MiddlewareEntry>;

// vvv User Callbacks vvv
using AsyncCallbackType = WFX::Utils::MoveOnlyFunction<AsyncPtr(WFX::Http::HttpRequest&, Response&)>;  
using SyncCallbackType  = void (*)(WFX::Http::HttpRequest&, Response&);
using HttpCallbackType  = std::variant<std::monostate, SyncCallbackType, AsyncCallbackType>;

#endif // WFX_HTTP_ROUTE_COMMON_HPP