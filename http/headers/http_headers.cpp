#include "http_headers.hpp"

#include <algorithm>
#include <cctype>

namespace WFX::Http {

std::size_t CaseInsensitiveHash::operator()(ConstKeyType key) const
{
    constexpr std::size_t fnvPrime       = 1099511628211ULL;
    constexpr std::size_t fnvOffsetBasis = 14695981039346656037ULL;

    std::size_t hash = fnvOffsetBasis;
    for(char c : key) {
        hash ^= static_cast<unsigned char>(std::tolower(c));
        hash *= fnvPrime;
    }

    return hash;
}

bool CaseInsensitiveEqual::operator()(ConstKeyType lhs, ConstKeyType rhs) const
{
    if(lhs.size() != rhs.size()) return false;

    unsigned char result = 0;
    for(std::size_t i = 0; i < lhs.size(); ++i)
        result |= std::tolower(static_cast<unsigned char>(lhs[i])) ^
                  std::tolower(static_cast<unsigned char>(rhs[i]));

    return result == 0;
}

// === HttpHeaders Methods === //
void HttpHeaders::SetHeader(ConstKeyType key, ConstKeyType value)
{
    headers_[key] = value;
}

bool HttpHeaders::HasHeader(ConstKeyType key) const
{
    return headers_.find(key) != headers_.end();
}

KeyType HttpHeaders::GetHeader(ConstKeyType key) const
{
    auto it = headers_.find(key);
    return (it != headers_.end()) ? it->second : "";
}

void HttpHeaders::RemoveHeader(ConstKeyType key)
{
    headers_.erase(key);
}

HttpHeaderMap& HttpHeaders::GetHeaderMap()
{
    return headers_;
}

void HttpHeaders::Clear()
{
    headers_.clear();
}

} // namespace WFX