// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_ASYNC_API_HPP
#define WFX_SHARED_ASYNC_API_HPP

#include "shared/abis/types.hpp"

// Fwd declare stuff
namespace WFX::Http {
struct HttpConnectionHandler;
}

namespace WFX::Shared {

// Data internally used by Async API
struct AsyncAPIDataExt1 {
    Http::HttpConnectionHandler* connHandler = nullptr;
};

// vvv All aliases for clarity vvv
using RegisterAsyncTimerFn = bool (*)(void* ctx, std::uint32_t delayMs, AsyncData asyncData);

// vvv API declarations vvv
struct AsyncAPIExt1 {
    RegisterAsyncTimerFn registerAsyncTimer;
};
static_assert(std::is_standard_layout<AsyncAPIExt1>::value, "'ASYNC_API_EXT1' must be standard layout");

// vvv Getter & Initializers vvv
const AsyncAPIExt1* GetAsyncAPIExt1();
void InitAsyncAPIExt1(Http::HttpConnectionHandler*);

} // namespace WFX::Shared

#endif // WFX_SHARED_ASYNC_API_HPP