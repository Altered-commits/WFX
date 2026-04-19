#ifndef WFX_INC_HTTP_USER_REQUEST_HPP
#define WFX_INC_HTTP_USER_REQUEST_HPP

#include "core/core.hpp"
#include "shared/apis/http_api.hpp"
#include <string_view>
#include <cstring>

namespace WFX::Http {

/* User side implementation of 'Request'. 'CoreEngine' passes the API */
class Request {
public:
    Request(void* backend) : backend_(backend) {}

public:
    Shared::HttpMethod Method() const
    {
        return Core::HttpApi()->GetMethod(backend_);
    }

    Shared::HttpVersion Version() const
    {
        return Core::HttpApi()->GetVersion(backend_);
    }

    std::string_view Path() const
    {
        auto sv = Core::HttpApi()->GetPath(backend_);
        return { sv.data, static_cast<std::size_t>(sv.length) };
    }

    std::string_view Body() const
    {
        auto sv = Core::HttpApi()->GetBody(backend_);
        return { sv.data, static_cast<std::size_t>(sv.length) };
    }

public:
    bool GetHeader(std::string_view key, std::string_view& out) const
    {
        Shared::StringView k = ToSV(key);
        Shared::StringView val{};

        bool ok = Core::HttpApi()->GetHeader(backend_, k, &val);
        if(!ok)
            return false;

        out = { val.data, static_cast<std::size_t>(val.length) };
        return true;
    }

public:
    std::uint64_t SegmentCount() const
    {
        return Core::HttpApi()->GetSegmentCount(backend_);
    }

    Shared::SegmentVariant GetSegment(std::uint64_t index) const
    {
        return Core::HttpApi()->GetSegment(backend_, index);
    }

public:
    template<typename T>
    bool SetContext(std::string_view key, T&& value)
    {
        using U = std::decay_t<T>;

        auto k = ToSV(key);

        Shared::Any any{};
        any.typeID = Shared::Any::TypeIDOf<U>();

        if constexpr (
            sizeof(U) <= sizeof(void*)
            && alignof(U) <= alignof(void*)
            && std::is_trivially_copyable_v<U>
            && std::is_trivially_destructible_v<U>
        ) {
            U tmp = std::forward<T>(value);
            std::memcpy(&any.data, &tmp, sizeof(U));
            any.destructor = nullptr;
        }
        else {
            void* mem = Core::MemoryApi()->Alloc(sizeof(U));
            if(!mem)
                return false;

            U* obj = new(mem) U(std::forward<T>(value));
            any.data = obj;

            any.destructor = [](void* p) {
                static_cast<U*>(p)->~U();
                Core::MemoryApi()->Free(p);
            };
        }

        Core::HttpApi()->SetContext(backend_, k, any);
        return true;
    }

    template<typename T>
    auto GetContext(std::string_view key) const
    {
        using U = std::decay_t<T>;

        auto k = ToSV(key);
        Shared::Any any{};

        // For trivial types, return val + bool
        if constexpr (
            sizeof(U) <= sizeof(void*)
            && alignof(U) <= alignof(void*)
            && std::is_trivially_copyable_v<U>
            && std::is_trivially_destructible_v<U>
        ) {
            if(
                !Core::HttpApi()->GetContext(backend_, k, &any)
                || any.typeID != Shared::Any::TypeIDOf<U>()
            )
                return std::pair<U, bool>{{}, false};

            U out{};
            std::memcpy(&out, &any.data, sizeof(U));

            return std::pair<U, bool>{out, true};
        }
        // For non-trivial types, we return ptr + bool
        else {
            if(!Core::HttpApi()->GetContext(backend_, k, &any))
                return std::pair<U*, bool>{nullptr, false};

            return std::pair<U*, bool>{any.As<U>(), true};
        }
    }

    void EraseContext(std::string_view key)
    {
        auto k = ToSV(key);
        Core::HttpApi()->EraseContext(backend_, k);
    }

private:
    static Shared::StringView ToSV(std::string_view s)
    {
        return { s.data(), static_cast<std::uint64_t>(s.size()) };
    }

private:
    void* backend_;
};

} // namespace WFX::Http

#endif // WFX_INC_HTTP_USER_REQUEST_HPP