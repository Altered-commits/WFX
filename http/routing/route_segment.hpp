#ifndef WFX_HTTP_ROUTE_SEGMENT_HPP
#define WFX_HTTP_ROUTE_SEGMENT_HPP

#include "shared/abis/types.hpp"
#include "shared/abis/segment_variant.hpp"
#include <vector>
#include <memory>

namespace WFX::Http {

using PathSegments = std::vector<Shared::SegmentVariant>;

enum class ParamType : std::uint8_t { UINT, INT, STRING, UUID, UNKNOWN };

struct RouteSegment;

struct TrieNode {
    std::vector<RouteSegment> children;
    Shared::RouteCallback callback;
};

struct RouteSegment {
    Shared::SegmentVariant routeValue{};
    std::unique_ptr<TrieNode> child = nullptr;

public:
    RouteSegment(std::string_view key, std::unique_ptr<TrieNode> c);
    RouteSegment(Shared::SegmentVariant p, std::unique_ptr<TrieNode> c);

    RouteSegment(const RouteSegment&) = delete;
    RouteSegment& operator=(const RouteSegment&) = delete;
    RouteSegment(RouteSegment&&) noexcept = default;
    RouteSegment& operator=(RouteSegment&&) noexcept = default;

public:
    // vvv Type Checks vvv
    bool IsStatic() const;
    bool IsParam() const;

    // vvv Accessors vvv
    const Shared::SegmentVariant* GetParam() const;
    TrieNode* GetChild() const;

    // vvv Utilities vvv
    bool MatchesStatic(std::string_view candidate) const;
    ParamType GetParamType() const;
    std::string_view ToString() const;
};

} // namespace WFX::Http

#endif // WFX_HTTP_ROUTE_SEGMENT_HPP