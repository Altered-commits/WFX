// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

// Compiles every public header under include/ as a real translation unit, in the same
// environment a user .so build actually sees: WFX_ENGINE_BUILD is deliberately NOT defined
// for this target, see its wiring in CMakeLists.txt. Nothing under engine/, http/,
// os_specific/, etc. ever includes include/wfx/* (those exist purely for user .so builds),
// and tests/*/app/ is excluded from scripts/tidy.sh's file scan, so without a real TU
// somewhere outside tests/, neither the compiler nor clang-tidy ever actually see this code.
// This target only ever needs to compile, it is never linked into anything.
#include "wfx/all.hpp"
