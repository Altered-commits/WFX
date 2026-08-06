// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_TYPES_HPP
#define WFX_INC_WFX_TYPES_HPP

// -----------------------------------------------------------------------
// wfx/types.hpp
// All ABI-stable types re-exported into the WFX namespace.
// Include this if you only need types without HTTP/Async/Form machinery.
// -----------------------------------------------------------------------

#include "shared/abis/types.hpp"
#include "shared/abis/constants.hpp"
#include "shared/abis/uuid.hpp"
#include "shared/abis/string_view.hpp"
#include "shared/abis/segment_variant.hpp"
#include "shared/abis/any.hpp"
#include "shared/json/json_writter.hpp"
#include "shared/json/json_object.hpp"
#include "shared/json/json_parser.hpp"

namespace WFX {

// -----------------------------------------------------------------------
// HTTP primitives
// -----------------------------------------------------------------------
using HttpStatus = Shared::HttpStatus;
using HttpMethod = Shared::HttpMethod;
using HttpVersion = Shared::HttpVersion;

// -----------------------------------------------------------------------
// Data types
// -----------------------------------------------------------------------
using StringView = Shared::StringView;
using UUID = Shared::UUID;
using UUIDString = Shared::UUIDString;
using SegmentVariant = Shared::SegmentVariant;
using Any = Shared::Any;
using JsonObject = Shared::JsonObject;

// -----------------------------------------------------------------------
// JSON serializers
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
inline Shared::JsonWriter ImJson(Http::Response& res) noexcept
{
    return Shared::JsonWriter{res};
}
inline Shared::JsonObject RmJson() noexcept
{
    return Shared::JsonObject::Init();
}

// Hint overload
inline Shared::JsonObject RmJson(std::uint32_t nodeHint, std::uint32_t kvHint, std::uint32_t strHint) noexcept
{
    return Shared::JsonObject::Init(nodeHint, kvHint, strHint);
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
inline Shared::JsonParseResult ParseJson(std::string_view body, bool view = false, std::uint32_t maxDepth = 64) noexcept
{
    return Shared::JsonParser::ParseImpl(body, view, maxDepth);
}

// -----------------------------------------------------------------------
// Segment variant tags, use with Request::GetSegment().Tag()
//
//   auto seg = req.GetSegment(0);
//   if (seg.Tag() == WFX::SegStr) { ... }
// -----------------------------------------------------------------------
inline constexpr auto SegEmpty = Shared::SEG_VARIANT_EMPTY;
inline constexpr auto SegU64 = Shared::SEG_VARIANT_U64;
inline constexpr auto SegI64 = Shared::SEG_VARIANT_I64;
inline constexpr auto SegStr = Shared::SEG_VARIANT_STR;
inline constexpr auto SegUUID = Shared::SEG_VARIANT_UUID;
inline constexpr auto SegStcStr = Shared::SEG_VARIANT_STC_STR;

// -----------------------------------------------------------------------
// Middleware flow control
//
// Return one of these from any middleware (sync or async):
//
//   return WFX::MwContinue;   // run next middleware, then the handler
//   return WFX::MwSkipNext;   // skip the next middleware, keep going
//   return WFX::MwBreak;      // stop everything, handler never runs
// -----------------------------------------------------------------------
inline constexpr auto MwContinue = Shared::MiddlewareAction::CONTINUE;
inline constexpr auto MwSkipNext = Shared::MiddlewareAction::SKIP_NEXT;
inline constexpr auto MwBreak = Shared::MiddlewareAction::BREAK;

// -----------------------------------------------------------------------
// Async result codes
//
// Returned by co_await expressions (SleepFor, SendPayload, etc.):
//
//   auto s = co_await WFX::SleepFor(500);
//   if (s == WFX::AsyncOk) { ... }
//
//   AsyncOk              : completed successfully
//   AsyncTimerFailure    : timer could not be scheduled
//   AsyncIoFailure       : I/O operation failed
//   AsyncInternalFailure : unhandled exception or engine fault
//   AsyncNone            : not yet run (internal sentinel, rarely seen by users)
// -----------------------------------------------------------------------
inline constexpr auto AsyncOk = Shared::AsyncStatus::COMPLETED;
inline constexpr auto AsyncTimerFailure = Shared::AsyncStatus::TIMER_FAILURE;
inline constexpr auto AsyncIoFailure = Shared::AsyncStatus::IO_FAILURE;
inline constexpr auto AsyncInternalFailure = Shared::AsyncStatus::INTERNAL_FAILURE;
inline constexpr auto AsyncNone = Shared::AsyncStatus::NONE;

// -----------------------------------------------------------------------
// Stream flow control
//
// Return one of these from your Stream() generator callable:
//
//   return { bytesWritten, WFX::StreamContinue };   // more chunks to come
//   return { bytesWritten, WFX::StreamDone };       // finished, keep connection alive
//   return { bytesWritten, WFX::StreamClose };      // finished, close connection
// -----------------------------------------------------------------------
inline constexpr auto StreamContinue = Shared::StreamAction::CONTINUE;
inline constexpr auto StreamDone = Shared::StreamAction::STOP_AND_ALIVE_CONN;
inline constexpr auto StreamClose = Shared::StreamAction::STOP_AND_CLOSE_CONN;

} // namespace WFX

#endif // WFX_INC_WFX_TYPES_HPP