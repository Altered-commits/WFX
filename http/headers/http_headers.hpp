#ifndef WFX_HTTP_HEADERS_HPP
#define WFX_HTTP_HEADERS_HPP

#include <string>
#include <unordered_map>
#include <vector>

namespace WFX::Http {

// Idk, looks good like this, too much std::string gives me cancer
using KeyType      = std::string_view;
using ConstKeyType = const KeyType&;

struct CaseInsensitiveHash {
    size_t operator()(ConstKeyType key) const;
};

struct CaseInsensitiveEqual {
    bool operator()(ConstKeyType lhs, ConstKeyType rhs) const;
};

using HttpHeaderMap = std::unordered_map<KeyType, KeyType, CaseInsensitiveHash, CaseInsensitiveEqual>;

// === HttpHeaders class === //
class HttpHeaders {
public:
    HttpHeaders() = default;

    void     SetHeader(ConstKeyType key, ConstKeyType value);
    bool     HasHeader(ConstKeyType key) const;
    KeyType  GetHeader(ConstKeyType key) const;
    void     RemoveHeader(ConstKeyType key);
    void     Clear();
    
    HttpHeaderMap& GetHeaderMap();

private:
    HttpHeaderMap headers_;
};

} // namespace WFX::Utils

#endif // WFX_HTTP_HEADERS_HPP