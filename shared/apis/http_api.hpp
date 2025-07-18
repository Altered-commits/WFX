#ifndef WFX_SHARED_HTTP_API_HPP
#define WFX_SHARED_HTTP_API_HPP

#include "shared/utils/export_macro.hpp"
#include "http/constants/http_constants.hpp"
#include "http/common/route_common.hpp"

#include <string_view>

namespace WFX::Shared {

using namespace WFX::Http; // For 'HttpMethod'

enum class HttpAPIVersion : std::uint8_t {
    V1 = 1,
};

// vvv All aliases for clarity vvv
using RegisterRouteFn = void (*)(HttpMethod method, std::string_view path, HttpCallbackType callback);

// vvv API declarations vvv
struct HTTP_API_TABLE {
    RegisterRouteFn RegisterRoute;

    HttpAPIVersion apiVersion;
};

// vvv Getter vvv
const HTTP_API_TABLE* GetHttpAPIV1();

} // namespace WFX::Shared

#endif // WFX_SHARED_HTTP_API_HPP
