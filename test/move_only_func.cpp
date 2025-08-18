#include <iostream>
#include <chrono>
#include <memory>
#include <vector>
#include <functional>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
size_t getProcessMemoryUsageKB() {
    PROCESS_MEMORY_COUNTERS counters;
    GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
    return counters.WorkingSetSize / 1024;
}
#else
#include <malloc.h>
size_t getProcessMemoryUsageKB() {
    struct mallinfo info = mallinfo();
    return info.uordblks / 1024; // used space
}
#endif

#include <memory>
#include <utility>

template<typename Signature>
class MoveOnlyFunction;

template<typename R, typename... Args>
class MoveOnlyFunction<R(Args...)> {
    struct Base {
        virtual R invoke(Args&&...) = 0;
        virtual ~Base() = default;
    };

    template<typename F>
    struct Impl final : Base {
        F f;
        explicit Impl(F&& func) noexcept(std::is_nothrow_move_constructible_v<F>)
            : f(std::move(func)) {}

        R invoke(Args&&... args) override {
            return f(std::forward<Args>(args)...);
        }
    };

    std::unique_ptr<Base> impl_;

public:
    template<typename F>
    explicit MoveOnlyFunction(F&& f) 
        : impl_(std::make_unique<Impl<std::decay_t<F>>>(std::forward<F>(f))) {}

    MoveOnlyFunction(MoveOnlyFunction&&) noexcept = default;
    MoveOnlyFunction& operator=(MoveOnlyFunction&&) noexcept = default;

    MoveOnlyFunction(const MoveOnlyFunction&) = delete;
    MoveOnlyFunction& operator=(const MoveOnlyFunction&) = delete;

    R operator()(Args... args) {
        return impl_->invoke(std::forward<Args>(args)...);
    }

    explicit operator bool() const noexcept {
        return static_cast<bool>(impl_);
    }

    void reset() noexcept {
        impl_.reset();
    }
};

// #include <utility>
// #include <type_traits>
// #include <cstdint>
// #include <cstring>
// #include <new>

// template<typename Signature>
// class MoveOnlyFunction;

// template<typename R, typename... Args>
// class MoveOnlyFunction<R(Args...)> {
//     static constexpr size_t SBO_SIZE = 64;
//     static constexpr size_t SBO_ALIGN = alignof(max_align_t);

//     struct VTable {
//         R (*invoke)(void*, Args&&...);
//         void (*destroy)(void*);
//         void (*move)(void* dest, void* src);
//     };

//     alignas(SBO_ALIGN) std::uint8_t storage_[SBO_SIZE];
//     const VTable* vtable_ = nullptr;

//     bool inUse_ = false;

// public:
//     MoveOnlyFunction() noexcept = default;

//     MoveOnlyFunction(MoveOnlyFunction&& other) noexcept {
//         if (other.vtable_) {
//             other.vtable_->move(&storage_, &other.storage_);
//             vtable_ = other.vtable_;
//             inUse_ = true;
//             other.vtable_ = nullptr;
//             other.inUse_ = false;
//         }
//     }

//     MoveOnlyFunction& operator=(MoveOnlyFunction&& other) noexcept {
//         if (this != &other) {
//             reset();
//             if (other.vtable_) {
//                 other.vtable_->move(&storage_, &other.storage_);
//                 vtable_ = other.vtable_;
//                 inUse_ = true;
//                 other.vtable_ = nullptr;
//                 other.inUse_ = false;
//             }
//         }
//         return *this;
//     }

//     template<typename F>
//     MoveOnlyFunction(F&& f) {
//         static_assert(!std::is_lvalue_reference_v<F>, "F must be an rvalue");
//         using Fn = std::decay_t<F>;
//         static_assert(sizeof(Fn) <= SBO_SIZE, "Functor too large for SBO");
//         static_assert(alignof(Fn) <= SBO_ALIGN, "Functor alignment too strict");

//         struct Impl {
//             static R invoke(void* self, Args&&... args) {
//                 return (*reinterpret_cast<Fn*>(self))(std::forward<Args>(args)...);
//             }

//             static void destroy(void* self) {
//                 reinterpret_cast<Fn*>(self)->~Fn();
//             }

