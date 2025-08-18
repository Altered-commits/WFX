#ifndef WFX_HTTP_SERIALIZER_HPP
#define WFX_HTTP_SERIALIZER_HPP

#include "http/response/http_response.hpp"

namespace WFX::Http {

using SerializedHttpResponse = std::pair<std::string, std::string_view>;
    
// Being used as a namespace rn, fun again
class HttpSerializer final {
public:
    static SerializedHttpResponse Serialize(HttpResponse& res);

private:
    HttpSerializer()  = delete;
    ~HttpSerializer() = delete;
};

} // namespace WFX::Http


#endif // WFX_HTTP_SERIALIZER_HPP