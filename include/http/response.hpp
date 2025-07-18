#ifndef WFX_HTTP_USER_RESPONSE_HPP
#define WFX_HTTP_USER_RESPONSE_HPP

#include "shared/apis/master_api.hpp"

// Forward declare HttpResponse
namespace WFX::Http {
    struct HttpResponse;
}

// Declare the injected API table
extern const WFX::Shared::MASTER_API_TABLE* __wfx_api;

/* User side implementation of 'Response' class via __wfx_api */
class Response {
public:
    explicit Response(WFX::Http::HttpResponse* ptr)
        : backend_(ptr)
    {}

    // Response& Status(int code)
    // {
    //     __wfx_api->SetStatus(backend_, code);
    //     return *this;
    // }

    // Response& Set(std::string_view key, std::string_view value)
    // {
    //     __wfx_api->SetHeader(backend_, key.data(), value.data());
    //     return *this;
    // }

    // void SendText(const char* cstr)
    // {
    //     __wfx_api->SendTextCStr(backend_, cstr);
    // }

    // void sendText(std::string_view view)
    // {
    //     __wfx_api->SendTextViewStr(backend_, view);
    // }

    // void sendJson(const Json& j)
    // {
    //     __wfx_api->SendJsonConstRef(backend_, &j);
    // }

    // void SendFile(const char* path)
    // {
    //     __wfx_api->SendFileCStr(backend_, path);
    // }

    // void SendFile(std::string_view path)
    // {
    //     __wfx_api->SendFileViewStr(backend_, path);
    // }

    // bool isFileOperation() const
    // {
    //     return __wfx_api->IsFileOperation(backend_);
    // }

private:
    WFX::Http::HttpResponse* backend_;
};

#endif // WFX_HTTP_USER_RESPONSE_HPP