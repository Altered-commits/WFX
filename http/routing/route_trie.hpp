#ifndef WFX_HTTP_ROUTE_TRIE_HPP
#define WFX_HTTP_ROUTE_TRIE_HPP

#include "route_segment.hpp"
#include "http/request/http_request.hpp"
#include "http/response/http_response.hpp"
#include "utils/backport/move_only_function.hpp"

#include <string_view>
#include <vector>
#include <unordered_map>
#include <memory>

namespace WFX::Http {

using namespace WFX::Utils; // For 'MoveOnlyFunction'

using CallbackType = MoveOnlyFunction<void(HttpRequest&, HttpResponse&)>;

struct TrieNode {
    // Child segments
    std::vector<RouteSegment> children;

    // Callback for GET or POST methods
    CallbackType callback = nullptr;
};

struct RouteTrie {
    TrieNode root;

    // To register a new route ("/path/<id:int>", etc)
    void Insert(std::string_view fullRoute, CallbackType handler);

    // To match a full route string and extract any parameters
    CallbackType* Match(std::string_view requestPath, std::vector<DynamicSegment>& outParams) const;
};

} // namespace WFX::Http

#endif // WFX_HTTP_ROUTE_TRIE_HPP