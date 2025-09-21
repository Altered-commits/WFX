#include "http_api.hpp"

#include "http/response/http_response.hpp"
#include "http/routing/router.hpp"
#include "http/middleware/http_middleware.hpp"

namespace WFX::Shared {

using namespace WFX::Http; // For 'Router', 'Middleware'

const HTTP_API_TABLE* GetHttpAPIV1()
{
    static HTTP_API_TABLE __GlobalHttpAPIV1 = {
        // Routing
        [](HttpMethod method, std::string_view path, HttpCallbackType cb) {  // RegisterRoute
            Router::GetInstance().RegisterRoute(method, path, std::move(cb));
        },
        [](std::string_view prefix) {  // PushRoutePrefix
            Router::GetInstance().PushRouteGroup(prefix);
        },
        [](void) {  // PopRoutePrefix
            Router::GetInstance().PopRouteGroup();
        },

        // Middleware
        [](std::string_view name, MiddlewareCallbackType cb) { // RegisterMiddleware
            HttpMiddleware::GetInstance().RegisterMiddleware(name, std::move(cb));
        },
        
        // Response handling
        [](HttpResponse* backend, HttpStatus code) {  // SetStatusFn
            backend->Status(code);
        },
        [](HttpResponse* backend, std::string key, std::string value) {  // SetHeaderFn
            backend->Set(std::move(key), std::move(value));
        },
        [](HttpResponse* backend, const char* cstr) {  // SendTextCStrFn
            backend->SendText(cstr);
        },
        SendTextRvalueFn{[](HttpResponse* backend, std::string&& text) {  // SendTextRvalueFn
            backend->SendText(std::move(text));
        }},
        [](HttpResponse* backend, const Json* json) {  // SendJsonConstRefFn
            backend->SendJson(*json);
        },
        SendJsonRvalueFn{[](HttpResponse* backend, Json&& json) {  // SendJsonRvalueFn
            backend->SendJson(std::move(json));
        }},
        [](HttpResponse* backend, const char* cstr, bool autoHandle404) {  // SendFileCStrFn
            backend->SendFile(cstr, autoHandle404);
        },
        SendFileRvalueFn{[](HttpResponse* backend, std::string&& path, bool autoHandle404) {  // SendFileRvalueFn
            backend->SendFile(std::move(path), autoHandle404);
        }},

        // Version
        HttpAPIVersion::V1
    };

    return &__GlobalHttpAPIV1;
}

} // namespace WFX::Shared