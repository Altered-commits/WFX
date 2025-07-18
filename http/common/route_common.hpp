#ifndef WFX_HTTP_ROUTE_COMMON_HPP
#define WFX_HTTP_ROUTE_COMMON_HPP

#include "utils/uuid/uuid.hpp"
#include "utils/backport/move_only_function.hpp"

#include <string_view>
#include <cstdint>
#include <variant>
#include <vector>

// Forward declare for 'HttpCallbackType'
namespace WFX::Http {

class HttpRequest;
class HttpResponse;

} // namespace WFX::Http

// Bunch of stuff which will be used in routes and outside of routing as well
using DynamicSegment         = std::variant<std::uint64_t, std::int64_t, std::string_view, WFX::Utils::UUID>;
using StaticOrDynamicSegment = std::variant<std::string_view, DynamicSegment>;
using PathSegments           = std::vector<DynamicSegment>;

// Used throughout the entire program, hopefully
using HttpCallbackType = WFX::Utils::MoveOnlyFunction<void(WFX::Http::HttpRequest&, WFX::Http::HttpResponse&)>;

#endif // WFX_HTTP_ROUTE_COMMON_HPP