// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_MEMORY_HPP
#define WFX_INC_WFX_MEMORY_HPP

// -----------------------------------------------------------------------
// wfx/memory.hpp
// User-facing wrappers over the engine's allocator. Implementation lives in-
// -shared/utils/memory.hpp (shared between engine and user code, dual-mode-
// -via WFX_ENGINE_BUILD); this header is just the public include path.
//
// The engine and every user-space allocation should come from the same-
// -allocator, so prefer these over raw new/delete/malloc in user-space code.
//
// Provides:
//   WFX::Alloc(size)          : raw allocation, like malloc
//   WFX::Realloc(ptr, size)   : raw resize, like realloc
//   WFX::Free(ptr)            : raw free
//   WFX::New<T>(args...)      : allocate + construct, like `new T(args...)`
//   WFX::Delete(ptr)          : destroy + free, like `delete ptr`
//   WFX::NewArray<T>(count)   : allocate + value-construct N, like `new T[count]{}`
//   WFX::DeleteArray(ptr, n)  : destroy N + free, like `delete[] ptr`
//   WFX::Allocator<T>         : std::allocator-compatible, stateless. Use with
//                                std::vector<T, WFX::Allocator<T>>,
//                                std::basic_string<char, std::char_traits<char>, WFX::Allocator<char>>, etc.
//   WFX::Vector<T>             : std::vector<T, WFX::Allocator<T>>
//   WFX::String                : std::basic_string<..., WFX::Allocator<char>>
//
// Examples:
//   auto* p = WFX::New<MyType>(arg1, arg2);
//   WFX::Delete(p);
//
//   WFX::Vector<int> v; // backing storage allocated via WFX::Alloc/Free
//   WFX::String s = "hi";
// -----------------------------------------------------------------------

#include "shared/utils/memory.hpp"

namespace WFX {

using WFX::Shared::Alloc;
using WFX::Shared::Allocator;
using WFX::Shared::Delete;
using WFX::Shared::DeleteArray;
using WFX::Shared::Free;
using WFX::Shared::New;
using WFX::Shared::NewArray;
using WFX::Shared::Realloc;
using WFX::Shared::String;
using WFX::Shared::Vector;

} // namespace WFX

#endif // WFX_INC_WFX_MEMORY_HPP
