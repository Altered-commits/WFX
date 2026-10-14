// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_COMPILER_MACROS_HPP
#define WFX_SHARED_COMPILER_MACROS_HPP

// ---------------------------------------------------------------------
// WFX_EXPORT: Force symbol to stay alive + be externally visible
// ---------------------------------------------------------------------
#if defined(__clang__)
// Clang: used + retain prevents dead-strip
#define WFX_EXPORT __attribute__((used, retain, visibility("default")))
#else
// GCC: visibility + used is enough (retain attribute may not exist)
#define WFX_EXPORT __attribute__((used, visibility("default")))
#endif

// ---------------------------------------------------------------------
// WFX_USED: Mark symbol as used to prevent dead-strip / optimize away
// ---------------------------------------------------------------------
#if defined(__clang__)
// Clang: used + retain prevents dead-strip
#define WFX_USED __attribute__((used, retain))
#else
// GCC: 'used' is enough to prevent elimination
#define WFX_USED __attribute__((used))
#endif

// ---------------------------------------------------------------------
// WFX_UNREACHABLE: Tell the compiler this code path cannot be reached
// ---------------------------------------------------------------------
#define WFX_UNREACHABLE __builtin_unreachable()

// ---------------------------------------------------------------------
// WFX_FORCE_INLINE: Demand inlining regardless of optimization level
// Note: compiler may still refuse for recursive or complex functions
// ---------------------------------------------------------------------
#define WFX_FORCE_INLINE inline __attribute__((always_inline))

// ---------------------------------------------------------------------
// WFX_NOINLINE: Prevent inlining
// ---------------------------------------------------------------------
#define WFX_NOINLINE __attribute__((noinline))

// ---------------------------------------------------------------------
// WFX_ASSUME(cond): Tell the optimizer the condition is always true
//   Unlike WFX_UNREACHABLE, this takes an expression. The optimizer
//   uses it as a constraint rather than eliminating the block entirely
// ---------------------------------------------------------------------
#if defined(__clang__)
#define WFX_ASSUME(cond) __builtin_assume(cond)
#else
// GCC has no __builtin_assume; synthesize it via unreachable
#define WFX_ASSUME(cond)                                                                                               \
    do {                                                                                                               \
        if(!(cond))                                                                                                    \
            __builtin_unreachable();                                                                                   \
    } while(0)
#endif

#endif // WFX_SHARED_COMPILER_MACROS_HPP
