#ifndef WFX_INC_HTTP_USER_RESPONSE_HPP
#define WFX_INC_HTTP_USER_RESPONSE_HPP

#include "shared/apis/http_api.hpp"
#include "third_party/json/json.hpp"
#include <cassert>

/* User side implementation of 'Response' class. CoreEngine passes the API */
class Response {
    using ResponsePtr = WFX::Http::HttpResponse*;
    using ApiPtr      = const WFX::Shared::HTTP_API_TABLE*;

public:
    Response(ResponsePtr backend, ApiPtr httpApi)
        : backend_(backend), httpApi_(httpApi)
    {
        assert(backend_ && httpApi_);
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
    void SendText(const char* cstr)  { httpApi_->SendTextCStr(backend_, cstr); }
    void SendText(std::string&& str) { httpApi_->SendTextMove(backend_, std::move(str)); }

    // SendJson overloads
    void SendJson(const Json& j) { httpApi_->SendJsonConstRef(backend_, &j); }

    // SendFile overloads
    void SendFile(const char* path, bool autoHandle404 = true)   { httpApi_->SendFileCStr(backend_, path, autoHandle404); }
    void SendFile(std::string&& path, bool autoHandle404 = true) { httpApi_->SendFileMove(backend_, std::move(path), autoHandle404); }

    // SendTemplate overloads
    void SendTemplate(const char* path, Json&& ctx = {})   { httpApi_->SendTemplateCStr(backend_, path, std::move(ctx)); }
    void SendTemplate(std::string&& path, Json&& ctx = {}) { httpApi_->SendTemplateMove(backend_, std::move(path), std::move(ctx)); }

    // Stream API
    void Stream(StreamGenerator generator, bool streamChunked = true) { httpApi_->Stream(backend_, std::move(generator), streamChunked); }

private:
    ResponsePtr backend_;
    ApiPtr      httpApi_;
};

#endif // WFX_INC_HTTP_USER_RESPONSE_HPP