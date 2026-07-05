// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_HTTP_HPP
#define WFX_INC_WFX_HTTP_HPP

// -----------------------------------------------------------------------
// wfx/http.hpp
// The primary include for building HTTP handlers.
//
// Provides:
//   WFX::Request, WFX::Response
//   WFX_GET, WFX_POST, WFX_GET_EX, WFX_POST_EX
//   WFX_MIDDLEWARE, WFX_MW_LIST
//   WFX_GROUP_START, WFX_GROUP_END
//   WFX::Coro, WFX::MwCoro                          (via wfx/async.hpp)
//   WFX::MwContinue / MwSkipNext / MwBreak          (via wfx/types.hpp)
//   WFX::AsyncOk / AsyncTimerFailure / ...          (via wfx/types.hpp)
//   WFX::StreamContinue / StreamDone / StreamClose  (via wfx/types.hpp)
//   WFX::RmJson / ImJson / JsonObject / ParseJson   (via wfx/types.hpp)
//
// Sync handler:
//   WFX_GET("/hello", [](WFX::Request req, WFX::Response res) {
//       res.SendText("Hello!");
//   });
//
// Async handler:
//   WFX_GET("/wait", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
//       auto s = co_await WFX::SleepFor(100);
//       if(s != WFX::AsyncOk) co_return;
//       res.SendText("waited");
//       co_return;
//   });
//
// -----------------------------------------------------------------------

#include "http/request.hpp"
#include "http/response.hpp"
#include "http/routes.hpp"
#include "http/middleware.hpp"
#include "http/endpoint.hpp"
#include "wfx/async.hpp"
#include "wfx/types.hpp"

namespace WFX {

// -----------------------------------------------------------------------
// Core HTTP types
// -----------------------------------------------------------------------
using Request = Http::Request;
using Response = Http::Response;

} // namespace WFX

#endif // WFX_INC_WFX_HTTP_HPP