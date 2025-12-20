
namespace WFX::Http {

template<typename T>
std::size_t CaseInsensitiveHash::operator()(const T& key) const
{
    static_assert(
        std::is_same_v<std::decay_t<T>, std::string> ||
        std::is_same_v<std::decay_t<T>, std::string_view>,
        "CaseInsensitiveHash: key must be std::string or std::string_view"
    );
    return WFX::Utils::Hasher::Fnv1aCaseInsensitive(key);
}

template<typename T1, typename T2>
bool CaseInsensitiveEqual::operator()(const T1& lhs, const T2& rhs) const
{
    static_assert(
        (std::is_same_v<std::decay_t<T1>, std::string> || std::is_same_v<std::decay_t<T1>, std::string_view>) &&
        (std::is_same_v<std::decay_t<T2>, std::string> || std::is_same_v<std::decay_t<T2>, std::string_view>),
        "CaseInsensitiveEqual: both operands must be std::string or std::string_view"
    );
    return WFX::Utils::StringCanonical::CTInsensitiveStringCompare(lhs, rhs);
}

template<typename K, typename V>
HttpHeaders<K, V>::HttpHeaders()
{
    // In most HTTP responses we have around 6–12 headers per request/response, so yeah
    // Pre reserve to prevent rehashing
    headers_.reserve(12);
}

template<typename K, typename V>
void HttpHeaders<K, V>::SetHeader(K key, V value)
{
    headers_[std::move(key)] = std::move(value);
}

template<typename K, typename V>
bool HttpHeaders<K, V>::HasHeader(const K& key) const
{
    return headers_.find(key) != headers_.end();
}

template<typename K, typename V>
V HttpHeaders<K, V>::GetHeader(const K& key) const
{
    auto it = headers_.find(key);
    return it != headers_.end() ? it->second : V{};
}

template<typename K, typename V>
typename HttpHeaders<K, V>::HeaderResult
HttpHeaders<K, V>::CheckAndGetHeader(const K& key) const
{
    auto it = headers_.find(key);
    if(it != headers_.end())
        return { true, &it->second };

    return { false, nullptr };
}

template<typename K, typename V>
void HttpHeaders<K, V>::RemoveHeader(const K& key)
{
    headers_.erase(key);
}

template<typename K, typename V>
void HttpHeaders<K, V>::Clear()
{
    headers_.clear();
}

template<typename K, typename V>
typename HttpHeaders<K, V>::HeaderMapType& HttpHeaders<K, V>::GetHeaderMap()
{
    return headers_;
}

template<typename K, typename V>
const typename HttpHeaders<K, V>::HeaderMapType& HttpHeaders<K, V>::GetHeaderMap() const
{
    return headers_;
}

} // namespace WFX