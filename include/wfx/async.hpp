#ifndef WFX_INC_WFX_ASYNC_HPP
#define WFX_INC_WFX_ASYNC_HPP

// -----------------------------------------------------------------------
// wfx/async.hpp
// Everything needed for async route handlers and middleware.
//
// Async route:
//   WFX_GET("/path", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
//       auto s = co_await WFX::SleepFor(500);
//       if(s != WFX::AsyncOk) {
//           res.Status(WFX::HttpStatus::INTERNAL_SERVER_ERROR)
//              .SendText("timer failed");
//           co_return;
//       }
//
//       res.SendText("done");
//       co_return;
//   });
//
// Async middleware:
//   WFX_MIDDLEWARE("auth", [](WFX::Request req, WFX::Response res) -> WFX::MwCoro {
//       if(!HasValidToken(req))
//           co_return WFX::MwBreak;
//
//       co_return WFX::MwContinue;
//   });
// -----------------------------------------------------------------------

#include "async/task.hpp"
#include "async/promise.hpp"
#include "async/awaitable.hpp"
#include "async/builtins.hpp"
#include "wfx/types.hpp"

namespace WFX {

// -----------------------------------------------------------------------
// Coroutine return types, put these in your handler's return type:
//   -> WFX::Coro     for route handlers
//   -> WFX::MwCoro   for middleware
// -----------------------------------------------------------------------
using Coro = WFX::Async::Task<void>;
using MwCoro = WFX::Async::Task<Shared::MiddlewareAction>;

// -----------------------------------------------------------------------
// SleepFor(ms) — suspend for `ms` milliseconds, then resume.
//
//   auto s = co_await WFX::SleepFor(500);
//   if (s != WFX::AsyncOk) { /* WFX::AsyncTimerFailure */ }
// -----------------------------------------------------------------------
using WFX::Async::SleepFor;

} // namespace WFX

#endif // WFX_INC_WFX_ASYNC_HPP