#ifndef WFX_SHARED_HTTP_COMMON_HPP
#define WFX_SHARED_HTTP_COMMON_HPP

#include "async/task.hpp"
#include "shared/abis/uuid.hpp"
#include "shared/abis/types.hpp"
#include "utils/backport/move_only_function.hpp"

#include <string_view>
#include <cstdint>
#include <variant>
#include <vector>

// Fwd declare stuff
namespace WFX::Http {

// Defined in user side of code (include/http/response.hpp)
struct Response;

// Defined in user side of code (include/http/response.hpp)
struct Request;

} // namespace WFX::Http

namespace WFX::Shared {

// Bunch of stuff which will be used in routes and outside of routing as well
using DynamicSegment         = std::variant<std::uint64_t, std::int64_t, std::string_view, UUID>;
using StaticOrDynamicSegment = std::variant<std::string_view, DynamicSegment>;
using PathSegments           = std::vector<DynamicSegment>;

// vvv Middleware (Sync & Async) vvv
using SyncMiddlewareType  = MiddlewareAction (*)(WFX::Http::Request, WFX::Http::Response);
using AsyncMiddlewareType = Async::Task<MiddlewareAction> (*)(WFX::Http::Request, WFX::Http::Response);
using HttpMiddlewareType  = std::variant<std::monostate, SyncMiddlewareType, AsyncMiddlewareType>;
using HttpMiddlewareStack = std::vector<HttpMiddlewareType>;

// vvv User Callbacks vvv
using AsyncCallbackType = Async::Task<void> (*)(WFX::Http::Request, WFX::Http::Response);  
using SyncCallbackType  = void (*)(WFX::Http::Request, WFX::Http::Response);
using HttpCallbackType  = std::variant<std::monostate, SyncCallbackType, AsyncCallbackType>;

// vvv Some commonly used async aliases vvv
using AsyncVoid             = Async::Task<void>;
using AsyncMiddlewareAction = Async::Task<MiddlewareAction>;

} // namespace WFX::Shared

#endif // WFX_SHARED_HTTP_COMMON_HPP