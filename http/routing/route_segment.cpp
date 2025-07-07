#include "route_segment.hpp"

namespace WFX::Http {

RouteSegment::RouteSegment(std::string_view key, TrieNode* c)
    : routeValue(key), child(c) {}

RouteSegment::RouteSegment(DynamicSegment p, TrieNode* c)
    : routeValue(std::move(p)), child(c) {}

// vvv Type checks vvv
bool RouteSegment::IsStatic() const
{
    return std::holds_alternative<std::string_view>(routeValue);
}

bool RouteSegment::IsParam() const
{
    return std::holds_alternative<DynamicSegment>(routeValue);
}

// vvv Accessors vvv 
const std::string_view* RouteSegment::GetStaticKey() const
{
    return std::get_if<std::string_view>(&routeValue);
}

const DynamicSegment* RouteSegment::GetParam() const
{
    return std::get_if<DynamicSegment>(&routeValue);
}

// vvv Utilities vvv
bool RouteSegment::MatchesStatic(std::string_view candidate) const
{
    if(auto key = GetStaticKey())
        return *key == candidate;
    return false;
}

ParamType RouteSegment::GetParamType() const
{
    if(const DynamicSegment* p = GetParam()) {
        if(std::holds_alternative<std::uint64_t>(*p))    return ParamType::UINT;
        if(std::holds_alternative<std::int64_t>(*p))     return ParamType::INT;
        if(std::holds_alternative<std::string_view>(*p)) return ParamType::STRING;
        if(std::holds_alternative<WFX::Utils::UUID>(*p)) return ParamType::UUID;
    }

    return ParamType::UNKNOWN;
}

std::string_view RouteSegment::ToString() const
{
    if(auto key = GetStaticKey())
        return *key;
    else {
        switch(GetParamType())
        {
            case ParamType::UINT:   return "<uint>";
            case ParamType::INT:    return "<int>";
            case ParamType::STRING: return "<str>";
            case ParamType::UUID:   return "<uuid>";
            default:                return "<unknown>";
        }
    }
}

} // namespace WFX::Http