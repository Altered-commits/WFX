#ifndef WFX_INC_WFX_ALL_HPP
#define WFX_INC_WFX_ALL_HPP

// -----------------------------------------------------------------------
// wfx/all.hpp
// Includes everything WFX provides in one shot.
// Use this during prototyping or for small projects.
//
// For larger projects prefer individual headers to reduce compile times:
//   #include "wfx/http.hpp"   — Request, Response, route/middleware macros,
//                               Endpoint, all status aliases
//   #include "wfx/async.hpp"  — Coro, MwCoro, SleepFor
//   #include "wfx/form.hpp"   — Form parsing, Schema, Field
//   #include "wfx/app.hpp"    — App lifecycle (WFX_CONSTRUCTOR)
//   #include "wfx/types.hpp"  — Just types and aliases, no HTTP machinery
// -----------------------------------------------------------------------

#include "wfx/http.hpp"
#include "wfx/async.hpp"
#include "wfx/form.hpp"
#include "wfx/app.hpp"
#include "wfx/types.hpp"

#endif // WFX_INC_WFX_ALL_HPP