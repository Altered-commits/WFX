#ifndef WFX_HTTP_SERIALIZER_HPP
#define WFX_HTTP_SERIALIZER_HPP

#include "http/response/http_response.hpp"

namespace WFX::Http {

// Being used as a namespace rn, fun again
class HttpSerializer {
public:
    static std::string Serialize(HttpResponse& res);

private:
    constexpr static int HEADER_RESERVE_SIZE_HINT = 512;
};

} // namespace WFX::Http


#endif // WFX_HTTP_SERIALIZER_HPP