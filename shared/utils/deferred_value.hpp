// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_UTILS_DEFERRED_VALUE_HPP
#define WFX_SHARED_UTILS_DEFERRED_VALUE_HPP

#include <new>
#include <utility>

namespace WFX::Shared {

// Holds a T that doesn't exist yet. For a slot that's declared before the value it will hold is
// known, e.g. a coroutine promise's return storage, where T might not even have a default
// constructor. Raw aligned storage plus placement new, same technique std::optional uses
// internally, just without the extra machinery we don't need here.
template <typename T> struct DeferredValue {
public: // Constructor and Destructor
    DeferredValue() noexcept = default;

    ~DeferredValue()
    {
        if(hasValue_)
            Ptr()->~T();
    }

    // No move or copy constructor
    DeferredValue(const DeferredValue&) = delete;
    DeferredValue& operator=(const DeferredValue&) = delete;
    DeferredValue(DeferredValue&&) = delete;
    DeferredValue& operator=(DeferredValue&&) = delete;

public: // Value Access
    template <typename... Args> void Emplace(Args&&... args)
    {
        if(hasValue_)
            Ptr()->~T();

        ::new (static_cast<void*>(storage_)) T(std::forward<Args>(args)...);
        hasValue_ = true;
    }

    T& Get() noexcept
    {
        return *Ptr();
    }
    const T& Get() const noexcept
    {
        return *Ptr();
    }

    operator T&() noexcept
    {
        return Get();
    }
    operator const T&() const noexcept
    {
        return Get();
    }

private: // Helpers
    T* Ptr() noexcept
    {
        return std::launder(reinterpret_cast<T*>(storage_));
    }
    const T* Ptr() const noexcept
    {
        return std::launder(reinterpret_cast<const T*>(storage_));
    }

private: // Storage
    alignas(T) unsigned char storage_[sizeof(T)];
    bool hasValue_ = false;
};

} // namespace WFX::Shared

#endif // WFX_SHARED_UTILS_DEFERRED_VALUE_HPP
