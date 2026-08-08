// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_ASYNC_ENDPOINT_HPP
#define WFX_INC_ASYNC_ENDPOINT_HPP

#include "task.hpp" // EraseOnConnectImpl needs the complete Async::Task<T>, not just the fwd-decl builtins.hpp gets from promise.hpp
#include "builtins.hpp"
#include "core/core.hpp"
#include "core/deferred_init_vector.hpp"
#include "shared/abis/types.hpp"

namespace WFX::Async {

using namespace WFX::Shared;

// Fwd declare stuff
struct SlotHandle;

// co_await handle.OpenSideConnection() inside onConnect/onAbort. Opens a second, throwaway
// connection to the same endpoint (Postgres CancelRequest, MySQL COM_PROCESS_KILL, ...). The
// returned SlotHandle supports Send/Receive/UpgradeToTLS like any other, plus Close()
struct SlotOpenSideConnectionAwaitable : public AwaitableBase<SlotOpenSideConnectionAwaitable> {
    void* ownerImpl;

public:
    explicit SlotOpenSideConnectionAwaitable(void* impl) noexcept : AwaitableBase{}, ownerImpl(impl)
    {}

    // NOLINTNEXTLINE(readability-identifier-naming): C++20 coroutine protocol name, fixed spelling.
    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        handle = h;
        Core::EndpointApiExt1()->openSideConnection(ownerImpl, {this, OnComplete, OnDestroy});
        return true;
    }

    // Defined out-of-line below, once SlotHandle is a complete type
    struct Result;
    // NOLINTNEXTLINE(readability-identifier-naming): C++20 coroutine protocol name, fixed spelling.
    Result await_resume() const noexcept;
};

// Typed wrapper over EndpointSlotHandle passed into onConnect coroutines.
// Exposes co_await Send(...) and co_await Receive()
// The underlying impl pointer is the slot's context, type-erased at the ABI boundary
struct SlotHandle {
    EndpointSlotHandle raw;

public:
    SlotSendAwaitable Send(const void* data, std::uint32_t size) const noexcept
    {
        return SlotSendAwaitable{raw.impl, data, size};
    }

    // consumed: bytes of the PREVIOUS Receive() result already used, see SlotReceiveAwaitable
    SlotReceiveAwaitable Receive(std::uint32_t consumed = 0) const noexcept
    {
        return SlotReceiveAwaitable{raw.impl, consumed};
    }

    // Wraps this still-plaintext connection in TLS, for protocols that negotiate encryption
    // in-band (Postgres SSLRequest, SMTP STARTTLS). Probe with Send/Receive first, then call
    // this only if the server agreed. The endpoint must be configured plaintext, otherwise
    // the engine already wrapped it at connect time and this fails with EpSlotInvalidState.
    SlotUpgradeTlsAwaitable UpgradeToTLS() const noexcept
    {
        return SlotUpgradeTlsAwaitable{raw.impl};
    }

    // Empty if this slot isn't TLS or the handshake hasn't completed. Any
    // ALPN-aware protocol can call this to decide how to speak on this connection.
    StringView NegotiatedProtocol() const noexcept
    {
        return raw.negotiatedProtocol(raw.impl);
    }

    // Opens a second, throwaway connection to the same endpoint. Valid from onConnect and
    // onAbort alike (onAbort only gets this via AbortSlotHandle, not the full SlotHandle)
    SlotOpenSideConnectionAwaitable OpenSideConnection() const noexcept
    {
        return SlotOpenSideConnectionAwaitable{raw.impl};
    }

    // Closes a side connection early. No-op on a primary handle (raw.close is null there,
    // only a side connection's handle gets a real one)
    void Close() const noexcept
    {
        if(raw.close)
            raw.close(raw.impl);
    }
};

struct SlotOpenSideConnectionAwaitable::Result {
    Shared::SlotStatus status;
    SlotHandle handle;
};

// NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
inline SlotOpenSideConnectionAwaitable::Result SlotOpenSideConnectionAwaitable::await_resume() const noexcept
{
    const Shared::SlotStatus status = ResolveSlotStatus(result);
    if(status != Shared::SlotStatus::OK)
        return {status, SlotHandle{}};

    const auto* api = Core::EndpointApiExt1();

    EndpointSlotHandle raw{};
    raw.impl = result.data;
    raw.close = api->closeSideConnection;
    raw.negotiatedProtocol = api->negotiatedProtocol;
    return {status, SlotHandle{raw}};
}

