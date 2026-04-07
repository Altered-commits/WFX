#include "route_segment.hpp"
#include "shared/abis/uuid.hpp"

namespace WFX::Http {

RouteSegment::RouteSegment(std::string_view key, std::unique_ptr<TrieNode> c)
    : routeValue(SegmentVariant::FromString(StringView{key.data(), key.size()}, true)),
    child(std::move(c))
{}

RouteSegment::RouteSegment(SegmentVariant p, std::unique_ptr<TrieNode> c)
    : routeValue(p), child(std::move(c)) {}

// vvv Type checks vvv
bool RouteSegment::IsStatic() const
{
    return routeValue.Tag() == SEG_VARIANT_STC_STR;
}

bool RouteSegment::IsParam() const
{
    std::uint8_t t = routeValue.Tag();
    return t == SEG_VARIANT_U64
        || t == SEG_VARIANT_I64
        || t == SEG_VARIANT_STR
        || t == SEG_VARIANT_UUID;
}

// vvv Accessors vvv
const SegmentVariant* RouteSegment::GetParam() const
{
    if(!IsParam())
        return nullptr;

    return &routeValue;
}

TrieNode* RouteSegment::GetChild() const
{
    return child.get();
}

// vvv Utilities vvv
bool RouteSegment::MatchesStatic(std::string_view candidate) const
{
    if(!IsStatic())
        return false;

    auto sv = routeValue.AsString();

    return sv.Size() == candidate.size()
        && std::memcmp(sv.data, candidate.data(), sv.Size()) == 0;
}

ParamType RouteSegment::GetParamType() const
{
    switch(routeValue.Tag()) {
        case SEG_VARIANT_U64:  return ParamType::UINT;
        case SEG_VARIANT_I64:  return ParamType::INT;
        case SEG_VARIANT_STR:  return ParamType::STRING;
        case SEG_VARIANT_UUID: return ParamType::UUID;
        default:               return ParamType::UNKNOWN;
    }
}

std::string_view RouteSegment::ToString() const
{
    if(IsStatic())
        return "<static>";

    switch(GetParamType()) {
        case ParamType::UINT:   return "<uint>";
        case ParamType::INT:    return "<int>";
        case ParamType::STRING: return "<str>";
        case ParamType::UUID:   return "<uuid>";
        default:                return "<unknown>";
    }
}

} // namespace WFX::Http