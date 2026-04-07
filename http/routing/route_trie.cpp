#include "route_trie.hpp"
#include "utils/backport/string.hpp"
#include "utils/logger/logger.hpp"

namespace WFX::Http {

const TrieNode* RouteTrie::Insert(std::string_view fullRoute, RouteCallback handler)
{
    TrieNode* node = InsertRoute(fullRoute);
    node->callback = handler;
    return node;
}

const TrieNode* RouteTrie::Match(std::string_view requestPath, PathSegments& outParams) const
{
    const TrieNode* current = &root_;
    requestPath = StripRoute(requestPath);

    while(!requestPath.empty()) {
        std::size_t      slashPos = requestPath.find('/');
        std::string_view segment  = (slashPos == std::string_view::npos)
                                    ? requestPath
                                    : requestPath.substr(0, slashPos);

        requestPath = (slashPos == std::string_view::npos)
                        ? std::string_view{}
                        : requestPath.substr(slashPos + 1);

        const TrieNode* next = nullptr;

        for(const auto& child : current->children) {
            if(child.IsStatic()) {
                // Special Case: Wildcard '*' match, we copy the whole thing into DynamicSegment STRING type
                if(child.MatchesStatic("*")) {
                    std::size_t len = segment.size();

                    if(!requestPath.empty())
                        len += 1 + requestPath.size(); // include '/' and rest of path

                    outParams.emplace_back(
                        SegmentVariant::FromString(StringView{ segment.data(), len })
                    );

                    // Signal that wildcard consumed all the remaining path
                    requestPath = std::string_view{};
                    next = child.GetChild();
                    break;
                }

                // Normal static match
                if(child.MatchesStatic(segment)) {
                    next = child.GetChild();
                    break;
                }
            }
            else if(child.IsParam()) {
                switch(child.GetParamType()) {
                    case ParamType::UINT:
                    {
                        std::uint64_t val;
                        if(!Utils::StrToUInt64(segment, val))
                            continue;

                        outParams.emplace_back(SegmentVariant::FromU64(val));
                        break;
                    }

                    case ParamType::INT:
                    {
                        std::int64_t val;
                        if(!Utils::StrToInt64(segment, val))
                            continue;

                        outParams.emplace_back(SegmentVariant::FromI64(val));
                        break;
                    }

                    case ParamType::UUID:
                    {
                        Shared::UUID uuid;
                        if(!Shared::UUID::FromString(
                            Shared::StringView{segment.data(), segment.size()},
                            uuid
                        ))
                            continue;

                        outParams.emplace_back(SegmentVariant::FromUUID(uuid));
                        break;
                    }

                    case ParamType::STRING:
                        outParams.emplace_back(
                            SegmentVariant::FromString(StringView{segment.data(), segment.size()})
                        );
                        break;

                    // Unknown or unsupported ParamType, this should not happen silently
                    default:
                        return nullptr;
                }

                // Match found for dynamic segment, store parameter and proceed
                next = child.GetChild();
            }
        }

        if(!next)
            return nullptr;

        current = next;
    }

    // No callback registered on this node
    if(current->callback.IsEmpty())
        return nullptr;

    return current;
}

void RouteTrie::PushGroup(std::string_view prefix)
{
    cursorStack_.push_back(insertCursor_);
    insertCursor_ = InsertRoute(prefix);
}

void RouteTrie::PopGroup()
{
    if(cursorStack_.empty())
        Utils::Logger::GetInstance().Fatal("[RouteTrie]: PopGroup called without corresponding PushGroup.");

    insertCursor_ = cursorStack_.back();
    cursorStack_.pop_back();
}

// vvv Helper Functions vvv
TrieNode* RouteTrie::InsertRoute(std::string_view route)
{
    auto& logger = Utils::Logger::GetInstance();

    TrieNode* current = insertCursor_;
    route = StripRoute(route);

    while(!route.empty()) {
        std::size_t      slashPos = route.find('/');
        std::string_view segment  = (slashPos == std::string_view::npos)
                                    ? route
                                    : route.substr(0, slashPos);

        route = (slashPos == std::string_view::npos)
                    ? std::string_view{}
                    : route.substr(slashPos + 1);

        TrieNode* next = nullptr;

        // Dynamic segment
        if(!segment.empty() && segment.front() == '<' && segment.back() == '>') {
            if(segment.size() <= 2)
                logger.Fatal(
                    "[Route-Formatter]: Empty parameter segment: ", segment, ". Example: <id:int> or <int>"
                );

            auto        inner = segment.substr(1, segment.size() - 2);
            std::size_t colon = inner.find(':');

            std::string_view type;

            if(colon == std::string_view::npos)
                type = inner;
            else {
                if(colon == 0 || colon == inner.size() - 1)
                    logger.Fatal(
                        "[Route-Formatter]: Malformed dynamic segment: ", segment, ". Example: <id:int> or <int>"
                    );

                type = inner.substr(colon + 1);
            }

            SegmentVariant dynSeg;
            if     (type == "uint")   dynSeg = SegmentVariant::FromU64(0);
            else if(type == "int")    dynSeg = SegmentVariant::FromI64(0);
            else if(type == "uuid")   dynSeg = SegmentVariant::FromUUID(UUID{});
            else if(type == "string") dynSeg = SegmentVariant::FromString(StringView{});
            else
                logger.Fatal(
                    "[Route-Formatter]: Unknown parameter type: '", type, "'. Valid types -> uint, int, uuid and string."
                );

            auto nextNode = std::make_unique<TrieNode>();
            next = nextNode.get();
            current->children.emplace_back(dynSeg, std::move(nextNode));
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
                // Insert current segment as static node
                auto nextNode = std::make_unique<TrieNode>();
                next = nextNode.get();
                current->children.emplace_back(segment, std::move(nextNode));

                // Special Case: We have wildcard '*'
                // It will be the last thing in the route. Any other stuff following it will lead to an error
                if(segment == "*") {
                    if(!route.empty())
                        logger.Fatal(
                            "[Route-Formatter]: Wildcard '*' must be the last segment in a route."
                        );

                    current = next;
                    break;
                }
            }
        }

        current = next;
    }

    return current;
}

std::string_view RouteTrie::StripRoute(std::string_view route)
{
    if(!route.empty() && route.front() == '/')
        return route.substr(1);

    return route;
}

} // namespace WFX::Http