// Restricted handle passed into onAbort coroutines: OpenSideConnection + NegotiatedProtocol only
// The primary slot is still mid-request (EVENT_ENDPOINT_RECV) when onAbort fires, so driving
// Send/Receive/UpgradeToTLS on it directly is illegal (crashes or corrupts the real response
// in flight). This is a compile-time restriction, not a runtime guard, since there's no safe
// general meaning for it. A protocol that needs to talk must go through OpenSideConnection()
struct AbortSlotHandle {
    EndpointSlotHandle raw;

public:
    SlotOpenSideConnectionAwaitable OpenSideConnection() const noexcept
    {
        return SlotOpenSideConnectionAwaitable{raw.impl};
    }

    StringView NegotiatedProtocol() const noexcept
    {
        return raw.negotiatedProtocol(raw.impl);
    }
};

// RAII owner for the response object returned by SendPayload
// Calls destroyOutput when it goes out of scope
// Using the response pointer after this object is destroyed is undefined behavior
template <typename T> struct EndpointOutput {
public: // Constructors & Destructors
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

public: // Operators
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
    T* Get() const noexcept
    {
        return ptr_;
    }

private: // Storage
    T* ptr_ = nullptr;
    EndpointDestroyStateFn destroy_ = nullptr;
};

// Returned by Resolve::SendPayload. Suspends the calling route handler coroutine, registers
// itself as the async completion target on the client context, then calls the engine's
// SendPayload
//
// Owns the request by value so it stays alive across the Send()/Get()/Post()-style helper
// chains a caller may build on top of Resolve, only takes the address of the owned copy in
// await_suspend(), by which point this awaitable is at its final, non-relocating address
//
// On synchronous failure (pool exhausted etc.) it resumes immediately so the caller can
// inspect the status without suspending at all
//
// On success the result is an EndpointOutput<TRes> (an RAII owner)
// It is valid for as long as the variable is in scope
template <typename TReq, typename TRes>
struct SendPayloadAwaitable : public AwaitableBase<SendPayloadAwaitable<TReq, TRes>> {
    TReq req;
    EndpointStatus syncStatus{};
    std::uint16_t endpointIdx{0};
    EndpointDestroyStateFn destroyOutput{nullptr};
    std::uint64_t pinnedSlot{0}; // 0 = pool-routed, else the ReservedSlot this was issued from

public:
    SendPayloadAwaitable(std::uint16_t idx, TReq r, EndpointDestroyStateFn destroy,
                         std::uint64_t pinnedSlot = 0) noexcept
        : AwaitableBase<SendPayloadAwaitable<TReq, TRes>>{}, req(std::move(r)), endpointIdx(idx),
          destroyOutput(destroy), pinnedSlot(pinnedSlot)
    {}

    // NOLINTNEXTLINE(readability-identifier-naming): C++20 coroutine protocol name, fixed spelling.
    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        this->handle = h;

        const EndpointStatus s =
            Core::EndpointApiExt1()->sendPayload(Core::HttpApiExt1()->getGlobalPtrData(), endpointIdx,
                                                 static_cast<const void*>(&req),
                                                 {this, AwaitableBase<SendPayloadAwaitable<TReq, TRes>>::OnComplete,
                                                  AwaitableBase<SendPayloadAwaitable<TReq, TRes>>::OnDestroy},
                                                 pinnedSlot);

        // Synchronous failure, engine could not start the operation. Resume immediately
        if(s != EndpointStatus::PENDING) {
            // Preserve the real reason
            syncStatus = s;

            this->result.status = AsyncStatus::IO_FAILURE;
            this->result.data = nullptr;

            return false;
        }

        // Suspend, engine resumes via HandleAsyncCallback
        return true;
    }

    // NOLINTNEXTLINE(readability-identifier-naming): C++20 coroutine protocol name, fixed spelling.
    std::pair<EndpointStatus, EndpointOutput<TRes>> await_resume() noexcept
    {
        // Synchronous failure. Return the actual engine status
        if(this->result.status == AsyncStatus::IO_FAILURE && syncStatus != EndpointStatus::SUCCESS)
            return {syncStatus, EndpointOutput<TRes>{}};

        // Async failure (engine fired IO_FAILURE via HandleAsyncCallback)
        if(this->result.status != AsyncStatus::COMPLETED)
            return {this->result.endpointStatus, EndpointOutput<TRes>{}};

        return {EndpointStatus::SUCCESS, EndpointOutput<TRes>{static_cast<TRes*>(this->result.data), destroyOutput}};
    }
};

