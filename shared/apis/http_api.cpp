#include "http_api.hpp"

#include "http/response/http_response.hpp"
#include "http/routing/router.hpp"

namespace WFX::Shared {

const HTTP_API_TABLE* GetHttpAPIV1()
{
    static HTTP_API_TABLE __GlobalHttpAPIV1 = {
        // Routing
        [](HttpMethod method, std::string_view path, HttpCallbackType cb) {  // RegisterRoute
            WFX::Http::Router::GetInstance().RegisterRoute(method, path, std::move(cb));
        },
        [](std::string_view prefix) {  // PushRoutePrefix
            WFX::Http::Router::GetInstance().PushRouteGroup(prefix);
        },
        [](void) {  // PopRoutePrefix
            WFX::Http::Router::GetInstance().PopRouteGroup();
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
        [](HttpResponse* backend, std::string_view vstr) {  // SendTextViewFn
            backend->SendText(vstr);
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
        [](HttpResponse* backend, const char* cstr) {  // SendFileCStrFn
            backend->SendFile(cstr);
        },
        [](HttpResponse* backend, std::string_view view) {  // SendFileViewFn
            backend->SendFile(view);
        },
        SendFileRvalueFn{[](HttpResponse* backend, std::string&& path) {  // SendFileRvalueFn
            backend->SendFile(std::move(path));
        }},
        [](const HttpResponse* backend) {  // IsFileOperationFn
            return backend->IsFileOperation();
        },

        // Version
        HttpAPIVersion::V1
    };

    return &__GlobalHttpAPIV1;
}

} // namespace WFX::Shared