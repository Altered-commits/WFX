// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_DETECTION_MACROS_HPP
#define WFX_SHARED_DETECTION_MACROS_HPP

// ---------------------------------------------------------------------
// WFX builds on Linux only, with GCC or Clang (both define __GNUC__)
// ---------------------------------------------------------------------
#if !defined(__linux__)
#error "WFX builds on Linux only"
#endif

#if !defined(__GNUC__)
#error "WFX builds with GCC or Clang only"
#endif

// ---------------------------------------------------------------------
// WFX_ARCH_... for detecting runtime CPU architecture
// ---------------------------------------------------------------------
#if defined(__x86_64__)
#define WFX_ARCH_X64
#elif defined(__aarch64__)
#define WFX_ARCH_ARM64
#else
#define WFX_ARCH_UNKNOWN
#endif

#endif // WFX_SHARED_DETECTION_MACROS_HPP