// User facing onConnect function pointer type
// The user writes: Task<ConnectResult> MyConnect(SlotHandle, void*) and
// passes &MyConnect as the OnConnect template argument to Resolve
using UserOnConnectFn = Task<ConnectResult> (*)(SlotHandle, void*);

// Compile-time ABI erasure for the onConnect coroutine
// Takes the user's typed function pointer as a non-type template
// parameter and produces a stateless static wrapper matching the
// ABI signature EndpointOnConnectFn. Each distinct user function
// gets its own instantiation with its own function pointer
//
// constexpr nullptr specialization: returns nullptr directly so
// the engine skips the onConnect phase for simple protocols
template <UserOnConnectFn UserFn>
void EraseOnConnectImpl(EndpointSlotHandle handle, void* slotState, AsyncCompleteFn onDone, void* onDoneUd) noexcept
{
    const SlotHandle sh{handle};
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

// User facing onAbort function pointer type
// The user writes: Task<void> MyAbort(AbortSlotHandle, void*) and
// passes &MyAbort as the OnAbort template argument to Resolve
// No ConnectResult-style verdict: the original slot's fate is decided by its own ongoing
// receive/parse/timeout cycle, not by what onAbort returns
using UserOnAbortFn = Task<void> (*)(AbortSlotHandle, void*);

// Compile-time ABI erasure for the onAbort coroutine, same pattern as EraseOnConnectImpl/GetErasedOnConnect
template <UserOnAbortFn UserFn>
void EraseOnAbortImpl(EndpointSlotHandle handle, void* slotState, AsyncCompleteFn onDone, void* onDoneUd) noexcept
{
    const AbortSlotHandle sh{handle};
    auto task = UserFn(sh, slotState);
    task.SetCompletion(onDone, onDoneUd);
    task.Resume();
}

template <UserOnAbortFn UserFn> constexpr EndpointOnAbortFn GetErasedOnAbort() noexcept
{
    if constexpr(UserFn == nullptr)
        return nullptr;
    else
        return &EraseOnAbortImpl<UserFn>;
}

// One chunk of a streamed response. data borrows the engine's output object and stays valid
// only until the next Next(), which is what keeps peak memory at one chunk instead of the
// whole response. Copy anything you need to outlive the loop iteration
// done marks the final delivery, at which point data is null and the slot is back in the pool
template <typename TRes> struct StreamChunk {
    EndpointStatus status = EndpointStatus::SUCCESS;
    const TRes* data = nullptr;
    bool done = false;
};

// Returned by StreamHandle::Next(). The first Next() sends the request, later ones pull the
// following chunk; when the engine can satisfy one from already-buffered bytes it says so
// and this never suspends, mirroring SendPayloadAwaitable's synchronous path
template <typename TReq, typename TRes>
struct StreamNextAwaitable : public AwaitableBase<StreamNextAwaitable<TReq, TRes>> {
    const TReq* req;
    bool first;
    std::uint16_t endpointIdx;
    std::uint64_t pinnedSlot;
    EndpointStatus syncStatus{EndpointStatus::PENDING};

public:
    StreamNextAwaitable(std::uint16_t idx, const TReq* r, bool isFirst, std::uint64_t pinned) noexcept
        : AwaitableBase<StreamNextAwaitable<TReq, TRes>>{}, req(r), first(isFirst), endpointIdx(idx), pinnedSlot(pinned)
    {}

    // NOLINTNEXTLINE(readability-identifier-naming): C++20 coroutine protocol name, fixed spelling.
    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        this->handle = h;

        const AsyncData onDone{this, AwaitableBase<StreamNextAwaitable<TReq, TRes>>::OnComplete,
                               AwaitableBase<StreamNextAwaitable<TReq, TRes>>::OnDestroy};

        // The opening request is an ordinary send; the engine only learns this is a stream when
        // parse() first returns a CHUNK_* result, so nothing extra is needed on the send side
        const EndpointStatus s =
            first ? Core::EndpointApiExt1()->sendPayload(Core::HttpApiExt1()->getGlobalPtrData(), endpointIdx,
                                                         static_cast<const void*>(req), onDone, pinnedSlot)
                  : Core::EndpointApiExt1()->streamNext(Core::HttpApiExt1()->getGlobalPtrData(),
                                                        static_cast<const void*>(req), onDone);

        if(s == EndpointStatus::PENDING)
            return true;

        // Either a chunk was already buffered, the stream ended, or it failed. All three resume
        // immediately: suspending would strand the coroutine, since no callback is coming
        syncStatus = s;
        this->result.status = (s == EndpointStatus::CHUNK_AVAILABLE || s == EndpointStatus::SUCCESS)
                                  ? AsyncStatus::COMPLETED
                                  : AsyncStatus::IO_FAILURE;

        return false;
    }

    // NOLINTNEXTLINE(readability-identifier-naming): C++20 coroutine protocol name, fixed spelling.
    StreamChunk<TRes> await_resume() const noexcept
    {
        // Synchronous chunk: the engine left it in the slot's output object rather than firing
        // a callback, so ask for it now that the coroutine is safely past its suspend point
        if(syncStatus == EndpointStatus::CHUNK_AVAILABLE) {
            const auto* chunk =
                static_cast<const TRes*>(Core::EndpointApiExt1()->streamChunk(Core::HttpApiExt1()->getGlobalPtrData()));

            // A null chunk here would otherwise read as "more to come, but nothing yet" and spin
            // the caller's loop forever, so it terminates the stream instead
            return {EndpointStatus::SUCCESS, chunk, chunk == nullptr};
        }

        if(syncStatus == EndpointStatus::SUCCESS)
            return {EndpointStatus::SUCCESS, nullptr, true};

        if(this->result.status != AsyncStatus::COMPLETED)
            return {syncStatus != EndpointStatus::PENDING ? syncStatus : this->result.endpointStatus, nullptr, true};

        // PENDING on a delivered result means "chunk, more to come"; SUCCESS means the stream ended
        // A null chunk is terminal either way, for the same reason as the synchronous path above
        const auto* chunk = static_cast<const TRes*>(this->result.data);
        const bool isFinal = this->result.endpointStatus != EndpointStatus::PENDING || chunk == nullptr;

        return {EndpointStatus::SUCCESS, chunk, isFinal};
    }
};

