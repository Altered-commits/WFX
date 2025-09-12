#ifndef WFX_HTTP_SERIALIZER_HPP
#define WFX_HTTP_SERIALIZER_HPP

#include "http/response/http_response.hpp"
#include "utils/rw_buffer/rw_buffer.hpp"

namespace WFX::Http {

using namespace WFX::Utils; // For 'RWBuffer'

enum class SerializeResult : std::uint8_t {
    SERIALIZE_SUCCESS,
    SERIALIZE_BUFFER_FAILED,      // Allocation failed, buffer is nullptr
    SERIALIZE_BUFFER_TOO_SMALL,   // No data yet buffer is too small to hold data
    SERIALIZE_BUFFER_INSUFFICIENT // Buffer is too small to hold the serialized data
};

using SerializedHttpResponseDeprecated = std::pair<std::string, std::string_view>;
using SerializedHttpResponse           = std::pair<SerializeResult, std::string_view>;

// Being used as a namespace rn, fun again
class HttpSerializer final {
public:
    [[deprecated("Use SerializeToBuffer instead, this function works but doesn't use proper write buffering")]]
    static SerializedHttpResponseDeprecated Serialize(HttpResponse& res);

    static SerializedHttpResponse SerializeToBuffer(HttpResponse& res, RWBuffer& buffer);

private:
    HttpSerializer()  = delete;
    ~HttpSerializer() = delete;
};

} // namespace WFX::Http


#endif // WFX_HTTP_SERIALIZER_HPP