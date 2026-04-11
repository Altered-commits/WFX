#include "http_api.hpp"

#include "http/connection/http_connection.hpp"
#include "http/request/http_request.hpp"
#include "http/response/http_response.hpp"
#include "http/routing/router.hpp"
#include "http/middleware/http_middleware.hpp"
#include "http/common/http_detector.hpp"
#include "utils/logger/logger.hpp"

namespace WFX::Shared {

using namespace WFX::Http; // For 'Router', 'Middleware'

using WFX::Utils::Logger;

// '__GlobalHttpDataV1.data' Can be set via the http api, the reason why this is safe to set even-
// -with multiple connections is our entire flow of data is single threaded and will remain that way
static HttpAPIDataV1 __GlobalHttpDataV1;

// vvv Helper functions vvv
static HttpRequest*       ToReq(void* backend) { return static_cast<HttpRequest*>(backend); }
static const HttpRequest* ToReq(const void* backend) { return static_cast<const HttpRequest*>(backend); }
static HttpResponse*      ToRes(void* backend) { return static_cast<HttpResponse*>(backend); }

// vvv Main Stuff vvv
const HTTP_API_TABLE* GetHttpAPIV1()
{
    static HTTP_API_TABLE __GlobalHttpAPIV1 = {
        // Routing
        [](HttpMethod method, StringView path, RouteCallback cb) {  // RegisterRoute
            if(!__GlobalHttpDataV1.router)
                Logger::GetInstance().Fatal("[HttpAPI]: Router was nullptr for 'RegisterRoute'");

            (void)__GlobalHttpDataV1.router->RegisterRoute(
                method, std::string_view{path.Data(), path.Size()}, std::move(cb)
            );
        },
        [](HttpMethod method, StringView path, const MwCallback* mwStack, std::size_t mwStackSize, RouteCallback cb) { // RegisterRouteEx
            auto& logger = Logger::GetInstance();

            if(!__GlobalHttpDataV1.router || !__GlobalHttpDataV1.middleware)
                logger.Fatal("[HttpAPI]: Router or Middleware was nullptr for 'RegisterRouteEx'");

            if(mwStackSize == 0)
                logger.Fatal("[HttpAPI]: 'RegisterRouteEx' must have atleast 1 middleware, got 0");

            // Create per-route middleware vector
            std::vector<MwCallback> mwVector;
            for(std::size_t i = 0; i < mwStackSize; i++)
                mwVector.push_back(mwStack[i]);

            auto* node = __GlobalHttpDataV1.router->RegisterRoute(
                method, std::string_view{path.Data(), path.Size()}, cb
            );

            __GlobalHttpDataV1.middleware->RegisterPerRouteMiddleware(node, std::move(mwVector));
        },
        [](StringView prefix) {  // PushRoutePrefix
            if(!__GlobalHttpDataV1.router)
                Logger::GetInstance().Fatal("[HttpAPI]: Router was nullptr for 'PushRoutePrefix'");

            __GlobalHttpDataV1.router->PushRouteGroup(std::string_view{prefix.Data(), prefix.Size()});
        },
        [] {  // PopRoutePrefix
            if(!__GlobalHttpDataV1.router)
                Logger::GetInstance().Fatal("[HttpAPI]: Router was nullptr for 'PopRoutePrefix'");

            __GlobalHttpDataV1.router->PopRouteGroup();
        },

        // Middleware
        [](StringView name, MwCallback cb) { // RegisterMiddleware
            if(!__GlobalHttpDataV1.middleware)
                Logger::GetInstance().Fatal("[HttpAPI]: Middleware was nullptr for 'RegisterMiddleware'");

            __GlobalHttpDataV1.middleware->RegisterMiddleware(
                std::string_view{name.Data(), name.Size()}, cb
            );
        },

        // Request Handling
        [](const void* request) { // GetMethodFn
            return ToReq(request)->method;
        },
        [](const void* request) { // GetVersionFn
            return ToReq(request)->version;
        },
        [](const void* request) { // GetPathFn
            auto path = ToReq(request)->path;
            return StringView{path.data(), static_cast<std::uint64_t>(path.size())};
        },
        [](const void* request) { // GetBodyFn
            auto body = ToReq(request)->body;
            return StringView{body.data(), static_cast<std::uint64_t>(body.size())};
        },
        [](const void* request, StringView key, StringView* outVal) { // GetHeaderFn
            if(!outVal)
                return false;

            auto* req = ToReq(request);
            auto val = req->headers.GetHeader(std::string_view{key.Data(), key.Size()});

            if(val.empty())
                return false;

            *outVal = StringView{val.data(), static_cast<std::uint64_t>(val.size())};
            return true;
        },
        [](const void* request) { // GetSegmentCountFn
            return ToReq(request)->pathSegments.size();
        },
        [](const void* request, std::uint64_t index) { // GetSegmentFn
            auto& segments = ToReq(request)->pathSegments;

            if(index >= segments.size())
                Logger::GetInstance().Fatal(
                    "[HttpAPI]: Index out of bounds for 'GetSegment'. Expected less than ",
                    segments.size(), ", got", index
                );

            return segments[index];
        },
        [](void* request, StringView key, Any value) { // SetContextFn
            auto* req = ToReq(request);
            auto k = std::string(key.Data(), key.Size());

            // If key exists, destroy old value
            auto [it, inserted] = req->context.try_emplace(k);
            if(!inserted)
                it->second.Reset();

            it->second = value;
        },
        [](const void* request, StringView key, Any* outVal) { // GetContextFn
            if(!outVal)
                return false;

            auto* req = ToReq(request);

            auto it = req->context.find(std::string(key.Data(), key.Size()));
            if(it == req->context.end())
                return false;

            *outVal = it->second;
            return true;
        },
        [](void* request, StringView key) { // EraseContextFn
            auto* req = ToReq(request);

            auto it = req->context.find(std::string(key.Data(), key.Size()));
            if(it != req->context.end()) {
                it->second.Reset();
                req->context.erase(it);
            }
        },

        // Response handling
        [](void* backend, HttpStatus code) {  // SetStatusFn
            ToRes(backend)->Status(code);
        },
        [](void* backend, StringView key, StringView value) {  // SetHeaderFn
            ToRes(backend)->Set(
                std::string(key.Data(), key.Size()), std::string(value.Data(), value.Size())
            );
        },
        [](void* backend, StringView view, bool copyBuffer) {  // SendTextFn
            auto* response = ToRes(backend);

            if(copyBuffer)
                response->SendText(std::string{view.Data(), view.Size()});
            else
                response->SendText(std::string_view{view.Data(), view.Size()});
        },
        [](void* backend, StringView view, bool autoHandle404, bool copyBuffer) {  // SendFileFn
            auto* response = ToRes(backend);

            if(copyBuffer)
                response->SendFile(std::string{view.Data(), view.Size()}, autoHandle404);
            else
                response->SendFile(std::string_view{view.Data(), view.Size()}, autoHandle404);
        },
        // Stream API
        [](void* backend, StreamGenerator generator, bool streamChunked) { // StreamFn
            ToRes(backend)->Stream(generator, streamChunked);
        },

        // Endpoint API
        [](StringView urlView, std::uint32_t cLimit, std::uint32_t ifLimit, EndpointTLSConfig tlsConfig) -> std::uint16_t {
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
            if(urlView.Empty())
                logger.Fatal("[HttpAPI]: Endpoint got empty URL");

            std::string_view protocol{}, host{}, port{}, url{urlView.Data(), urlView.Size()}, urlCpy{url};

            logger.Info("[HttpAPI]: Resolving endpoint: ", url);

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

                port = Http::PortDetector::DetectFromProtocol(protocol);
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

            bool useTLS = false;

            switch(tlsConfig) {
                // AUTO: TLS only on known secure ports
                case EndpointTLSConfig::AUTO:
                    switch(nport) {
                        case 443:   // HTTPS
                        case 465:   // SMTPS
                        case 993:   // IMAPS
                        case 995:   // POP3S
                        case 636:   // LDAPS
                        case 989:   // FTPS (data)
                        case 990:   // FTPS (control)
                        case 5671:  // AMQP over TLS
                        case 8883:  // MQTT over TLS
                            useTLS = true;
                            break;
                    }
                    break;

                // FORCE_REQUIRE: Always TLS, no matter what
                case EndpointTLSConfig::FORCE_REQUIRE:
                    useTLS = true;
                    break;
    
                // FORCE_INSECURE: Never TLS, even on 443
                case EndpointTLSConfig::FORCE_INSECURE:
                    useTLS = false;
                    break;
            }

            return __GlobalHttpDataV1.connHandler->AllocateEndpoint(host, port, cLimit, ifLimit, useTLS);
        },
        // WriteEndpointFn
        [](void* ctx, std::uint16_t endpointIndex, const std::byte* ptr, std::uint32_t size) -> EndpointStatus {
            if(!ctx) {
                Logger::GetInstance().Error("[HttpAPI]: 'WriteEndpoint' received null context");
                return EndpointStatus::INTERNAL_ERROR;
            }

            return __GlobalHttpDataV1.connHandler->WriteEndpoint(
                reinterpret_cast<ConnectionContext*>(ctx), endpointIndex, ptr, size
            );
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