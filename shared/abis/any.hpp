#ifndef WFX_SHARED_ABI_ANY_HPP
#define WFX_SHARED_ABI_ANY_HPP

#include <cstdint>
#include <type_traits>

namespace WFX::Shared {

struct alignas(8) Any {
public:
    void* data;
    void (*destructor)(void*);

public: // vvv Basic methods vvv
    void Reset() noexcept
    {
        if(data && destructor)
            destructor(data);

        data       = nullptr;
        destructor = nullptr;
    }

    bool        HasValue() const noexcept { return data != nullptr; }
    void*       Get()            noexcept { return data; }
    const void* Get()      const noexcept { return data; }

public: // vvv Factory vvv
    template<typename T>
    static Any Create(T* ptr) noexcept
    {
        Any a{};
        a.data       = static_cast<void*>(ptr);
        a.destructor = [](void* p) { delete static_cast<T*>(p); };

        return a;
    }

    static Any FromRaw(void* ptr) noexcept
    {
        Any a{};
        a.data = ptr;
        a.destructor = nullptr;
        return a;
    }
};

static_assert(sizeof(Any) == 16,                      "WFX_Any ABI size mismatch");
static_assert(alignof(Any) == alignof(void*),         "WFX_Any alignment mismatch");
static_assert(std::is_standard_layout<Any>::value,    "WFX_Any must be standard layout");
static_assert(std::is_trivially_copyable<Any>::value, "WFX_Any must be trivially copyable");

} // namespace WFX::Shared

#endif // WFX_SHARED_ABI_ANY_HPP