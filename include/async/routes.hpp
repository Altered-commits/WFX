#ifndef WFX_INC_ASYNC_ROUTES_HPP
#define WFX_INC_ASYNC_ROUTES_HPP

/*
 * Contains the sugar syntax for async functions
 * While i could've defined all these inside of http/routes.hpp, decided it was cleaner to define-
 * -them here. They will get included anyways later on
 */

#include "interface.hpp"
#include "core/core.hpp"
#include "core/macros.hpp"
#include <tuple>

namespace Async {

// Everything will be decayed except std::ref, which stays as is
template<typename T>
struct StoredHelper {
    using type = std::decay_t<T>;
};

template<typename T>
struct StoredHelper<std::reference_wrapper<T>> {
    using type = std::reference_wrapper<T>;
};

template<typename T>
using StoredType = typename StoredHelper<T>::type;

// Wraps any callable + args into a CoroutineBase
template<typename Fn, typename... Args>
class CallableCoroutine final : public CoroutineBase {
public:
    Fn fn_;
    std::tuple<StoredType<Args>...> args_;

    CallableCoroutine(Fn&& fn, Args&&... args)
        : fn_(std::forward<Fn>(fn)),
          args_(std::forward<Args>(args)...)
    {}

    CallableCoroutine(const CallableCoroutine&)            = delete;
    CallableCoroutine& operator=(const CallableCoroutine&) = delete;

    void Resume() noexcept override
    {
        SetYielded(false);
        std::apply(
            [this](auto&&... unpacked) {
                fn_(this, UnwrapRef(unpacked)...);
            },
            args_
        );
    }

private:
    template<typename T>
    constexpr static decltype(auto) UnwrapRef(T&& value) noexcept
    {
        if constexpr(IsReferenceWrapperV<T>)
            return value.get();      // Unwrap
        else
            return value;            // Pass through unchanged
    }
};

// Factory that constructs, registers and resumes async func
// IMPORTANT: THIS FUNCTION EXPECTS POINTER TO CONNECTION CONTEXT BE SET VIA 'SetGlobalPtrData' BEFORE BEING INVOKED
template<typename Fn, typename... Args>
inline AsyncPtr MakeAsync(Fn&& fn, Args&&... args)
{
    // Keep 'Args' as forwarding refs
    using CoroutineType = CallableCoroutine<std::decay_t<Fn>, Args&&...>;

    auto coro = std::make_unique<CoroutineType>(std::forward<Fn>(fn), std::forward<Args>(args)...);
    auto ptr = __WFXApi->GetAsyncAPIV1()->RegisterCallback(
                __WFXApi->GetHttpAPIV1()->GetGlobalPtrData(), std::move(coro)
            );
    
    if(ptr)
        ptr->Resume();

    return ptr;
}

// Wrapper around 'MakeAsync', if u want to make ur life easier in implementing free standing functions-
// -(You don't have to call 'MakeAsync' directly). Only caveat is, u cannot directly call that function-
// -instead u have to use the below function to call it for you
template<typename Fn, typename... Args>
inline AsyncPtr Call(Fn&& fn, Args&&... args)
{
    return MakeAsync(std::forward<Fn>(fn), std::forward<Args>(args)...);
}

// Await returns whether to yield or not. True means we need to yield, False me no need to [yield / handle error]-
// -function was completed in sync. It also increments 'self' state by 1 on every call.
// IMPORTANT: THIS FUNCTION EXPECTS POINTER TO CONNECTION CONTEXT BE SET VIA 'SetGlobalPtrData' BEFORE BEING INVOKED
template<typename T>
inline bool Await(AsyncPtr self, T&& asyncCall) noexcept
{
    static_assert(Async::IsCoroutinePtrV<T>, "Async::Await() requires callable to return 'AsyncPtr'.");

    if(self->IsYielded())
        WFX_ABORT("Async::Await() called while coroutine still yielded from previous await")

    self->IncState();

    AsyncPtr result = asyncCall;

    if(!result) {
        self->SetError(Error::INTERNAL_FAILURE);
        goto __CallComplete;
    }

    if(!result->IsFinished()) {
        self->SetYielded(true);
        return true;
    }

    // If the async completed instantly but failed, propagate error
    if(result->HasError())
        self->SetError(result->GetError());

__CallComplete:
    __WFXApi->GetAsyncAPIV1()->PopCallback(__WFXApi->GetHttpAPIV1()->GetGlobalPtrData());
    return false;
}

} // namespace Async

#endif // WFX_INC_ASYNC_ROUTES_HPP