#ifndef WFX_HTTP_HEADERS_HPP
#define WFX_HTTP_HEADERS_HPP

#include "utils/crypt/hash.hpp"
#include "utils/crypt/string.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace WFX::Http {

struct CaseInsensitiveHash {
    template<typename T>
    std::size_t operator()(const T& key) const;
};

struct CaseInsensitiveEqual {
    template<typename T1, typename T2>
    bool operator()(const T1& lhs, const T2& rhs) const;
};

// --- Generic HttpHeaders ---
template<typename KeyType, typename ValType>
class HttpHeaders {
public:
    using HeaderMapType = std::unordered_map<KeyType, ValType, CaseInsensitiveHash, CaseInsensitiveEqual>;

    HttpHeaders();

    void    SetHeader(KeyType key, ValType value);
    bool    HasHeader(const KeyType& key) const;
    ValType GetHeader(const KeyType& key) const;
    void    RemoveHeader(const KeyType& key);
    void    Clear();

    HeaderMapType& GetHeaderMap();
    const HeaderMapType& GetHeaderMap() const;

private:
    HeaderMapType headers_;
};

// vvv Type Aliases vvv
using RequestHeaders  = HttpHeaders<std::string_view, std::string_view>;
using ResponseHeaders = HttpHeaders<std::string, std::string>;

} // namespace WFX::Http

// For template definitions
#include "http_headers.ipp"

#endif // WFX_HTTP_HEADERS_HPP