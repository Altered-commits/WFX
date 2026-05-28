// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_COMPILER_MACROS_HPP
#define WFX_SHARED_COMPILER_MACROS_HPP

// ---------------------------------------------------------------------
// WFX_EXPORT: Force symbol to stay alive + be externally visible
// ---------------------------------------------------------------------
#if defined(_MSC_VER) || defined(__ICL) || defined(__INTEL_COMPILER)
// Windows/MSVC/ICC: Must use dllexport for exported visible symbols
#define WFX_EXPORT __declspec(dllexport)
#elif defined(__clang__)
// Clang: used + retain prevents dead-strip on all platforms
#define WFX_EXPORT __attribute__((used, retain, visibility("default")))
#elif defined(__GNUC__) || defined(__MINGW32__)
// GCC: visibility + used is enough (retain attribute may not exist)
#define WFX_EXPORT __attribute__((used, visibility("default")))
#else
// Unknown compiler
#define WFX_EXPORT
#endif

// ---------------------------------------------------------------------
// WFX_USED: Mark symbol as used to prevent dead-strip / optimize away
// ---------------------------------------------------------------------
#if defined(_MSC_VER) || defined(__ICL) || defined(__INTEL_COMPILER)
// Windows/MSVC/ICC: no direct equivalent, just use __declspec(selectany) for globals
#define WFX_USED __declspec(selectany)
#elif defined(__clang__)
// Clang: used + retain prevents dead-strip on all platforms
#define WFX_USED __attribute__((used, retain))
#elif defined(__GNUC__) || defined(__MINGW32__)
// GCC: 'used' is enough to prevent elimination
#define WFX_USED __attribute__((used))
#else
// Unknown compiler
#define WFX_USED
#endif

// ---------------------------------------------------------------------
// WFX_UNREACHABLE: Tell the compiler this code path cannot be reached
// ---------------------------------------------------------------------
#if defined(_MSC_VER)
// MSVC & Intel classic on Windows
#define WFX_UNREACHABLE __assume(0)
#elif defined(__clang__)
// Clang has builtin_unreachable and also supports __builtin_trap as fallback
#define WFX_UNREACHABLE __builtin_unreachable()
#elif defined(__GNUC__) || defined(__MINGW32__) || defined(__INTEL_COMPILER) || defined(__ICL)
// GCC-family compilers + Intel Linux compilers
#define WFX_UNREACHABLE __builtin_unreachable()
#else
// Portable safe fallback
#define WFX_UNREACHABLE                                                                                                \
    do {                                                                                                               \
    } while(0)
#endif

// ---------------------------------------------------------------------
// WFX_FORCE_INLINE: Demand inlining regardless of optimization level
// Note: compiler may still refuse for recursive or complex functions
// ---------------------------------------------------------------------
#if defined(_MSC_VER) || defined(__ICL) || defined(__INTEL_COMPILER)
#define WFX_FORCE_INLINE __forceinline
#elif defined(__clang__) || defined(__GNUC__) || defined(__MINGW32__)
#define WFX_FORCE_INLINE inline __attribute__((always_inline))
#else
#define WFX_FORCE_INLINE inline
#endif

// ---------------------------------------------------------------------
// WFX_NOINLINE: Prevent inlining
// ---------------------------------------------------------------------
#if defined(_MSC_VER) || defined(__ICL) || defined(__INTEL_COMPILER)
#define WFX_NOINLINE __declspec(noinline)
#elif defined(__clang__) || defined(__GNUC__) || defined(__MINGW32__)
#define WFX_NOINLINE __attribute__((noinline))
#else
#define WFX_NOINLINE
#endif

// ---------------------------------------------------------------------
// WFX_ASSUME(cond): Tell the optimizer the condition is always true
//   Unlike WFX_UNREACHABLE, this takes an expression. The optimizer-
//   -uses it as a constraint rather than eliminating the block entirely
// ---------------------------------------------------------------------
#if defined(_MSC_VER) || defined(__ICL) || defined(__INTEL_COMPILER)
#define WFX_ASSUME(cond) __assume(cond)
#elif defined(__clang__)
#define WFX_ASSUME(cond) __builtin_assume(cond)
#elif defined(__GNUC__) || defined(__MINGW32__)
// GCC has no __builtin_assume; synthesize it via unreachable
#define WFX_ASSUME(cond)                                                                                               \
    do {                                                                                                               \
        if(!(cond))                                                                                                    \
            __builtin_unreachable();                                                                                   \
    } while(0)
#else
#define WFX_ASSUME(cond) (void)(cond)
#endif

#endif // WFX_SHARED_COMPILER_MACROS_HPP