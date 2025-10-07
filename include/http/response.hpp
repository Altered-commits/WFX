#ifndef WFX_INC_HTTP_USER_RESPONSE_HPP
#define WFX_INC_HTTP_USER_RESPONSE_HPP

#include "shared/apis/http_api.hpp"
#include "shared/apis/config_api.hpp"
#include <cassert>

// Forward declare HttpResponse
namespace WFX::Http {
    struct HttpResponse;
}

/* User side implementation of 'Response' class. CoreEngine passes the API */
class Response {
public:
    Response(WFX::Http::HttpResponse* backend,
        const WFX::Shared::HTTP_API_TABLE* httpApi,
        const WFX::Shared::CONFIG_API_TABLE* configApi)
        : backend_(backend), httpApi_(httpApi), configApi_(configApi)
    {
        assert(httpApi_ && configApi_);
    }

    Response& Status(WFX::Http::HttpStatus code)
    {
        httpApi_->SetStatus(backend_, code);
        return *this;
    }

    Response& Set(std::string key, std::string value)
    {
        httpApi_->SetHeader(backend_, std::move(key), std::move(value));
        return *this;
    }

    // SendText overloads
    void SendText(const char* cstr)      { httpApi_->SendTextCStr(backend_, cstr); }
    void SendText(std::string&& str)     { httpApi_->SendTextMove(backend_, std::move(str)); }

    // SendJson overloads
    void SendJson(const Json& j)         { httpApi_->SendJsonConstRef(backend_, &j); }
    void SendJson(Json&& j)              { httpApi_->SendJsonMove(backend_, std::move(j)); }

    // SendFile overloads
    void SendFile(const char* path, bool autoHandle404 = true)   { httpApi_->SendFileCStr(backend_, path, autoHandle404); }
    void SendFile(std::string&& path, bool autoHandle404 = true) { httpApi_->SendFileMove(backend_, std::move(path), autoHandle404); }

    // SendTemplate overloads
    void SendTemplate(const char* path, bool autoHandle404 = true) { SendTemplate(std::string{path}, autoHandle404); }
    void SendTemplate(std::string&& path, bool autoHandle404 = true)
    {
        auto& templateDir = configApi_->GetConfig().projectConfig.templateDir;
        SendFile(templateDir + '/' + path, autoHandle404);
    }

private:
    WFX::Http::HttpResponse*             backend_;
    const WFX::Shared::CONFIG_API_TABLE* configApi_;
    const WFX::Shared::HTTP_API_TABLE*   httpApi_;
};

#endif // WFX_INC_HTTP_USER_RESPONSE_HPP