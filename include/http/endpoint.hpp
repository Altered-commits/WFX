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

// RAII owner for the response object returned by SendPayload
// Calls destroyOutput when it goes out of scope
// Using the response pointer after this object is destroyed is undefined behavior
template <typename T> struct EndpointOutput {
    EndpointOutput() = default;
    EndpointOutput(T* ptr, EndpointDestroyStateFn destroy) noexcept : ptr_(ptr), destroy_(destroy)
    {}
    ~EndpointOutput()
    {
        if(ptr_ && destroy_)
            destroy_(ptr_);
    }

    EndpointOutput(EndpointOutput&& o) noexcept : ptr_(o.ptr_), destroy_(o.destroy_)
    {
        o.ptr_ = nullptr;
    }
    EndpointOutput& operator=(EndpointOutput&& o) noexcept
    {
        if(this != &o) {
            if(ptr_ && destroy_)
                destroy_(ptr_);

            ptr_ = o.ptr_;
            destroy_ = o.destroy_;
            o.ptr_ = nullptr;
        }

        return *this;
    }
    EndpointOutput(const EndpointOutput&) = delete;
    EndpointOutput& operator=(const EndpointOutput&) = delete;

public:
    T* operator->() const noexcept
    {
        return ptr_;
    }
    T& operator*() const noexcept
    {
        return *ptr_;
    }
    explicit operator bool() const noexcept
    {
        return ptr_ != nullptr;
    }
    T* get() const noexcept
    {
        return ptr_;
    }

private:
    T* ptr_ = nullptr;
    EndpointDestroyStateFn destroy_ = nullptr;
};

// Returned by Resolve::SendPayload. Suspends the calling route-
// -handler coroutine, registers itself as the async completion-
// -target on the client context, then calls the engine's SendPayload
//
// On synchronous failure (pool exhausted etc.) it resumes-
// -immediately so the caller can inspect the status without-
// -suspending at all
//
// On success the result is an EndpointOutput<TRes> (an RAII owner)
// It is valid for as long as the variable is in scope
template <typename TRes> struct SendPayloadAwaitable : public AwaitableBase<SendPayloadAwaitable<TRes>> {
    EndpointStatus syncStatus{}; // set on synchronous failure path
    std::uint16_t endpointIdx{0};
    const void* req{};
    EndpointDestroyStateFn destroyOutput_{nullptr};

public:
    SendPayloadAwaitable(std::uint16_t idx, const void* r, EndpointDestroyStateFn destroy) noexcept
        : AwaitableBase<SendPayloadAwaitable<TRes>>{}, endpointIdx(idx), req(r), destroyOutput_(destroy)
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

    std::pair<EndpointStatus, EndpointOutput<TRes>> await_resume() noexcept
    {
        // Synchronous failure. Return the actual engine status
        if(this->result_.status == AsyncStatus::IO_FAILURE && syncStatus != EndpointStatus::SUCCESS)
            return {syncStatus, EndpointOutput<TRes>{}};

        // Async failure (engine fired IO_FAILURE via HandleAsyncCallback)
        if(this->result_.status != AsyncStatus::COMPLETED)
            return {this->result_.endpointStatus, EndpointOutput<TRes>{}};

        return {EndpointStatus::SUCCESS, EndpointOutput<TRes>{static_cast<TRes*>(this->result_.data), destroyOutput_}};
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

// Constructed once at namespace scope before Run(). Registers the endpoint with the engine-
// -via the deferred init vector and stores the assigned index for use in SendPayload calls
//
// Pass EndpointDesc directly. Leave onConnect = nullptr in the desc (Resolve fills it in from the-
// OnConnect template parameter so the compiler produces a zero-cost stateless erasing wrapper)
//
// Example with onConnect:
//   inline const auto PgEndpoint = Resolve<PgReq, PgRes, PgOnConnect>{
//       "postgres.internal:5432", EndpointDesc{.serialize=..., .userCtx=&pgConfig}, config };
//
// Example without onConnect:
//   inline const auto RedisEndpoint = Resolve<RedisReq, RedisRes>{
//       "redis.internal:6379", EndpointDesc{.serialize=..., .parse=...}, config };
template <typename TReq, typename TRes, UserOnConnectFn OnConnect = nullptr> class Resolve {
public:
    Resolve(const char* host, EndpointDesc desc, EndpointConfig config)
    {
        destroyOutput_ = desc.destroyOutput;
        Core::__WFXDeferred.emplace_back([=, this] {
            EndpointDesc d = desc;
            d.onConnect = GetErasedOnConnect<OnConnect>();
            endpointIdx_ = Core::EndpointApiExt1()->AllocateEndpoint(host, d, config);
        });
    }

    // No copying or moving
    Resolve(const Resolve&) = delete;
    Resolve& operator=(const Resolve&) = delete;
    Resolve(Resolve&&) = default;
    Resolve& operator=(Resolve&&) = default;

public:
    // Returns std::pair<EndpointStatus, EndpointOutput<TRes>>
    SendPayloadAwaitable<TRes> SendPayload(const TReq& req) const noexcept
    {
        return {endpointIdx_, static_cast<const void*>(&req), destroyOutput_};
    }

private:
    std::uint16_t endpointIdx_ = 0;
    EndpointDestroyStateFn destroyOutput_ = nullptr;
};

} // namespace WFX::Http

#endif // WFX_INC_HTTP_ENDPOINT_HPP