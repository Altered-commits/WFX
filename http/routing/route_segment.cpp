#include "route_segment.hpp"

namespace WFX::Http {

RouteSegment::RouteSegment(std::string_view key, std::unique_ptr<TrieNode> c)
    : routeValue(key), child(std::move(c)) {}

RouteSegment::RouteSegment(DynamicSegment p, std::unique_ptr<TrieNode> c)
    : routeValue(std::move(p)), child(std::move(c)) {}

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

TrieNode* RouteSegment::GetChild() const
{
    return child.get();
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
        return "<static>";
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