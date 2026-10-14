// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_JSON_HPP
#define WFX_INC_WFX_JSON_HPP

// -----------------------------------------------------------------------
// wfx/json.hpp
// Building and parsing JSON. The types themselves (JsonObject, JsonWriter,
// JsonParseResult) come from wfx/types.hpp.
//
// Two complementary serializers, pick based on your use case:
//
// WFX::ImJson  : Immediate-mode Json (16 bytes, zero heap)
//                Streams JSON directly into the response buffer as each
//                Write/Obj/Arr call is made. No allocation, no DOM.
//                Best for large or repetitive payloads where structure
//                is known upfront and throughput is the priority.
//
//   WFX_GET("/json", [](WFX::Request req, WFX::Response res) {
//       auto w = WFX::ImJson(res);
//       w.Write("key", "value");
//       w.Arr("items");
//           w.Obj(); w.Write("id", 1); w.End();
//       w.End();
//   })
//
// WFX::RmJson  : Retained-mode Json, DOM object (8 bytes, heap-backed)
//                Builds a full JSON object in memory first, then
//                serializes it in one shot via Write(). Supports nested
//                access, array push, and merge.
//                Best for dynamic payloads where structure is built
//                conditionally or incrementally.
//
//   WFX_GET("/json", [](WFX::Request req, WFX::Response res) {
//       auto o = WFX::RmJson();
//       o["key"] = "value";
//       o["meta"]["version"] = 2u;
//       o.Write(res);
//   })
// -----------------------------------------------------------------------

#include "wfx/types.hpp"

#include <cstdint>
#include <string_view>

namespace WFX {

inline JsonWriter ImJson(Http::Response& res) noexcept
{
    return JsonWriter{res};
}

inline JsonObject RmJson() noexcept
{
    return JsonObject::Init();
}

// Hint overload
inline JsonObject RmJson(std::uint32_t nodeHint, std::uint32_t kvHint, std::uint32_t strHint) noexcept
{
    return JsonObject::Init(nodeHint, kvHint, strHint);
}

// -----------------------------------------------------------------------
// Parses a JSON body into a JsonObject.
//
// view : Controls string storage strategy.
//        When false, all strings are copied into internal storage and
//        remain valid after the input body is destroyed. When true,
//        strings reference the input body directly, so the body must
//        remain alive.
//
// maxDepth : Maximum allowed nesting depth
// -----------------------------------------------------------------------
inline JsonParseResult ParseJson(std::string_view body, bool view = false, std::uint32_t maxDepth = 64) noexcept
{
    return Shared::JsonParser::ParseImpl(body, view, maxDepth);
}

} // namespace WFX

#endif // WFX_INC_WFX_JSON_HPP
