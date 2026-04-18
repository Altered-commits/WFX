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
#include "json/json_writter.hpp"
#include "json/json_object.hpp"

namespace WFX {

// -----------------------------------------------------------------------
// HTTP primitives
// -----------------------------------------------------------------------
using HttpStatus  = Shared::HttpStatus;
using HttpMethod  = Shared::HttpMethod;
using HttpVersion = Shared::HttpVersion;

// -----------------------------------------------------------------------
// Async / middleware underlying types (rarely used directly)
// -----------------------------------------------------------------------
using AsyncStatus      = Shared::AsyncStatus;
using MiddlewareAction = Shared::MiddlewareAction;

// -----------------------------------------------------------------------
// Data types
// -----------------------------------------------------------------------
using StringView     = Shared::StringView;
using UUID           = Shared::UUID;
using UUIDString     = Shared::UUIDString;
using SegmentVariant = Shared::SegmentVariant;
using Any            = Shared::Any;

// -----------------------------------------------------------------------
// JSON serializers
//
// Two complementary serializers, pick based on your use case:
//
// WFX::ImJson  : Immediate-mode Json (16 bytes, zero heap)
//                Streams JSON directly into the response buffer as each-
//                -Write/Obj/Arr call is made. No allocation, no DOM.
//                Best for large or repetitive payloads where structure-
//                -is known upfront and throughput is the priority.
//
//   WFX_GET("/json", [](WFX::Request req, WFX::Response res) {
//       auto j = WFX::ImJson(res);
//       j.Write("key", "value");
//       j.Arr("items");
//           j.Obj(); j.Write("id", 1); j.End();
//       j.End();
//   })
//
// WFX::RmJson  : Retained-mode Json, DOM object (8 bytes, heap-backed)
//                Builds a full JSON object in memory first, then-
//                -serializes it in one shot via Write(). Supports nested-
//                -access, array push, and merge.
//                Best for dynamic payloads where structure is built-
//                -conditionally or incrementally.
//
//   WFX_GET("/json", [](WFX::Request req, WFX::Response res) {
//       auto o = WFX::RmJson();
//       o["key"] = "value";
//       o["meta"]["version"] = 2u;
//       o.Write(res);
//   })
// -----------------------------------------------------------------------
inline Json::JsonWriter ImJson(Http::Response& res) noexcept { return Json::JsonWriter{res}; }
inline Json::JsonObject RmJson()                    noexcept { return Json::JsonObject::Init(); }

// -----------------------------------------------------------------------
// Segment variant tags, use with Request::GetSegment().Tag()
//
//   auto seg = req.GetSegment(0);
//   if (seg.Tag() == WFX::SegStr) { ... }
// -----------------------------------------------------------------------
inline constexpr auto SegEmpty  = Shared::SEG_VARIANT_EMPTY;
inline constexpr auto SegU64    = Shared::SEG_VARIANT_U64;
inline constexpr auto SegI64    = Shared::SEG_VARIANT_I64;
inline constexpr auto SegStr    = Shared::SEG_VARIANT_STR;
inline constexpr auto SegUUID   = Shared::SEG_VARIANT_UUID;
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
inline constexpr auto MwBreak    = Shared::MiddlewareAction::BREAK;

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
inline constexpr auto AsyncOk              = Shared::AsyncStatus::COMPLETED;
inline constexpr auto AsyncTimerFailure    = Shared::AsyncStatus::TIMER_FAILURE;
inline constexpr auto AsyncIoFailure       = Shared::AsyncStatus::IO_FAILURE;
inline constexpr auto AsyncInternalFailure = Shared::AsyncStatus::INTERNAL_FAILURE;
inline constexpr auto AsyncNone            = Shared::AsyncStatus::NONE;

// -----------------------------------------------------------------------
// Outbound endpoint result codes
//
// Returned by co_await Endpoint::SendPayload(...):
//
//   auto s = co_await myEndpoint.SendPayload(data);
//   if (s == WFX::EpOk) { ... }
//
//   EpOk                  : dispatched successfully
//   EpPending             : in progress (engine internal, rarely seen by users)
//   EpBufferError         : buffer could not be initialised
//   EpInsufficientBuffer  : buffer too small for this payload
//   EpInvalidKey          : bad Endpoint object / index out of bounds
//   EpPoolExhausted       : no free slots, raise pool limit in config
//   EpSocketFailure       : socket could not be created or configured
//   EpConnectFailure      : TCP connection refused or timed out
//   EpSslFailure          : TLS handshake or certificate error
//   EpInternalError       : unclassified engine fault
// -----------------------------------------------------------------------
inline constexpr auto EpOk                = Shared::EndpointStatus::SUCCESS;
inline constexpr auto EpPending           = Shared::EndpointStatus::PENDING;
inline constexpr auto EpBufferError       = Shared::EndpointStatus::BUFFER_ERROR;
inline constexpr auto EpInsufficientBuffer = Shared::EndpointStatus::INSUFFICIENT_BUFFER;
inline constexpr auto EpInvalidKey        = Shared::EndpointStatus::INVALID_KEY;
inline constexpr auto EpPoolExhausted     = Shared::EndpointStatus::POOL_EXHAUSTED;
inline constexpr auto EpSocketFailure     = Shared::EndpointStatus::SOCKET_FAILURE;
inline constexpr auto EpConnectFailure    = Shared::EndpointStatus::CONNECT_FAILURE;
inline constexpr auto EpSslFailure        = Shared::EndpointStatus::SSL_FAILURE;
inline constexpr auto EpInternalError     = Shared::EndpointStatus::INTERNAL_ERROR;

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
inline constexpr auto StreamDone     = Shared::StreamAction::STOP_AND_ALIVE_CONN;
inline constexpr auto StreamClose    = Shared::StreamAction::STOP_AND_CLOSE_CONN;

} // namespace WFX

#endif // WFX_INC_WFX_TYPES_HPP