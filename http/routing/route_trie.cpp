#include "route_trie.hpp"
#include "utils/backport/string.hpp"

namespace WFX::Http {

void RouteTrie::Insert(std::string_view fullRoute, HttpCallbackType handler)
{
    TrieNode* current = &root;

    while(!fullRoute.empty()) {
        std::size_t      slashPos = fullRoute.find('/');
        std::string_view segment  = (slashPos == std::string_view::npos)
                                    ? fullRoute
                                    : fullRoute.substr(0, slashPos);

        fullRoute = (slashPos == std::string_view::npos)
                    ? std::string_view{}
                    : fullRoute.substr(slashPos + 1);

        TrieNode* next = nullptr;

        // Dynamic segment
        if(!segment.empty() && segment.front() == '<' && segment.back() == '>') {
            if(segment.size() <= 2)
                Logger::GetInstance().Fatal("[Route-Formatter]: Empty parameter segment: ", segment, ". Example: <id:int> or <int>");

            auto        inner = segment.substr(1, segment.size() - 2);
            std::size_t colon = inner.find(':');

            // We only care about type, the identifier before ':' is like a comment for understanding only
            // We access data by indexes not identifiers
            std::string_view type;

            // Only type is provided: <int>, <string>
            if(colon == std::string_view::npos)
                type = inner;
            // Both comment and type is provided: <id:int>
            else {
                // Check for malformed usage like <:int> or <id:>
                if(colon == 0 || colon == inner.size() - 1)
                    Logger::GetInstance().Fatal(
                        "[Route-Formatter]: Malformed dynamic segment: ", segment, ". Example: <id:int> or <int>"
                    );

                type = inner.substr(colon + 1);
            }

            DynamicSegment dynSeg;
            if     (type == "uint")   dynSeg = std::uint64_t{0};
            else if(type == "int")    dynSeg = std::int64_t{0};
            else if(type == "uuid")   dynSeg = UUID{};
            else if(type == "string") dynSeg = std::string_view{};
            else
                Logger::GetInstance().Fatal(
                    "[Route-Formatter]: Unknown parameter type: '", type, "'. Valid types -> uint, int, uuid and string."
                );

            auto nextNode = std::make_unique<TrieNode>();
            next = nextNode.get();
            current->children.emplace_back(std::move(dynSeg), std::move(nextNode));
        }
        // Static segment
        else {
            bool found = false;
            for(auto& child : current->children) {
                if(child.IsStatic() && child.MatchesStatic(segment)) {
                    next  = child.GetChild();
                    found = true;
                    break;
                }
            }
            if(!found) {
                auto nextNode = std::make_unique<TrieNode>();
                next = nextNode.get();
                current->children.emplace_back(segment, std::move(nextNode));
            }
        }

        current = next;
    }

    current->callback = std::move(handler);
}

const HttpCallbackType* RouteTrie::Match(std::string_view requestPath, PathSegments& outParams) const {
    const TrieNode* current = &root;

    while(!requestPath.empty()) {
        std::size_t      slashPos = requestPath.find('/');
        std::string_view segment  = (slashPos == std::string_view::npos)
                                    ? requestPath
                                    : requestPath.substr(0, slashPos);
        requestPath = (slashPos == std::string_view::npos) ? std::string_view{} : requestPath.substr(slashPos + 1);

        const TrieNode* next = nullptr;
        DynamicSegment  paramCandidate;

        for(const auto& child : current->children) {
            if(child.IsStatic()) {
                if(child.MatchesStatic(segment)) {
                    next = child.GetChild();
                    break;
                }
            }
            else if(child.IsParam()) {
                ParamType type = child.GetParamType();

                switch(type)
                {
                    case ParamType::UINT:
                    {
                        std::uint64_t val;
                        if(!StrToUInt64(segment, val))
                            continue;
                        
                        paramCandidate = val;
                        break;
                    }

                    case ParamType::INT:
                    {
                        std::int64_t val;
                        if(!StrToInt64(segment, val))
                            continue;
                        
                        paramCandidate = val;
                        break;
                    }

                    case ParamType::UUID:
                    {
                        WFX::Utils::UUID uuid;
                        if(!WFX::Utils::UUID::FromString(segment, uuid))
                            continue;
                        
                        paramCandidate = uuid;
                        break;
                    }

                    case ParamType::STRING:
                        paramCandidate = segment;
                        break;

                    // Unknown or unsupported ParamType — this should not happen silently
                    default:
                        return nullptr;
                }

                // Save the matching param *only if no static matched already*
                if(!next) {
                    next = child.GetChild();
                    outParams.emplace_back(std::move(paramCandidate));
                }
            }
        }

        if(!next)
            return nullptr;

        current = next;
    }

    return (current->callback) ? std::addressof(current->callback) : nullptr;
}

} // namespace WFX::Http