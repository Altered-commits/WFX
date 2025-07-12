#ifndef WFX_HTTP_ROUTE_COMMON_HPP
#define WFX_HTTP_ROUTE_COMMON_HPP

#include "utils/uuid/uuid.hpp"

#include <string_view>
#include <cstdint>
#include <variant>
#include <vector>

// Bunch of stuff which will be used in routes and outside of routing as well
using DynamicSegment         = std::variant<std::uint64_t, std::int64_t, std::string_view, WFX::Utils::UUID>;
using StaticOrDynamicSegment = std::variant<std::string_view, DynamicSegment>;
using PathSegments           = std::vector<DynamicSegment>;

#endif // WFX_HTTP_ROUTE_COMMON_HPP