// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_ASYNC_RESPONSE_HPP
#define WFX_INC_ASYNC_RESPONSE_HPP

#include "awaitable.hpp"
#include "core/core.hpp"
#include "shared/abis/types.hpp"

namespace WFX::Async {

using namespace WFX::Shared;

// Returned by Response::Flush()/FlushEnd(). Sends whatever's currently buffered in the response
// body as one HTTP chunk, then resets the buffer for the next round. Suspends the calling
// coroutine only if the socket isn't immediately writable, same synchronous-fast-path shape as
// SendPayloadAwaitable/StreamNextAwaitable.
struct FlushAwaitable : public AwaitableBase<FlushAwaitable> {
    bool isFinal;
    FlushStatus syncStatus{FlushStatus::PENDING};

public:
    explicit FlushAwaitable(bool final) noexcept : AwaitableBase{}, isFinal(final)
    {}

    // NOLINTNEXTLINE(readability-identifier-naming): C++20 coroutine protocol name, fixed spelling.
    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        handle = h;

        const FlushStatus s = Core::HttpApiExt1()->flushChunk(Core::HttpApiExt1()->getGlobalPtrData(), isFinal,
                                                              {this, OnComplete, OnDestroy});

        if(s == FlushStatus::PENDING)
            return true;

        syncStatus = s;
        return false;
    }

    // NOLINTNEXTLINE(readability-identifier-naming): C++20 coroutine protocol name, fixed spelling.
    FlushStatus await_resume() const noexcept
    {
        if(syncStatus != FlushStatus::PENDING)
            return syncStatus;

        return result.flushStatus;
    }
};

} // namespace WFX::Async

#endif // WFX_INC_ASYNC_RESPONSE_HPP
