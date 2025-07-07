#ifndef WFX_HTTP_ROUTE_SEGMENT_HPP
#define WFX_HTTP_ROUTE_SEGMENT_HPP

#include <cstdint>
#include <string>
#include <variant>

#include "utils/uuid/uuid.hpp"

// Forward declaration to avoid circular inclusion
struct TrieNode;

using DynamicSegment         = std::variant<std::uint64_t, std::int64_t, std::string_view, WFX::Utils::UUID>;
using StaticOrDynamicSegment = std::variant<std::string_view, DynamicSegment>;

namespace WFX::Http {

enum class ParamType : std::uint8_t {
    UINT,
    INT,
    STRING,
    UUID,
    UNKNOWN
};

struct RouteSegment {
    StaticOrDynamicSegment routeValue;
    TrieNode* child = nullptr;

    RouteSegment(std::string_view key, TrieNode* c);
    RouteSegment(DynamicSegment p, TrieNode* c);

    // vvv Type Checks vvv
    bool IsStatic() const;
    bool IsParam()  const;

    // vvv Accessors vvv
    const std::string_view* GetStaticKey() const;
    const DynamicSegment*   GetParam()     const;

    // vvv Utilities vvv
    bool             MatchesStatic(std::string_view candidate) const;
    ParamType        GetParamType()                            const;
    std::string_view ToString()                                const;
};

} // namespace WFX::Http


#endif // WFX_HTTP_ROUTE_SEGMENT_HPP