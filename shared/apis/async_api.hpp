#ifndef WFX_SHARED_ASYNC_API_HPP
#define WFX_SHARED_ASYNC_API_HPP

#include "shared/abis/types.hpp"

// Fwd declare stuff
namespace WFX::Http { class HttpConnectionHandler; }

namespace WFX::Shared {

enum class AsyncAPIVersion : std::uint8_t {
    V1 = 1,
};

// Data internally used by Async API
struct AsyncAPIDataV1 {
    Http::HttpConnectionHandler* connHandler = nullptr;
};

// vvv All aliases for clarity vvv
using RegisterAsyncTimerFn = bool(*)(void* ctx, std::uint32_t delayMs, AsyncData asyncData);

// vvv API declarations vvv
struct ASYNC_API_TABLE {
    // vvv Async Operations vvv
    RegisterAsyncTimerFn   RegisterAsyncTimer;

    // Metadata
    AsyncAPIVersion apiVersion;
};

// vvv Getter & Initializers vvv
const ASYNC_API_TABLE* GetAsyncAPIV1();
void                   InitAsyncAPIV1(Http::HttpConnectionHandler*);

} // namespace WFX::Shared

#endif // WFX_SHARED_ASYNC_API_HPP