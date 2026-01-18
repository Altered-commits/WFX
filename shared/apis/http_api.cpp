#include "http_api.hpp"

#include "http/connection/http_connection.hpp"
#include "http/response/http_response.hpp"
#include "http/routing/router.hpp"
#include "http/middleware/http_middleware.hpp"
#include "http/common/http_detector.hpp"
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

        // Stream API
        [](HttpResponse* backend, StreamGenerator generator, bool streamChunked) { // StreamFn
            backend->Stream(std::move(generator), streamChunked);
        },

        // Endpoint API
        [](std::string_view url) -> std::uint32_t {
            /*
             * NOTE: 'url' allowed only till port number (route and optional parameters are not allowed)
             * Example:
             *      https://example.com    is allowed
             *      example.com:443        is allowed
             * 
             *      https://api.xyz.com/v1 is not allowed (/v1 not allowed)
             *      example.com            is not allowed (no protocol defined)
             */
            auto& logger = Logger::GetInstance();
            if(url.empty())
                logger.Fatal("[HttpAPI]: Endpoint got empty URL");

            std::string_view protocol{}, host{}, port{}, urlCpy{url};

            // Detect protocol
            auto pos = url.find("://");
            if(pos != std::string_view::npos) {
                protocol = url.substr(0, pos);
                url = url.substr(pos + 3);
                if(protocol.empty())
                    logger.Fatal("[HttpAPI]: Endpoint got empty protocol");
            }

            // Reject forbidden chars
            for(char c : url) {
                if(c == '/' || c == '?' || c == '#' || c == '@')
                    logger.Fatal(
                        "[HttpAPI]: Endpoint forbids usage of (/, ?, #, @) in url"
                    );
            }

            // Host + port
            // IPv6
            if(!url.empty() && url.front() == '[') {
                auto end = url.find(']');
                if(end == std::string_view::npos)
                    logger.Fatal(
                        "[HttpAPI]: Endpoint got unclosed IPv6 literal"
                    );

                host = url.substr(1, end - 1);
                url  = url.substr(end + 1);

                if(!url.empty()) {
                    if(url.front() != ':')
                        logger.Fatal("[HttpAPI]: Unexpected characters in endpoint after IPv6 host");
                    port = url.substr(1);
                }
            }
            // IPv4 / hostname
            else {
                auto colon = url.rfind(':');
                if(colon != std::string_view::npos) {
                    host = url.substr(0, colon);
                    port = url.substr(colon + 1);
                }
                else
                    host = url;
            }

            if(host.empty())
                logger.Fatal("[HttpAPI]: Missing host in endpoint");

            // Port rules
            std::uint32_t nport = 0;
            if(port.empty()) {
                if(protocol.empty())
                    logger.Fatal("[HttpAPI]: Missing port and protocol in endpoint");

                port = WFX::Http::PortDetector::DetectFromProtocol(protocol);
                if(port.empty())
                    logger.Fatal(
                        "[HttpAPI]: Endpoint cannot infer port for protocol '", protocol,
                        "'. Write your own port explicitly "
                        "(e.g. protocol://host:PORT or host:PORT)."
                    );

                // Our ports are perfect so directly go to resolving address
                goto __DirectResolve;
            }

            // Validate port digits
            for(char c : port) {
                if(c < '0' || c > '9')
                    logger.Fatal("[HttpAPI]: Invalid port in endpoint: '", port, '\'');

                nport = nport * 10 + (c - '0');
                if(nport > 65535)
                    logger.Fatal(
                        "[HttpAPI]: Endpoint received invalid port '", nport,
                        "'. Port must be in the range [1, 65535]"
                    );
            }
            if(nport == 0)
                logger.Fatal("[HttpAPI]: Endpoint received port 0, invalid port");

        __DirectResolve:
            // Sanity checks
            if(!__GlobalHttpDataV1.connHandler)
                logger.Fatal("[HttpAPI]: Connection handler was nullptr for endpoint");

            // return __GlobalHttpDataV1.connHandler->InitializeEndpoint(host.data(), port.data());
            return 0;
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

void InitHttpAPIV1(HttpConnectionHandler* connHandler, Router* extRouter, HttpMiddleware* extMiddleware)
{
    __GlobalHttpDataV1.connHandler = connHandler;
    __GlobalHttpDataV1.router      = extRouter;
    __GlobalHttpDataV1.middleware  = extMiddleware;
}

} // namespace WFX::Shared