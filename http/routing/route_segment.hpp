#ifndef WFX_HTTP_ROUTE_SEGMENT_HPP
#define WFX_HTTP_ROUTE_SEGMENT_HPP

#include "http/common/http_route_common.hpp"

namespace WFX::Http {

enum class ParamType : std::uint8_t {
    UINT,
    INT,
    STRING,
    UUID,
    UNKNOWN
};

// Forward declare so TrieNode doesn't cry
struct RouteSegment;

// TODO: Optimize later like compressed_pair does, so only leaf nodes have callback use memory
// In rest of the nodes, callback shouldn't take any memory
struct TrieNode {
    // Child segments
    std::vector<RouteSegment> children;

    // Callback for GET or POST methods
    HttpCallbackType callback;
};

struct RouteSegment {
    StaticOrDynamicSegment routeValue;
    std::unique_ptr<TrieNode> child = nullptr;

    RouteSegment(std::string_view key, std::unique_ptr<TrieNode> c);
    RouteSegment(DynamicSegment p, std::unique_ptr<TrieNode> c);

    // Delete copy constructor and assignment operator
    RouteSegment(const RouteSegment&) = delete;
    RouteSegment& operator=(const RouteSegment&) = delete;

    // Allow move constructor and move assignment
    RouteSegment(RouteSegment&&) noexcept = default;
    RouteSegment& operator=(RouteSegment&&) noexcept = default;

    // vvv Type Checks vvv
    bool IsStatic() const;
    bool IsParam()  const;

    // vvv Accessors vvv
    const std::string_view* GetStaticKey() const;
    const DynamicSegment*   GetParam()     const;
          TrieNode*         GetChild()     const;

    // vvv Utilities vvv
    bool             MatchesStatic(std::string_view candidate) const;
    ParamType        GetParamType()                            const;
    std::string_view ToString()                                const;
};

} // namespace WFX::Http

#endif // WFX_HTTP_ROUTE_SEGMENT_HPP