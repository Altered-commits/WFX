// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_DETECTION_MACROS_HPP
#define WFX_SHARED_DETECTION_MACROS_HPP

// ---------------------------------------------------------------------
// WFX_PLATFORM_... for detecting runtime operating system
// ---------------------------------------------------------------------
#if defined(_WIN32) || defined(_WIN64)
#define WFX_PLATFORM_WINDOWS
#elif defined(__APPLE__)
#define WFX_PLATFORM_MACOS
#define WFX_PLATFORM_POSIX
#elif defined(__linux__)
#define WFX_PLATFORM_LINUX
#define WFX_PLATFORM_POSIX
#else
#error "Unsupported platform"
#endif

// ---------------------------------------------------------------------
// WFX_ARCH_... for detecting runtime CPU architecture
// ---------------------------------------------------------------------
#if defined(__x86_64__) || defined(_M_X64)
#define WFX_ARCH_X64
#elif defined(__aarch64__) || defined(_M_ARM64)
#define WFX_ARCH_ARM64
#else
#define WFX_ARCH_UNKNOWN
#endif

// ---------------------------------------------------------------------
// WFX_COMPILER_... for detecting compiler used
// ---------------------------------------------------------------------
#if defined(_MSC_VER) && !defined(__clang__)
#define WFX_COMPILER_MSVC
#elif defined(__clang__)
#define WFX_COMPILER_CLANG
#elif defined(__INTEL_COMPILER) || defined(__ICC)
#define WFX_COMPILER_INTEL
#elif defined(__GNUC__)
#define WFX_COMPILER_GCC
#else
#error "Unsupported compiler"
#endif

#endif // WFX_SHARED_DETECTION_MACROS_HPP