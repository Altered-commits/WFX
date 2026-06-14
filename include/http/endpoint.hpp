// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_HTTP_ENDPOINT_HPP
#define WFX_INC_HTTP_ENDPOINT_HPP

#include "async/builtins.hpp"
#include "core/core.hpp"
#include "core/deferred_init_vector.hpp"
#include "shared/abis/types.hpp"

namespace WFX::Http {

using namespace WFX::Shared;
using namespace WFX::Async;

// Typed wrapper over EndpointSlotHandle passed into onConnect-
// -coroutines. Exposes co_await Send(...) and co_await Receive()
// The underlying impl pointer is the slot's context, type-erased at the ABI boundary
struct SlotHandle {
    EndpointSlotHandle raw;

public:
    SlotSendAwaitable Send(const void* data, std::uint32_t size) const noexcept
    {
        return SlotSendAwaitable{raw.impl, data, size};
    }

    SlotReceiveAwaitable Receive() const noexcept
    {
        return SlotReceiveAwaitable{raw.impl};
    }
};

// Non-template aggregate the user fills out with designated-
// -initializers. All fields except 'onConnect' match the ABI-
// -function signatures directly. State pointer parameters are-
// -void* at the ABI level, the user casts internally
//
// 'onConnect' is intentionally absent from this struct because-
// -it requires ABI erasure that depends on a compile-time constant-
// -function pointer. It is passed as a non-type template argument-
// -on Resolve instead, which lets the compiler produce a zero-cost-
// -stateless erasing wrapper per distinct user function
struct EndpointHooks {
    EndpointSerializeFn serialize = nullptr;
    EndpointParseFn parse = nullptr;
    EndpointOnDisconnectFn onDisconnect = nullptr;      // nullable
    EndpointCreateStateFn createSlotState = nullptr;    // nullable
    EndpointDestroyStateFn destroySlotState = nullptr;  // nullable
    EndpointCreateStateFn createParseState = nullptr;   // nullable
    EndpointDestroyStateFn destroyParseState = nullptr; // nullable
    EndpointResetStateFn resetParseState = nullptr;     // nullable
    EndpointCreateStateFn createOutput = nullptr;       // nullable
    EndpointDestroyStateFn destroyOutput = nullptr;     // nullable
    EndpointCoalesceKeyFn coalesceKey = nullptr;        // nullable -> 0 = no coalescing
};

// Returned by Resolve::SendPayload. Suspends the calling route-
// -handler coroutine, registers itself as the async completion-
// -target on the client context, then calls the engine's SendPayload
//
// On synchronous failure (pool exhausted etc.) it resumes-
// -immediately so the caller can inspect the status without-
// -suspending at all
//
// On success the response pointer (TRes*) is non-owning. The-
// -engine owns the object's lifetime via destroyOutput. The-
// -pointer is valid only until the next co_await on this path
template <typename TRes> struct SendPayloadAwaitable : public AwaitableBase<SendPayloadAwaitable<TRes>> {
    EndpointStatus syncStatus{}; // set on synchronous failure path
    std::uint16_t endpointIdx{0};
    const void* req{};

public:
    SendPayloadAwaitable(std::uint16_t idx, const void* r) noexcept
        : AwaitableBase<SendPayloadAwaitable<TRes>>{}, endpointIdx(idx), req(r)
    {}

    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        this->handle_ = h;

        EndpointStatus s =
            Core::EndpointApiExt1()->SendPayload(Core::HttpApiExt1()->GetGlobalPtrData(), endpointIdx, req,
                                                 {this, AwaitableBase<SendPayloadAwaitable<TRes>>::OnComplete,
                                                  AwaitableBase<SendPayloadAwaitable<TRes>>::OnDestroy});

        // Synchronous failure, engine could not start the operation. Resume immediately
        if(s != EndpointStatus::PENDING) {
            // Preserve the real reason
            syncStatus = s;

            this->result_.status = AsyncStatus::IO_FAILURE;
            this->result_.data = nullptr;

            return false;
        }

        // Suspend, engine resumes via HandleAsyncCallback
        return true;
    }

