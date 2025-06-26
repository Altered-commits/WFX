#include "http_response.hpp"

namespace WFX::Http {

// vvv Setters vvv
HttpResponse& HttpResponse::Status(HttpStatus code)
{
    status = code;
    return *this;
}

HttpResponse& HttpResponse::Set(std::string_view key, std::string_view value)
{
    headers.SetHeader(key, value);
    return *this;
}

void HttpResponse::Body(std::string_view body_)
{
    body = body_;
}

// vvv Senders vvv
void HttpResponse::SendText(std::string_view text)
{
    headers.SetHeader("Content-Type", "text/plain");
    body = text;
}

void HttpResponse::SendJson(const Json& json)
{
    headers.SetHeader("Content-Type", "application/json");
    body = json.dump();
}

void HttpResponse::SendHtml(std::string_view path)
{
    headers.SetHeader("Content-Type", "text/html");
    body = path;
}

} // namespace WFX::Http
