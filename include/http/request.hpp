#ifndef WFX_INC_HTTP_USER_REQUEST_HPP
#define WFX_INC_HTTP_USER_REQUEST_HPP

#include "core/core.hpp"
#include "shared/apis/http_api.hpp"

namespace WFX::Http {

/* User side implementation of 'Request'. 'CoreEngine' passes the API */
class Request {
public:
    Request(void* backend) : backend_(backend) {}

public:
    HttpMethod Method() const
    {
        return __WFXApi->GetHttpAPIV1()->GetMethod(backend_);
    }

    HttpVersion Version() const
    {
        return __WFXApi->GetHttpAPIV1()->GetVersion(backend_);
    }

    std::string_view Path() const
    {
        auto sv = __WFXApi->GetHttpAPIV1()->GetPath(backend_);
        return { sv.data, static_cast<std::size_t>(sv.length) };
    }

    std::string_view Body() const
    {
        auto sv = __WFXApi->GetHttpAPIV1()->GetBody(backend_);
        return { sv.data, static_cast<std::size_t>(sv.length) };
    }

public:
    bool GetHeader(std::string_view key, std::string_view& out) const
    {
        Shared::StringView k = ToSV(key);
        Shared::StringView val{};

        bool ok = __WFXApi->GetHttpAPIV1()->GetHeader(backend_, k, &val);
        if(!ok)
            return false;

        out = { val.data, static_cast<std::size_t>(val.length) };
        return true;
    }

public:
    void SetContext(std::string_view key, Shared::Any value)
    {
        auto k = ToSV(key);
        __WFXApi->GetHttpAPIV1()->SetContext(backend_, k, value);
    }

    bool GetContext(std::string_view key, Shared::Any& out) const
    {
        auto k = ToSV(key);
        return __WFXApi->GetHttpAPIV1()->GetContext(backend_, k, &out);
    }

    void EraseContext(std::string_view key)
    {
        auto k = ToSV(key);
        __WFXApi->GetHttpAPIV1()->EraseContext(backend_, k);
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