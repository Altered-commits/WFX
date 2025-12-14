#ifndef WFX_UTILS_MOVE_ONLY_FUNCTION_HPP
#define WFX_UTILS_MOVE_ONLY_FUNCTION_HPP

#include <type_traits>
#include "utils/logger/logger.hpp"

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
            if constexpr(std::is_void_v<R>)
                f(std::forward<Args>(args)...);
            else
                return f(std::forward<Args>(args)...);
        }

        // Helper for 'InvokeConst'
        template<typename Func>
        static R Call(Func&& func, Args&&... args)
        {
            if constexpr(std::is_void_v<R>)
                std::forward<Func>(func)(std::forward<Args>(args)...);
            else
                return std::forward<Func>(func)(std::forward<Args>(args)...);
        }

        // This version must NOT call f() if its not const callable
        R InvokeConst(Args&&... args) const override
        {
            // Callable supports const operator()
            if constexpr(std::is_invocable_v<const F&, Args...>)
                return Call(f, std::forward<Args>(args)...);

            // Safe fallback for mutable lambdas
            else
                return Call(const_cast<F&>(f), std::forward<Args>(args)...);
        }
    };

    Base* impl_ = nullptr;

public:
    // vvv Constructors and Destructor vvv
    // Support for Late-Initialization
    MoveOnlyFunction() noexcept = default;

    // Implicit construction from any move-only callable
    template<typename F, typename = std::enable_if_t<
            !std::is_same_v<std::decay_t<F>, MoveOnlyFunction> &&
            !std::is_same_v<std::decay_t<F>, std::nullptr_t>
        >
    >
    MoveOnlyFunction(F&& f)
        : impl_(new Impl<std::decay_t<F>>(std::forward<F>(f))) {}

    ~MoveOnlyFunction()
    {
        delete impl_;
    }

    // vvv Move Constructor and Operator vvv
    MoveOnlyFunction(MoveOnlyFunction&& other) noexcept
        : impl_(other.impl_)
    {
        other.impl_ = nullptr;
    }

    MoveOnlyFunction& operator=(MoveOnlyFunction&& other) noexcept
    {
        if(this != &other) {
            delete impl_;
            impl_ = other.impl_;
            other.impl_ = nullptr;
        }
        return *this;
    }

    // Copying is not allowed
    MoveOnlyFunction(const MoveOnlyFunction&)            = delete;
    MoveOnlyFunction& operator=(const MoveOnlyFunction&) = delete;

public:
    R operator()(Args... args)
    {
        if(!impl_)
            Logger::GetInstance().Fatal("[MoveOnlyFunction]: 'operator()' called but function is nullptr");

        return impl_->Invoke(std::forward<Args>(args)...);
    }

    // Const call overload
    R operator()(Args... args) const
    {
        if(!impl_)
            Logger::GetInstance().Fatal("[MoveOnlyFunction]: 'operator() const' called but function is nullptr");

        return impl_->InvokeConst(std::forward<Args>(args)...);
    }

    operator bool() const noexcept
    {
        return impl_ != nullptr;
    }

    // Extras
    void Reset() noexcept
    {
        delete impl_;
        impl_ = nullptr;
    }
};

} // WFX::Utils

#endif // WFX_UTILS_MOVE_ONLY_FUNCTION_HPP