#include "http_api.hpp"

#include "http/response/http_response.hpp"
#include "http/routing/router.hpp"
#include "http/middleware/http_middleware.hpp"
#include "utils/logger/logger.hpp"

namespace WFX::Shared {

using namespace WFX::Http; // For 'Router', 'Middleware'

using WFX::Utils::Logger;

// '__GlobalHttpDataV1.data' Can be set via the http api, the reason why this is safe to set even-
// with multiple connections is our entire flow of data is single threaded and will remain that way
static HttpAPIDataV1 __GlobalHttpDataV1;

const HTTP_API_TABLE* GetHttpAPIV1()
{
    static HTTP_API_TABLE __GlobalHttpAPIV1 = {
        // Routing
        [](HttpMethod method, std::string_view path, HttpCallbackType cb) {  // RegisterRoute
            if(!__GlobalHttpDataV1.router)
                Logger::GetInstance().Fatal("[HttpAPI]: Router was nullptr for 'RegisterRoute'");

            (void)__GlobalHttpDataV1.router->RegisterRoute(method, path, std::move(cb));
        },
        [](HttpMethod method, std::string_view path, HttpMiddlewareStack mwStack, HttpCallbackType cb) { // RegisterRouteEx
            if(!__GlobalHttpDataV1.router || !__GlobalHttpDataV1.middleware)
                Logger::GetInstance().Fatal("[HttpAPI]: Router or Middleware was nullptr for 'RegisterRouteEx'");

            auto* node = __GlobalHttpDataV1.router->RegisterRoute(method, path, std::move(cb));
            __GlobalHttpDataV1.middleware->RegisterPerRouteMiddleware(node, std::move(mwStack));
        },
        [](std::string_view prefix) {  // PushRoutePrefix
            if(!__GlobalHttpDataV1.router)
                Logger::GetInstance().Fatal("[HttpAPI]: Router was nullptr for 'PushRoutePrefix'");

            __GlobalHttpDataV1.router->PushRouteGroup(prefix);
        },
        [](void) {  // PopRoutePrefix
            if(!__GlobalHttpDataV1.router)
                Logger::GetInstance().Fatal("[HttpAPI]: Router was nullptr for 'PopRoutePrefix'");

            __GlobalHttpDataV1.router->PopRouteGroup();
        },

        // Middleware
        [](std::string_view name, HttpMiddlewareType cb) { // RegisterMiddleware
            if(!__GlobalHttpDataV1.middleware)
                Logger::GetInstance().Fatal("[HttpAPI]: Middleware was nullptr for 'RegisterMiddleware'");

            __GlobalHttpDataV1.middleware->RegisterMiddleware(name, std::move(cb));
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
        [](HttpResponse* backend, const char* cstr, bool autoHandle404) {  // SendFileCStrFn
            backend->SendFile(cstr, autoHandle404);
        },
        SendFileRvalueFn{[](HttpResponse* backend, std::string&& path, bool autoHandle404) {  // SendFileRvalueFn
            backend->SendFile(std::move(path), autoHandle404);
        }},
        [](HttpResponse* backend, const char* cstr, Json&& ctx) {  // SendTemplateCStrFn
            backend->SendTemplate(cstr, std::move(ctx));
        },
        SendTemplateRvalueFn{[](HttpResponse* backend, std::string&& path, Json&& ctx) {  // SendTemplateRvalueFn
            backend->SendTemplate(std::move(path), std::move(ctx));
        }},
        [](HttpResponse* backend, StreamGenerator generator, bool streamChunked) { // StreamFn
            backend->Stream(std::move(generator), streamChunked);
        },

        // Data API
        [](void* data) { // SetGlobalPtrData
            __GlobalHttpDataV1.data = data;
        },
        []() { // GetGlobalPtrData
            return __GlobalHttpDataV1.data;
        },

        // Version
        HttpAPIVersion::V1
    };

    return &__GlobalHttpAPIV1;
}

void InitHttpAPIV1(Router* extRouter, HttpMiddleware* extMiddleware)
{
    __GlobalHttpDataV1.router     = extRouter;
    __GlobalHttpDataV1.middleware = extMiddleware;
}

} // namespace WFX::Shared