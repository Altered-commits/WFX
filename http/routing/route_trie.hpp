#ifndef WFX_HTTP_ROUTE_TRIE_HPP
#define WFX_HTTP_ROUTE_TRIE_HPP

#include "route_segment.hpp"

#include <string_view>

namespace WFX::Http {

using namespace WFX::Utils; // For 'MoveOnlyFunction', 'Logger'

class RouteTrie {
public:    
    void                    Insert(std::string_view fullRoute, HttpCallbackType handler);
    const HttpCallbackType* Match(std::string_view requestPath, PathSegments& outParams) const;

    void PushGroup(std::string_view prefix);
    void PopGroup();

private: // Helper functions
    TrieNode* InsertRoute(std::string_view route);
    static std::string_view StripRoute(std::string_view route);

private:
    TrieNode root_;
    TrieNode* insertCursor_ = &root_;     // Current node where routes get inserted to
    std::vector<TrieNode*> cursorStack_; // For nesting
};

} // namespace WFX::Http

#endif // WFX_HTTP_ROUTE_TRIE_HPP