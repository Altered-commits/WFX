// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_ALL_HPP
#define WFX_INC_WFX_ALL_HPP

// -----------------------------------------------------------------------
// wfx/all.hpp
// Includes everything WFX provides in one shot.
// Use this during prototyping or for small projects.
//
// For larger projects prefer individual headers to reduce compile times:
//   #include "wfx/http.hpp"              : Request, Response, route/middleware macros
//   #include "wfx/async.hpp"             : Coro, MwCoro, SleepFor
//   #include "wfx/form.hpp"              : Form parsing, Schema, Field
//   #include "wfx/app.hpp"               : App lifecycle (WFX_CONSTRUCTOR)
//   #include "wfx/types.hpp"             : Just types and aliases, no HTTP machinery
//   #include "wfx/telemetry.hpp"         : Logging and metrics
//   #include "wfx/memory.hpp"            : Engine-allocator wrappers (Alloc/New/Vector/String)
//   #include "wfx/utils/crypto.hpp"      : Hashing, HMAC, AEAD, KDFs, CSPRNG, asymmetric sign/verify
//   #include "wfx/utils/hash.hpp"        : Fast non-cryptographic hashing (WyHash, FNV-1a, ...)
//   #include "wfx/utils/encoding.hpp"    : Base64 / hex / URL encoding
//   #include "wfx/utils/jwk.hpp"         : JWKS -> AsymKey by kid
//   #include "wfx/utils/env.hpp"         : Typed env var getters (GetEnvBool/GetEnvInt/GetEnvString)
//   #include "wfx/endpoint/base.hpp"     : Raw outbound endpoint (Endpoint<TReq,TRes>)
//   #include "wfx/endpoint/http.hpp"     : Outbound HTTP/1.1 client (HttpEndpoint)
// -----------------------------------------------------------------------

#include "wfx/http.hpp"
#include "wfx/async.hpp"
#include "wfx/form.hpp"
#include "wfx/app.hpp"
#include "wfx/types.hpp"
#include "wfx/telemetry.hpp"
#include "wfx/memory.hpp"
#include "wfx/utils/crypto.hpp"
#include "wfx/utils/hash.hpp"
#include "wfx/utils/encoding.hpp"
#include "wfx/utils/jwk.hpp"
#include "wfx/utils/env.hpp"
#include "wfx/endpoint/http.hpp"

#endif // WFX_INC_WFX_ALL_HPP