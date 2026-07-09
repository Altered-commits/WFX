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
static EndpointAPIDataExt1 __GlobalEndpointDataExt1;

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

// vvv Http API vvv
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

void InitHttpAPIExt1(Router* extRouter, HttpMiddleware* extMiddleware)
{
    __GlobalHttpDataExt1.router = extRouter;
    __GlobalHttpDataExt1.middleware = extMiddleware;
}

// vvv Endpoint API vvv
const ENDPOINT_API_EXT1* GetEndpointAPIExt1()
{
    // clang-format off
    static ENDPOINT_API_EXT1 __GlobalHttpAPIExt1 = {
        [](const char* host, EndpointDesc desc, EndpointConfig config) -> std::uint16_t {
            return __GlobalEndpointDataExt1.connHandler->AllocateEndpoint(host, desc, config);
        },
        [](void* clientCtx, std::uint16_t endpointIdx, const void* req, AsyncData asyncData) -> EndpointStatus {
            auto* ctx = static_cast<ClientCtx*>(clientCtx);
            return __GlobalEndpointDataExt1.connHandler->SendPayload(ctx, endpointIdx, req, asyncData);
        },
        [](void* endpointCtx, const void* data, std::uint32_t size, AsyncData asyncData) -> void {
            auto* ctx = static_cast<EndpointCtx*>(endpointCtx);
            __GlobalEndpointDataExt1.connHandler->SlotSend(ctx, data, size, asyncData);
        },
        [](void* endpointCtx, AsyncData asyncData) -> void {
            auto* ctx = static_cast<EndpointCtx*>(endpointCtx);
            __GlobalEndpointDataExt1.connHandler->SlotReceive(ctx, asyncData);
        },
        [](void* endpointCtx) -> StringView {
            auto* ctx = static_cast<EndpointCtx*>(endpointCtx);
            return __GlobalEndpointDataExt1.connHandler->NegotiatedProtocol(ctx);
        }
    };
    // clang-format on

    return &__GlobalHttpAPIExt1;
}

void InitEndpointAPIExt1(Http::HttpConnectionHandler* connHandler)
{
    __GlobalEndpointDataExt1.connHandler = connHandler;
}

} // namespace WFX::Shared