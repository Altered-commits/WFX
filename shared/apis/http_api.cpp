// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "http_api.hpp"

#include "http/connection/http_connection.hpp"
#include "http/request/http_request.hpp"
#include "http/response/http_response.hpp"
#include "http/routing/router.hpp"
#include "http/middleware/http_middleware.hpp"
#include "http/common/http_detector.hpp"
#include "utils/diagnostics/logger.hpp"

namespace WFX::Shared {

using namespace WFX::Http; // For 'Router', 'Middleware', ...

// '__GlobalHttpDataExt1.data' Can be set via the http api, the reason why this is safe to set even-
// -with multiple connections is our entire flow of data is single threaded and will remain that way
static HttpAPIDataExt1 __GlobalHttpDataExt1;

// vvv Helper functions vvv
static HttpRequest* ToReq(void* backend)
{
    return static_cast<HttpRequest*>(backend);
}
static const HttpRequest* ToReq(const void* backend)
{
    return static_cast<const HttpRequest*>(backend);
}
static HttpResponse* ToRes(void* backend)
{
    return static_cast<HttpResponse*>(backend);
}

// vvv Main Stuff vvv
const HTTP_API_EXT1* GetHttpAPIExt1()
{
    // clang-format off
    static HTTP_API_EXT1 __GlobalHttpAPIExt1 = {
        // vvv Routing vvv
        [](HttpMethod method, StringView path, RouteCallback cb) {  // RegisterRoute
            if(!__GlobalHttpDataExt1.router)
                Utils::GetLogger().Fatal("[HttpAPI]: Router was nullptr for 'RegisterRoute'");

            (void)__GlobalHttpDataExt1.router->RegisterRoute(
                method, std::string_view{path.Data(), path.Size()}, std::move(cb)
            );
        },
        [](HttpMethod method, StringView path, const MwCallback* mwStack, std::size_t mwStackSize, RouteCallback cb) { // RegisterRouteEx
            auto& logger = Utils::GetLogger();

            if(!__GlobalHttpDataExt1.router || !__GlobalHttpDataExt1.middleware)
                logger.Fatal("[HttpAPI]: Router or Middleware was nullptr for 'RegisterRouteEx'");

            if(mwStackSize == 0)
                logger.Fatal("[HttpAPI]: 'RegisterRouteEx' must have atleast 1 middleware, got 0");

            // Create per-route middleware vector
            std::vector<MwCallback> mwVector;
            for(std::size_t i = 0; i < mwStackSize; i++)
                mwVector.push_back(mwStack[i]);

            auto* node = __GlobalHttpDataExt1.router->RegisterRoute(
                method, std::string_view{path.Data(), path.Size()}, cb
            );

            __GlobalHttpDataExt1.middleware->RegisterPerRouteMiddleware(node, std::move(mwVector));
        },
        [](StringView prefix) {  // PushRoutePrefix
            if(!__GlobalHttpDataExt1.router)
                Utils::GetLogger().Fatal("[HttpAPI]: Router was nullptr for 'PushRoutePrefix'");

            __GlobalHttpDataExt1.router->PushRouteGroup(std::string_view{prefix.Data(), prefix.Size()});
        },
        [] {  // PopRoutePrefix
            if(!__GlobalHttpDataExt1.router)
                Utils::GetLogger().Fatal("[HttpAPI]: Router was nullptr for 'PopRoutePrefix'");

            __GlobalHttpDataExt1.router->PopRouteGroup();
        },

        // vvv Middleware vvv
        [](StringView name, MwCallback cb) { // RegisterMiddleware
            if(!__GlobalHttpDataExt1.middleware)
                Utils::GetLogger().Fatal("[HttpAPI]: Middleware was nullptr for 'RegisterMiddleware'");

            __GlobalHttpDataExt1.middleware->RegisterMiddleware(
                std::string_view{name.Data(), name.Size()}, cb
            );
        },

        // vvv Request Handling vvv
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
                Utils::GetLogger().Fatal(
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

        // vvv Response handling vvv
        [](void* backend, HttpStatus code) { // SetStatusFn
            ToRes(backend)->WriteStatus(code);
        },
        [](void* backend, StringView key, StringView value) { // SetHeaderFn
            ToRes(backend)->WriteHeader(
                std::string_view{key.Data(), key.Size()},
                std::string_view{value.Data(), value.Size()}
            );
        },
        [](void* backend, StringView data) { // WriteBodyFn
            ToRes(backend)->WriteBodyData(std::string_view{data.Data(), data.Size()});
        },
        [](void* backend, StringView path, bool autoHandle404) { // WriteFileFn
            ToRes(backend)->WriteFile(std::string_view{path.Data(), path.Size()}, autoHandle404);
        },
        [](void* backend, StreamGenerator gen, bool chunked) { // WriteStreamFn
            ToRes(backend)->WriteStream(gen, chunked);
        },
        [](void* backend, StringView path, Shared::JsonObject* ctx) { // WriteTemplateFn
            ToRes(backend)->WriteTemplate(std::string{path.Data(), path.Size()}, std::move(*ctx));
        },
        [](void* backend) { // CommitFn
            ToRes(backend)->Commit();
        },

        // vvv Endpoint API vvv
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
            auto& logger = Utils::GetLogger();
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
            if(!__GlobalHttpDataExt1.connHandler)
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

            return __GlobalHttpDataExt1.connHandler->AllocateEndpoint(host, port, cLimit, ifLimit, useTLS);
        },
        // WriteEndpointFn
        [](void* ctx, std::uint16_t endpointIndex, const std::byte* ptr, std::uint32_t size) -> EndpointStatus {
            if(!ctx) {
                Utils::GetLogger().Error("[HttpAPI]: 'WriteEndpoint' received null context");
                return EndpointStatus::INTERNAL_ERROR;
            }

            return __GlobalHttpDataExt1.connHandler->WriteEndpoint(
                reinterpret_cast<ConnectionContext*>(ctx), endpointIndex, ptr, size
            );
        },

        // vvv Data API vvv
        [](void* data) { // SetGlobalPtrData
            __GlobalHttpDataExt1.data = data;
        },
        []() { // GetGlobalPtrData
            return __GlobalHttpDataExt1.data;
        }
    };
    // clang-format on

    return &__GlobalHttpAPIExt1;
}

void InitHttpAPIExt1(HttpConnectionHandler* connHandler, Router* extRouter, HttpMiddleware* extMiddleware)
{
    __GlobalHttpDataExt1.connHandler = connHandler;
    __GlobalHttpDataExt1.router = extRouter;
    __GlobalHttpDataExt1.middleware = extMiddleware;
}

} // namespace WFX::Shared