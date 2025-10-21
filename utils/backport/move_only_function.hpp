#ifndef WFX_UTILS_MOVE_ONLY_FUNCTION_HPP
#define WFX_UTILS_MOVE_ONLY_FUNCTION_HPP

#include <memory>
#include <utility>

namespace WFX::Utils {

// These exist in C++23 but i'm currently using C++17 so i have no option but to write one myself
template<typename Signature>
class MoveOnlyFunction;

template<typename R, typename... Args>
class MoveOnlyFunction<R(Args...)> {
    struct Base {
        virtual R Invoke(Args&&...) = 0;
        virtual R InvokeConst(Args&&...) const = 0;
        virtual ~Base() = default;
    };

    template<typename F>
    struct Impl final : Base {
        F f;
        explicit Impl(F&& func) noexcept(std::is_nothrow_move_constructible_v<F>)
            : f(std::move(func)) {}

        R Invoke(Args&&... args) override
        {
            return f(std::forward<Args>(args)...);
        }

        // This version must NOT call f() if its not const callable
        R InvokeConst(Args&&... args) const override
        {
            // Callable supports const operator()
            if constexpr(std::is_invocable_v<const F&, Args...>)
                return f(std::forward<Args>(args)...);

            // Fallback for mutable lambdas, cast away const safely
            else
                return const_cast<F&>(f)(std::forward<Args>(args)...);
        }
    };

    std::unique_ptr<Base> impl_;

public:
    // Support for Late-Initialization
    MoveOnlyFunction() noexcept = default;
    // Implicit construction from any move-only callable
    template<typename F, typename = std::enable_if_t<
            !std::is_same_v<std::decay_t<F>, MoveOnlyFunction> &&
            !std::is_same_v<std::decay_t<F>, std::nullptr_t>
        >
    >
    MoveOnlyFunction(F&& f)
        : impl_(std::make_unique<Impl<std::decay_t<F>>>(std::forward<F>(f))) {}

    MoveOnlyFunction(MoveOnlyFunction&&) noexcept = default;
    MoveOnlyFunction& operator=(MoveOnlyFunction&&) noexcept = default;

    MoveOnlyFunction(const MoveOnlyFunction&) = delete;
    MoveOnlyFunction& operator=(const MoveOnlyFunction&) = delete;

    R operator()(Args... args)
    {
        return impl_->Invoke(std::forward<Args>(args)...);
    }

    // Const call overload
    R operator()(Args... args) const
    {
        return impl_->InvokeConst(std::forward<Args>(args)...);
    }

    operator bool() const noexcept {
        return static_cast<bool>(impl_);
    }

    // Extras
    void Reset() noexcept {
        impl_.reset();
    }
};

} // WFX::Utils

#endif // WFX_UTILS_MOVE_ONLY_FUNCTION_HPP