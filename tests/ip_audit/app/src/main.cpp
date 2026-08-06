// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits
//
// ip_audit.py drives every request against /health. There is no per-scenario route to add:
// real-IP resolution, ConnectionLimiter, and RequestRateLimiter all run before a request ever
// reaches a handler, so the interesting surface is CoreEngine::AllowRequest, not a route body.

#include <wfx/http.hpp>

WFX_GET("/health", [](WFX::Request, WFX::Response res) { res.Status(200).SendText("ok"); })
