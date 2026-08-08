// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_HTTP_ROUTE_MACROS_HPP
#define WFX_INC_HTTP_ROUTE_MACROS_HPP

#include "helper.hpp"
#include "response.hpp"
#include "request.hpp"
#include "core/core.hpp"
#include "core/deferred_init_vector.hpp"

// Glue suffix to names
#define WFX_ROUTE_CLASS(prefix, id) WFX_CONCAT(WFXRoute_, WFX_CONCAT(prefix, id))
#define WFX_ROUTE_INSTANCE(id) WFX_CONCAT(WFXRouteInst_, id)

#define WFX_INTERNAL_ROUTE_REGISTER_IMPL(method, path, uniq, ...)                                                      \
    namespace {                                                                                                        \
    struct WFX_ROUTE_CLASS(method, uniq) {                                                                             \
        WFX_ROUTE_CLASS(method, uniq)()                                                                                \
        {                                                                                                              \
            WFX::Core::GlobalWFXDeferred.emplace_back([] {                                                             \
                WFX::Core::HttpApiExt1()->registerRoute(WFX::Shared::HttpMethod::method,                               \
                                                        WFX::Shared::StringView::FromCString(path),                    \
                                                        WFX::Http::MakeRouteCallback(__VA_ARGS__));                    \
            });                                                                                                        \
        }                                                                                                              \
    } WFX_ROUTE_INSTANCE(uniq);                                                                                        \
    }

#define WFX_INTERNAL_ROUTE_REGISTER_EX_IMPL(method, path, mw, uniq, ...)                                               \
    namespace {                                                                                                        \
    struct WFX_ROUTE_CLASS(method, uniq) {                                                                             \
        WFX_ROUTE_CLASS(method, uniq)()                                                                                \
        {                                                                                                              \
            auto mwArr = mw;                                                                                           \
            WFX::Core::GlobalWFXDeferred.emplace_back([mwArr]() mutable {                                              \
                WFX::Core::HttpApiExt1()->registerRouteEx(WFX::Shared::HttpMethod::method,                             \
                                                          WFX::Shared::StringView::FromCString(path), mwArr.Data(),    \
                                                          mwArr.Count(), WFX::Http::MakeRouteCallback(__VA_ARGS__));   \
            });                                                                                                        \
        }                                                                                                              \
    } WFX_ROUTE_INSTANCE(uniq);                                                                                        \
    }

#define WFX_INTERNAL_ROUTE_REGISTER(method, path, ...)                                                                 \
    WFX_INTERNAL_ROUTE_REGISTER_IMPL(method, path, __COUNTER__, __VA_ARGS__)

#define WFX_INTERNAL_ROUTE_REGISTER_EX(method, path, mw, ...)                                                          \
    WFX_INTERNAL_ROUTE_REGISTER_EX_IMPL(method, path, mw, __COUNTER__, __VA_ARGS__)

// vvv HTTP MACROS vvv
// Simple routes
#define WFX_GET(path, ...) WFX_INTERNAL_ROUTE_REGISTER(GET, path, __VA_ARGS__)
#define WFX_POST(path, ...) WFX_INTERNAL_ROUTE_REGISTER(POST, path, __VA_ARGS__)
#define WFX_PUT(path, ...) WFX_INTERNAL_ROUTE_REGISTER(PUT, path, __VA_ARGS__)
#define WFX_PATCH(path, ...) WFX_INTERNAL_ROUTE_REGISTER(PATCH, path, __VA_ARGS__)
#define WFX_DELETE(path, ...) WFX_INTERNAL_ROUTE_REGISTER(DELETE, path, __VA_ARGS__)
#define WFX_HEAD(path, ...) WFX_INTERNAL_ROUTE_REGISTER(HEAD, path, __VA_ARGS__)
#define WFX_OPTIONS(path, ...) WFX_INTERNAL_ROUTE_REGISTER(OPTIONS, path, __VA_ARGS__)

// Routes with per-route middleware
// Usage: WFX_GET_EX("/path", WFX::Http::MakeMiddleware(mw1, mw2), handler)
#define WFX_GET_EX(path, mw, ...) WFX_INTERNAL_ROUTE_REGISTER_EX(GET, path, mw, __VA_ARGS__)
#define WFX_POST_EX(path, mw, ...) WFX_INTERNAL_ROUTE_REGISTER_EX(POST, path, mw, __VA_ARGS__)
#define WFX_PUT_EX(path, mw, ...) WFX_INTERNAL_ROUTE_REGISTER_EX(PUT, path, mw, __VA_ARGS__)
#define WFX_PATCH_EX(path, mw, ...) WFX_INTERNAL_ROUTE_REGISTER_EX(PATCH, path, mw, __VA_ARGS__)
#define WFX_DELETE_EX(path, mw, ...) WFX_INTERNAL_ROUTE_REGISTER_EX(DELETE, path, mw, __VA_ARGS__)
#define WFX_HEAD_EX(path, mw, ...) WFX_INTERNAL_ROUTE_REGISTER_EX(HEAD, path, mw, __VA_ARGS__)
#define WFX_OPTIONS_EX(path, mw, ...) WFX_INTERNAL_ROUTE_REGISTER_EX(OPTIONS, path, mw, __VA_ARGS__)

// vvv ROUTE GROUPING vvv
#define WFX_GROUP_START_IMPL(path, id)                                                                                 \
    namespace {                                                                                                        \
    struct WFX_CONCAT(WFXGroupStart_, id) {                                                                            \
        WFX_CONCAT(WFXGroupStart_, id)()                                                                               \
        {                                                                                                              \
            WFX::Core::GlobalWFXDeferred.emplace_back(                                                                 \
                [] { WFX::Core::HttpApiExt1()->pushRoutePrefix(WFX::Shared::StringView::FromCString(path)); });        \
        }                                                                                                              \
    } WFX_CONCAT(WFXGroupStartInst_, id);                                                                              \
    }

#define WFX_GROUP_END_IMPL(id)                                                                                         \
    namespace {                                                                                                        \
    struct WFX_CONCAT(WFXGroupEnd_, id) {                                                                              \
        WFX_CONCAT(WFXGroupEnd_, id)()                                                                                 \
        {                                                                                                              \
            WFX::Core::GlobalWFXDeferred.emplace_back([] { WFX::Core::HttpApiExt1()->popRoutePrefix(); });             \
        }                                                                                                              \
    } WFX_CONCAT(WFXGroupEndInst_, id);                                                                                \
    }

#define WFX_GROUP_START(path) WFX_GROUP_START_IMPL(path, __COUNTER__)
#define WFX_GROUP_END() WFX_GROUP_END_IMPL(__COUNTER__)

#endif // WFX_INC_HTTP_ROUTE_MACROS_HPP