// Drives a chunked response. Hold it across the whole loop: it owns the request, which
// cursor/paging protocols need handed back on every Next() to build their continuation
//
//   auto stream = ep.Stream(req);
//   while(true) {
//       auto chunk = co_await stream.Next();
//       if(chunk.status != WFX::EpOk || chunk.done) break;
//       // use chunk.data, only valid until the next Next()
//   }
template <typename TReq, typename TRes> class StreamHandle {
public:
    StreamHandle(std::uint16_t idx, TReq r, std::uint64_t pinned) noexcept
        : req_(std::move(r)), endpointIdx_(idx), pinnedSlot_(pinned)
    {}

    StreamHandle(StreamHandle&&) = default;
    StreamHandle& operator=(StreamHandle&&) = default;
    StreamHandle(const StreamHandle&) = delete;
    StreamHandle& operator=(const StreamHandle&) = delete;

public:
    StreamNextAwaitable<TReq, TRes> Next() noexcept
    {
        const bool isFirst = first_;
        first_ = false;

        return {endpointIdx_, &req_, isFirst, pinnedSlot_};
    }

private: // Storage
    TReq req_;
    std::uint16_t endpointIdx_ = 0;
    std::uint64_t pinnedSlot_ = 0;
    bool first_ = true;
};

// RAII owner of a connection pinned via Resolve::Reserve(). Every SendPayload through it runs
// on that one connection instead of whatever the pool hands out, which is what makes
// connection-scoped protocol state (an open transaction, a LISTEN subscription) work at all
//
// Releases on destruction, so an early co_return or a thrown exception can't leak the pin. The
// underlying slot can still die on its own (server hangup, idle timeout); the handle detects
// that and later sends fail with EpInvalidKey rather than landing on an unrelated connection
template <typename TReq, typename TRes> class ReservedSlot {
public:
    ReservedSlot(std::uint16_t idx, std::uint64_t handle, EndpointDestroyStateFn destroy) noexcept
        : endpointIdx_(idx), handle_(handle), destroyOutput_(destroy)
    {}

    ~ReservedSlot()
    {
        Release();
    }

    // Move-only: two owners would double-release
    ReservedSlot(ReservedSlot&& o) noexcept
        : endpointIdx_(o.endpointIdx_), handle_(o.handle_), destroyOutput_(o.destroyOutput_)
    {
        o.handle_ = 0;
    }
    ReservedSlot& operator=(ReservedSlot&& o) noexcept
    {
        if(this != &o) {
            Release();
            endpointIdx_ = o.endpointIdx_;
            handle_ = o.handle_;
            destroyOutput_ = o.destroyOutput_;
            o.handle_ = 0;
        }

        return *this;
    }

    ReservedSlot(const ReservedSlot&) = delete;
    ReservedSlot& operator=(const ReservedSlot&) = delete;

public:
    // False when the reservation failed (pool exhausted) or was already released
    bool IsValid() const noexcept
    {
        return handle_ != 0;
    }

    SendPayloadAwaitable<TReq, TRes> SendPayload(TReq req) const noexcept
    {
        return {endpointIdx_, std::move(req), destroyOutput_, handle_};
    }

    // Same chunked consumption as Resolve::Stream(), pinned to this reservation
    StreamHandle<TReq, TRes> Stream(TReq req) const noexcept
    {
        return {endpointIdx_, std::move(req), handle_};
    }

    // Idempotent, also runs on destruction. Safe to call mid-request: the engine hands the
    // connection back once that request finishes rather than yanking it out from under it
    void Release() noexcept
    {
        if(handle_ == 0)
            return;

        Core::EndpointApiExt1()->releaseSlot(handle_);
        handle_ = 0;
    }

private: // Storage
    std::uint16_t endpointIdx_ = 0;
    std::uint64_t handle_ = 0;
    EndpointDestroyStateFn destroyOutput_ = nullptr;
};

