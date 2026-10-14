// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_TYPES_HPP
#define WFX_INC_WFX_TYPES_HPP

// -----------------------------------------------------------------------
// wfx/types.hpp
// All ABI-stable types re-exported into the WFX namespace, with the
// constants for their values. Types and constants only: the functions that
// work on them live with what they do (JSON in wfx/json.hpp, HTTP helpers
// in wfx/http.hpp).
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
using JsonWriter = Shared::JsonWriter;
using JsonParseResult = Shared::JsonParseResult;

// -----------------------------------------------------------------------
// Segment variant tags, use with Request::GetSegment().Tag()
//
//   auto seg = req.GetSegment(0);
//   if (seg.Tag() == WFX::SegStr) { ... }
// -----------------------------------------------------------------------
// NOLINTBEGIN(readability-identifier-naming)
inline constexpr auto SegEmpty = Shared::SEG_VARIANT_EMPTY;
inline constexpr auto SegU64 = Shared::SEG_VARIANT_U64;
inline constexpr auto SegI64 = Shared::SEG_VARIANT_I64;
inline constexpr auto SegStr = Shared::SEG_VARIANT_STR;
inline constexpr auto SegUUID = Shared::SEG_VARIANT_UUID;
inline constexpr auto SegStcStr = Shared::SEG_VARIANT_STC_STR;

// -----------------------------------------------------------------------
// Middleware flow control
//
// A sync middleware returns WFX::MiddlewareAction (an async one returns
// WFX::MwCoro, see wfx/async.hpp), and its value is one of these:
//
//   return WFX::MwContinue;   // run next middleware, then the handler
//   return WFX::MwSkipNext;   // skip the next middleware, keep going
//   return WFX::MwBreak;      // stop everything, handler never runs
// -----------------------------------------------------------------------
using MiddlewareAction = Shared::MiddlewareAction;

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
using AsyncStatus = Shared::AsyncStatus;

inline constexpr auto AsyncOk = Shared::AsyncStatus::COMPLETED;
inline constexpr auto AsyncTimerFailure = Shared::AsyncStatus::TIMER_FAILURE;
inline constexpr auto AsyncIoFailure = Shared::AsyncStatus::IO_FAILURE;
inline constexpr auto AsyncInternalFailure = Shared::AsyncStatus::INTERNAL_FAILURE;
inline constexpr auto AsyncNone = Shared::AsyncStatus::NONE;

// -----------------------------------------------------------------------
// Stream flow control
//
// Your Stream() generator takes a WFX::StreamBuffer to fill and returns a
// WFX::StreamResult, whose action is one of these:
//
//   return { bytesWritten, WFX::StreamContinue };   // more chunks to come
//   return { bytesWritten, WFX::StreamDone };       // finished, connection kept as the request asked
//   return { bytesWritten, WFX::StreamClose };      // finished, close connection
// -----------------------------------------------------------------------
using StreamAction = Shared::StreamAction;
using StreamResult = Shared::StreamResult;
using StreamBuffer = Shared::StreamBuffer;

inline constexpr auto StreamContinue = Shared::StreamAction::CONTINUE;
inline constexpr auto StreamDone = Shared::StreamAction::STOP_AND_ALIVE_CONN;
inline constexpr auto StreamClose = Shared::StreamAction::STOP_AND_CLOSE_CONN;
// NOLINTEND(readability-identifier-naming)

} // namespace WFX

#endif // WFX_INC_WFX_TYPES_HPP