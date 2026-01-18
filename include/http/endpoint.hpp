#ifndef WFX_INC_HTTP_ENPOINT_HPP
#define WFX_INC_HTTP_ENPOINT_HPP

#include "core/core.hpp"

struct APIInterface {
    const char*   __Url       = nullptr;
    std::uint32_t __IntrnlIdx = 0;
};

struct LazyAPIInterface {
public: // Constructor
    constexpr LazyAPIInterface(const char* url) noexcept
    {
        value_.__Url = url;
    }

public: // Helper functions
    inline const APIInterface& Get() const noexcept
    {
        if(!init_) {
            value_.__IntrnlIdx = __WFXApi->GetHttpAPIV1()->AllocateEndpoint(value_.__Url);
            init_  = true;
        }
        return value_;
    }

    inline operator const APIInterface&() const noexcept
    {
        return Get();
    }

private:
    mutable APIInterface value_ = {};
    mutable bool         init_  = false;
};

#endif // WFX_INC_HTTP_ENPOINT_HPP