// Constructed once at namespace scope before Run(). Registers the endpoint with the engine
// via the deferred init vector and stores the assigned index for use in SendPayload calls
//
// Pass EndpointDesc directly. Leave onConnect = nullptr in the desc (Resolve fills it in from the
// OnConnect template parameter so the compiler produces a zero-cost stateless erasing wrapper)
//
// Example with onConnect:
//   inline const auto PgEndpoint = Resolve<PgReq, PgRes, PgOnConnect>{
//       "postgres.internal:5432", EndpointDesc{.serialize=..., .userCtx=&pgConfig}, config };
//
// Example without onConnect:
//   inline const auto RedisEndpoint = Resolve<RedisReq, RedisRes>{
//       "redis.internal:6379", EndpointDesc{.serialize=..., .parse=...}, config };
template <typename TReq, typename TRes, UserOnConnectFn OnConnect = nullptr, UserOnAbortFn OnAbort = nullptr>
class Resolve {
public:
    Resolve(const char* host, EndpointDesc desc, EndpointConfig config)
    {
        destroyOutput_ = desc.destroyOutput;
        Core::GlobalWFXDeferred.emplace_back([=, this] {
            EndpointDesc d = desc;
            d.onConnect = GetErasedOnConnect<OnConnect>();
            d.onAbort = GetErasedOnAbort<OnAbort>();
            endpointIdx_ = Core::EndpointApiExt1()->allocateEndpoint(host, d, config);
        });
    }

    // No copying or moving
    Resolve(const Resolve&) = delete;
    Resolve& operator=(const Resolve&) = delete;
    Resolve(Resolve&&) = default;
    Resolve& operator=(Resolve&&) = default;

public:
    // Takes req by value: the returned awaitable owns it, so it stays valid
    // across however many by-value helper functions (Send/Get/Post/...) a
    // caller layers on top before the co_await site actually suspends
    SendPayloadAwaitable<TReq, TRes> SendPayload(TReq req) const noexcept
    {
        return {endpointIdx_, std::move(req), destroyOutput_};
    }

    // Pins one connection to the caller so consecutive requests share it, for protocols where
    // that's load-bearing (SQL transactions, LISTEN/NOTIFY). Returns an empty ReservedSlot when
    // the pool is exhausted, check IsValid() before using it. Not available on multiplexed
    // endpoints (hasCapacity set), which already share one connection by design
    ReservedSlot<TReq, TRes> Reserve() const noexcept
    {
        return {endpointIdx_, Core::EndpointApiExt1()->reserveSlot(endpointIdx_), destroyOutput_};
    }

    // Consumes a response in chunks instead of materializing it whole, for results too large to
    // hold in memory. The protocol opts in by returning a CHUNK_* ParseResult; without that
    // this behaves like an ordinary SendPayload that completes on the first Next()
    StreamHandle<TReq, TRes> Stream(TReq req) const noexcept
    {
        return {endpointIdx_, std::move(req), 0};
    }

private:
    std::uint16_t endpointIdx_ = 0;
    EndpointDestroyStateFn destroyOutput_ = nullptr;
};

} // namespace WFX::Async

#endif // WFX_INC_ASYNC_ENDPOINT_HPP