//             static void move(void* dest, void* src) {
//                 new (dest) Fn(std::move(*reinterpret_cast<Fn*>(src)));
//                 destroy(src);
//             }
//         };

//         new (&storage_) Fn(std::forward<F>(f));
//         vtable_ = new VTable{ &Impl::invoke, &Impl::destroy, &Impl::move };
//         inUse_ = true;
//     }

//     ~MoveOnlyFunction() {
//         reset();
//     }

//     R operator()(Args... args) {
//         return vtable_->invoke(&storage_, std::forward<Args>(args)...);
//     }

//     void reset() {
//         if (vtable_ && inUse_) {
//             vtable_->destroy(&storage_);
//             vtable_ = nullptr;
//             inUse_ = false;
//         }
//     }

//     explicit operator bool() const noexcept {
//         return vtable_ != nullptr;
//     }

//     MoveOnlyFunction(const MoveOnlyFunction&) = delete;
//     MoveOnlyFunction& operator=(const MoveOnlyFunction&) = delete;
// };

#include <functional>

#include <iostream>
#include <chrono>
#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <cstring>

constexpr int NUM = 100000;
constexpr size_t BUF_SIZE = 4096;

void BenchmarkMoveOnlyFunction() {
    std::cout << "=== MoveOnlyFunction Benchmark ===\n";
    auto beforeMem = getProcessMemoryUsageKB();

    std::vector<MoveOnlyFunction<void()>> funcs;
    funcs.reserve(NUM);

    auto startConstruct = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM; ++i) {
        int socket = i;
        auto buf = std::make_unique<char[]>(BUF_SIZE);
        auto str = std::string("Callback " + std::to_string(i));

        funcs.emplace_back([socket, buf = std::move(buf), str = std::move(str)]() mutable {
            if (socket >= 0) {
                std::memset(buf.get(), 'Z', BUF_SIZE);
                buf[0] = 'X';
                str += "!";
            }
        });
    }

    auto endConstruct = std::chrono::high_resolution_clock::now();

    auto startCall = std::chrono::high_resolution_clock::now();

    for (auto& fn : funcs)
        fn();

    auto endCall = std::chrono::high_resolution_clock::now();

    auto afterMem = getProcessMemoryUsageKB();

    std::cout << "Construction Time : " << std::chrono::duration<double, std::milli>(endConstruct - startConstruct).count() << " ms\n";
    std::cout << "Invocation Time   : " << std::chrono::duration<double, std::milli>(endCall - startCall).count() << " ms\n";
    std::cout << "Memory Increase   : " << (afterMem - beforeMem) << " KB\n";
    std::cout << std::endl;
}

void BenchmarkStdFunction() {
    std::cout << "=== std::function Benchmark ===\n";
    auto beforeMem = getProcessMemoryUsageKB();

    std::vector<std::function<void()>> funcs;
    funcs.reserve(NUM);

    auto startConstruct = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM; ++i) {
        int socket = i;
        auto buf = std::make_shared<std::unique_ptr<char[]>>(std::make_unique<char[]>(BUF_SIZE));
        auto str = std::make_shared<std::string>("Callback " + std::to_string(i));

        funcs.emplace_back([socket, buf, str]() mutable {
            if (socket >= 0) {
                std::memset((*buf).get(), 'Z', BUF_SIZE);
                (*buf)[0] = 'X';
                *str += "!";
            }
        });
    }

    auto endConstruct = std::chrono::high_resolution_clock::now();

    auto startCall = std::chrono::high_resolution_clock::now();

    for (auto& fn : funcs)
        fn();

    auto endCall = std::chrono::high_resolution_clock::now();

    auto afterMem = getProcessMemoryUsageKB();

    std::cout << "Construction Time : " << std::chrono::duration<double, std::milli>(endConstruct - startConstruct).count() << " ms\n";
    std::cout << "Invocation Time   : " << std::chrono::duration<double, std::milli>(endCall - startCall).count() << " ms\n";
    std::cout << "Memory Increase   : " << (afterMem - beforeMem) << " KB\n";
    std::cout << std::endl;
}

int main() {
    BenchmarkMoveOnlyFunction();
    BenchmarkStdFunction();
    return 0;
}