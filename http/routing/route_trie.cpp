// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "route_trie.hpp"
#include "utils/string/string.hpp"
#include "utils/diagnostics/logger.hpp"

namespace WFX::Http {

using namespace WFX::Utils;  // For 'Logger', 'StringUtils'
using namespace WFX::Shared; // For every single abi type

TrieNode* RouteTrie::Insert(std::string_view fullRoute, RouteCallback handler)
{
    TrieNode* node = InsertRoute(fullRoute);
    node->callback = handler;
    return node;
}

const TrieNode* RouteTrie::Match(std::string_view requestPath, PathSegments& outParams) const
{
    return MatchNode(&root_, StripRoute(requestPath), outParams);
}

void RouteTrie::PushGroup(std::string_view prefix)
{
    cursorStack_.push_back(insertCursor_);
    insertCursor_ = InsertRoute(prefix);
}

void RouteTrie::PopGroup()
{
    if(cursorStack_.empty())
        GetLogger().Fatal("[RouteTrie]: PopGroup called without corresponding PushGroup.");

    insertCursor_ = cursorStack_.back();
    cursorStack_.pop_back();
}

// vvv Helper Functions vvv
const TrieNode* RouteTrie::MatchNode(const TrieNode* node, std::string_view requestPath, PathSegments& outParams) const
{
    // Whole path consumed: this node answers only if a handler was registered on it
    if(requestPath.empty())
        return node->callback.IsEmpty() ? nullptr : node;

    const std::size_t slashPos = requestPath.find('/');
    const std::string_view segment =
        (slashPos == std::string_view::npos) ? requestPath : requestPath.substr(0, slashPos);
    const std::string_view rest =
        (slashPos == std::string_view::npos) ? std::string_view{} : requestPath.substr(slashPos + 1);

    for(const auto& child : node->children) {
        if(child.IsStatic()) {
            // Wildcard '*' swallows this segment and everything after it into one STRING capture
            if(child.MatchesStatic("*")) {
                std::size_t len = segment.size();

                if(!rest.empty())
                    len += 1 + rest.size(); // include the '/' and the rest of the path

                outParams.emplace_back(SegmentVariant::FromString(StringView{segment.data(), len}));
                if(const TrieNode* hit = MatchNode(child.GetChild(), std::string_view{}, outParams))
                    return hit;

                outParams.pop_back();
                continue;
            }

            // A static match captures nothing, so a dead end below just moves on to the next sibling
            if(child.MatchesStatic(segment)) {
                if(const TrieNode* hit = MatchNode(child.GetChild(), rest, outParams))
                    return hit;

                continue;
            }
        }
        else if(child.IsParam()) {
            switch(child.GetParamType()) {
                case ParamType::UINT: {
                    std::uint64_t val;
                    if(!StringUtils::StrToUInt64(segment, val))
                        continue;

                    outParams.emplace_back(SegmentVariant::FromU64(val));
                    break;
                }

                case ParamType::INT: {
                    std::int64_t val;
                    if(!StringUtils::StrToInt64(segment, val))
                        continue;

                    outParams.emplace_back(SegmentVariant::FromI64(val));
                    break;
                }

                case ParamType::UUID: {
                    Shared::UUID uuid;
                    if(!Shared::UUID::FromString(Shared::StringView{segment.data(), segment.size()}, uuid))
                        continue;

                    outParams.emplace_back(SegmentVariant::FromUUID(uuid));
                    break;
                }

                case ParamType::STRING:
                    outParams.emplace_back(SegmentVariant::FromString(StringView{segment.data(), segment.size()}));
                    break;

                // Unknown or unsupported ParamType, this should not happen silently
                default:
                    return nullptr;
            }

            if(const TrieNode* hit = MatchNode(child.GetChild(), rest, outParams))
                return hit;

            // This sibling captured the segment but could not complete the path, drop it and move on
            outParams.pop_back();
            continue;
        }
    }

    return nullptr;
}

TrieNode* RouteTrie::InsertRoute(std::string_view route)
{
    auto& logger = GetLogger();

    TrieNode* current = insertCursor_;
    route = StripRoute(route);

    while(!route.empty()) {
        const std::size_t slashPos = route.find('/');
        const std::string_view segment = (slashPos == std::string_view::npos) ? route : route.substr(0, slashPos);

        route = (slashPos == std::string_view::npos) ? std::string_view{} : route.substr(slashPos + 1);

        TrieNode* next = nullptr;

        // Dynamic segment
        if(!segment.empty() && segment.front() == '<' && segment.back() == '>') {
            if(segment.size() <= 2)
                logger.Fatal("[Route-Formatter]: Empty parameter segment: ", segment, ". Example: <id:int> or <int>");

            auto inner = segment.substr(1, segment.size() - 2);
            const std::size_t colon = inner.find(':');

            std::string_view type;

            if(colon == std::string_view::npos)
                type = inner;
            else {
                if(colon == 0 || colon == inner.size() - 1)
                    logger.Fatal("[Route-Formatter]: Malformed dynamic segment: ", segment,
                                 ". Example: <id:int> or <int>");

                type = inner.substr(colon + 1);
            }

            SegmentVariant dynSeg;
            if(type == "uint")
                dynSeg = SegmentVariant::FromU64(0);
            else if(type == "int")
                dynSeg = SegmentVariant::FromI64(0);
            else if(type == "uuid")
                dynSeg = SegmentVariant::FromUUID(UUID{});
            else if(type == "string")
                dynSeg = SegmentVariant::FromString(StringView{});
            else
                logger.Fatal("[Route-Formatter]: Unknown parameter type: '", type,
                             "'. Valid types -> uint, int, uuid and string.");

            auto nextNode = std::make_unique<TrieNode>();
            next = nextNode.get();
            current->children.emplace_back(dynSeg, std::move(nextNode));
        }
        // Static segment
        else {
            bool found = false;
            for(auto& child : current->children) {
                if(child.IsStatic() && child.MatchesStatic(segment)) {
                    next = child.GetChild();
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
                        logger.Fatal("[Route-Formatter]: Wildcard '*' must be the last segment in a route.");

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