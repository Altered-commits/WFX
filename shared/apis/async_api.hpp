#ifndef WFX_SHARED_ASYNC_API_HPP
#define WFX_SHARED_ASYNC_API_HPP

#include "async/interface.hpp"

// Fwd declare stuff
namespace WFX::Http {
    class HttpConnectionHandler;
}

namespace WFX::Shared {

using WFX::Http::HttpConnectionHandler;

enum class AsyncAPIVersion : std::uint8_t {
    V1 = 1,
};

// Data internally used by Async API
struct AsyncAPIDataV1 {
    HttpConnectionHandler* connHandler = nullptr;
};

// vvv All aliases for clarity vvv
using RegisterCallbackFn     = Async::CoroutineRawPtr (*)(void*, Async::CoroutinePtr&&);
using PopCallbackFn          = void                   (*)(void*);
using ResumeRecentCallbackFn = bool                   (*)(void*);

using RegisterAsyncTimerFn   = bool (*)(void*, std::uint32_t);

// vvv API declarations vvv
struct ASYNC_API_TABLE {
    // vvv Base Operations vvv
    RegisterCallbackFn     RegisterCallback;
    PopCallbackFn          PopCallback;
    ResumeRecentCallbackFn ResumeRecentCallback;

    // vvv Async Operations vvv
    RegisterAsyncTimerFn   RegisterAsyncTimer;

    // Metadata
    AsyncAPIVersion apiVersion;
};

// vvv Getter & Initializers vvv
const ASYNC_API_TABLE* GetAsyncAPIV1();
void                   InitAsyncAPIV1(HttpConnectionHandler*);

} // namespace WFX::Shared

#endif // WFX_SHARED_ASYNC_API_HPP