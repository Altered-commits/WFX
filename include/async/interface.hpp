#ifndef WFX_INC_ASYNC_INTERFACE_HPP
#define WFX_INC_ASYNC_INTERFACE_HPP

/*
 * Interface for async emulation in C++17
 */

#include <cstdint>
#include <memory>
#include <type_traits>

namespace Async {

enum class Error {
    NONE = 0,
    TIMER_FAILURE,
    IO_FAILURE,
    INTERNAL_FAILURE
};

struct CoroutineBase {
    CoroutineBase()
        : __Done(0), __Yielded(0), __Error(0), __Pad(0)
    {}

    virtual ~CoroutineBase() = default;

    void          IncState()                       { __State_++; }
    void          SetState(std::uint32_t newState) { __State_ = newState; }
    std::uint32_t GetState()                       { return __State_; }

    void          SetYielded(bool yielded)         { __Yielded = yielded; }
    bool          IsYielded()                      { return __Yielded; }
    void          Finish()                         { __Done = 1; }
    bool          IsFinished()                     { return __Done; }

    void          SetError(Error e)                { __Error = static_cast<std::uint8_t>(e); }
    Error         GetError() const                 { return static_cast<Error>(__Error); }
    bool          HasError() const                 { return __Error != static_cast<std::uint8_t>(Error::NONE); }

    virtual void  Resume() noexcept = 0;

private: // Internals
    std::uint32_t __State_ = 0;
    struct {
        std::uint8_t __Done    : 1;
        std::uint8_t __Yielded : 1; // For safeguarding against forgotten 'return' after 'Await'
        std::uint8_t __Error   : 3; // 'Error' enum
        std::uint8_t __Pad     : 3;
    };
};

// Ease of use :)
using CoroutinePtr    = std::unique_ptr<CoroutineBase>;
using CoroutineRawPtr = CoroutineBase*;

// vvv Custom Traits vvv
//  Coro
template<typename T>
struct RemoveConstValRef {
    using type = std::remove_cv_t<std::remove_reference_t<T>>;
};

template<typename T>
using RemoveConstValRefT = typename RemoveConstValRef<T>::type;

template<typename T>
constexpr bool IsCoroutinePtrV = std::is_same_v<RemoveConstValRefT<T>, CoroutineRawPtr>;

//  Ref
template<typename T>
struct IsReferenceWrapper : std::false_type {};

template<typename U>
struct IsReferenceWrapper<std::reference_wrapper<U>> : std::true_type {};

// helper variable template
template<typename T>
constexpr bool IsReferenceWrapperV = IsReferenceWrapper<std::remove_reference_t<T>>::value;

} // namespace Async

// Considering this shits used quite often, better just alias it atp
using AsyncPtr = Async::CoroutineRawPtr;

#endif // WFX_INC_ASYNC_INTERFACE_HPP