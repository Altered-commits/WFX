#ifndef WFX_INC_HTTP_USER_RESPONSE_HPP
#define WFX_INC_HTTP_USER_RESPONSE_HPP

#include "core/core.hpp"
#include "shared/apis/http_api.hpp"
#include "third_party/json/json.hpp"

using WFX::Shared::StreamGenerator;

/* User side implementation of 'Response' class. CoreEngine passes the API */
class Response {
    using ResponsePtr = WFX::Http::HttpResponse*;

public:
    Response(ResponsePtr backend)
        : backend_(backend)
    {}

    Response& Status(WFX::Http::HttpStatus code)
    {
        __WFXApi->GetHttpAPIV1()->SetStatus(backend_, code);
        return *this;
    }

    Response& Set(std::string key, std::string value)
    {
        __WFXApi->GetHttpAPIV1()->SetHeader(backend_, std::move(key), std::move(value));
        return *this;
    }

    // SendText overloads
    void SendText(const char* cstr)
    {
        __WFXApi->GetHttpAPIV1()->SendTextCStr(backend_, cstr);
    }
    void SendText(std::string&& str)
    {
        __WFXApi->GetHttpAPIV1()->SendTextMove(backend_, std::move(str));
    }

    // SendJson overloads
    void SendJson(const Json& j)
    {
        __WFXApi->GetHttpAPIV1()->SendJsonConstRef(backend_, &j);
    }

    // SendFile overloads
    void SendFile(const char* path, bool autoHandle404 = true)
    {
        __WFXApi->GetHttpAPIV1()->SendFileCStr(backend_, path, autoHandle404);
    }
    void SendFile(std::string&& path, bool autoHandle404 = true)
    {
        __WFXApi->GetHttpAPIV1()->SendFileMove(backend_, std::move(path), autoHandle404);
    }

    // SendTemplate overloads
    void SendTemplate(const char* path, Json&& ctx = {})
    {
        __WFXApi->GetHttpAPIV1()->SendTemplateCStr(backend_, path, std::move(ctx));
    }
    void SendTemplate(std::string&& path, Json&& ctx = {})
    {
        __WFXApi->GetHttpAPIV1()->SendTemplateMove(backend_, std::move(path), std::move(ctx));
    }

    // Stream API
    void Stream(StreamGenerator generator, bool streamChunked = true)
    {
        __WFXApi->GetHttpAPIV1()->Stream(backend_, std::move(generator), streamChunked);
    }

private:
    ResponsePtr backend_;
};

#endif // WFX_INC_HTTP_USER_RESPONSE_HPP