    std::pair<EndpointStatus, TRes*> await_resume() const noexcept
    {
        // Synchronous failure. Return the actual engine status
        if(this->result_.status == AsyncStatus::IO_FAILURE && syncStatus != EndpointStatus::SUCCESS)
            return {syncStatus, nullptr};

        // Async failure (engine fired IO_FAILURE via HandleAsyncCallback)
        if(this->result_.status != AsyncStatus::COMPLETED)
            return {EndpointStatus::INTERNAL_ERROR, nullptr};

        return {EndpointStatus::SUCCESS, static_cast<TRes*>(this->result_.data)};
    }
};

// User facing onConnect function pointer type
// The user writes: Task<ConnectResult> MyConnect(SlotHandle, void*)-
// -and passes &MyConnect as the OnConnect template argument to Resolve
using UserOnConnectFn = Task<ConnectResult> (*)(SlotHandle, void*);

// Compile-time ABI erasure for the onConnect coroutine
// Takes the user's typed function pointer as a non-type template-
// -parameter and produces a stateless static wrapper matching the-
// -ABI signature EndpointOnConnectFn. Each distinct user function-
// -gets its own instantiation with its own function pointer
//
// constexpr nullptr specialization: returns nullptr directly so-
// -the engine skips the onConnect phase for simple protocols
template <UserOnConnectFn UserFn>
void EraseOnConnectImpl(EndpointSlotHandle handle, void* slotState, AsyncCompleteFn onDone, void* onDoneUd) noexcept
{
    SlotHandle sh{handle};
    auto task = UserFn(sh, slotState);
    task.SetCompletion(onDone, onDoneUd);
    task.Resume();
}

template <UserOnConnectFn UserFn> constexpr EndpointOnConnectFn GetErasedOnConnect() noexcept
{
    if constexpr(UserFn == nullptr)
        return nullptr;
    else
        return &EraseOnConnectImpl<UserFn>;
}

// Constructed once at namespace scope before Run(). Registers the-
// -endpoint with the engine via the deferred init vector and stores-
// -the assigned index for use in SendPayload calls
//
// OnConnect: pass a named function Task<ConnectResult>(SlotHandle, void*)-
// -or omit it (defaults to nullptr) for protocols with no handshake
//
// Example with onConnect:
//   inline const auto PgEndpoint = Resolve<PgReq, PgRes, PgOnConnect>{
//       "postgres.internal:5432", hooks, config, &pgConfig };
//
// Example without onConnect (Redis, Memcached, etc.):
//   inline const auto RedisEndpoint = Resolve<RedisReq, RedisRes>{
//       "redis.internal:6379", hooks, config };
// ============================================================
template <typename TReq, typename TRes, UserOnConnectFn OnConnect = nullptr> class Resolve {
public:
    Resolve(const char* host, EndpointHooks hooks, EndpointConfig config, void* userCtx = nullptr)
    {
        Core::__WFXDeferred.emplace_back([=, this] {
            EndpointDesc desc{};
            desc.serialize = hooks.serialize;
            desc.parse = hooks.parse;
            desc.onConnect = GetErasedOnConnect<OnConnect>();
            desc.onDisconnect = hooks.onDisconnect;
            desc.createSlotState = hooks.createSlotState;
            desc.destroySlotState = hooks.destroySlotState;
            desc.createParseState = hooks.createParseState;
            desc.destroyParseState = hooks.destroyParseState;
            desc.resetParseState = hooks.resetParseState;
            desc.createOutput = hooks.createOutput;
            desc.destroyOutput = hooks.destroyOutput;
            desc.coalesceKey = hooks.coalesceKey;
            desc.userCtx = userCtx;

            endpointIdx_ = Core::EndpointApiExt1()->AllocateEndpoint(host, desc, config);
        });
    }

    // No copying or moving
    Resolve(const Resolve&) = delete;
    Resolve& operator=(const Resolve&) = delete;
    Resolve(Resolve&&) = default;
    Resolve& operator=(Resolve&&) = default;

public:
    // Returns std::pair<EndpointStatus, TRes*>.
    // TRes* is non-owning. See SendPayloadAwaitable for lifetime notes
    SendPayloadAwaitable<TRes> SendPayload(const TReq& req) const noexcept
    {
        return {endpointIdx_, static_cast<const void*>(&req)};
    }

private:
    std::uint16_t endpointIdx_ = 0;
};

} // namespace WFX::Http

#endif // WFX_INC_HTTP_ENDPOINT_HPP