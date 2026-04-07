#ifndef WFX_HTTP_ROUTE_SEGMENT_HPP
#define WFX_HTTP_ROUTE_SEGMENT_HPP

#include "shared/abis/types.hpp"
#include "shared/abis/segment_variant.hpp"
#include <vector>
#include <memory>

namespace WFX::Http {

using namespace WFX::Shared;

using PathSegments = std::vector<SegmentVariant>;

enum class ParamType : std::uint8_t {
    UINT,
    INT,
    STRING,
    UUID,
    UNKNOWN
};

struct RouteSegment;

struct TrieNode {
    std::vector<RouteSegment> children;
    RouteCallback             callback;
};

struct RouteSegment {
    SegmentVariant            routeValue;
    std::unique_ptr<TrieNode> child = nullptr;

    RouteSegment(std::string_view key, std::unique_ptr<TrieNode> c);
    RouteSegment(SegmentVariant p, std::unique_ptr<TrieNode> c);

    RouteSegment(const RouteSegment&)            = delete;
    RouteSegment& operator=(const RouteSegment&) = delete;
    RouteSegment(RouteSegment&&)                 noexcept = default;
    RouteSegment& operator=(RouteSegment&&)      noexcept = default;

    // vvv Type Checks vvv
    bool IsStatic() const;
    bool IsParam()  const;

    // vvv Accessors vvv
    const SegmentVariant* GetParam()     const;
          TrieNode*       GetChild()     const;

    // vvv Utilities vvv
    bool             MatchesStatic(std::string_view candidate) const;
    ParamType        GetParamType()                            const;
    std::string_view ToString()                                const;
};

} // namespace WFX::Http

#endif // WFX_HTTP_ROUTE_SEGMENT_HPP