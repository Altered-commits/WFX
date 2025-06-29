#include "http_serializer.hpp"

namespace WFX::Http {

std::string HttpSerializer::Serialize(HttpResponse& res)
{
    std::string out;
    out.reserve(HEADER_RESERVE_SIZE_HINT + res.body.size());

    // 1. HTTP version and status
    out.append("HTTP/1.");
    out.push_back(res.version == HttpVersion::HTTP_1_1 ? '1' : '0');
    out.append(" ");

    uint8_t code = static_cast<uint8_t>(res.status);
    
    // Codes are 3 digits, so we can just push_back 3 times the 3 digits lmao
    // This is actually faster than doing snprintf or std::to_string
    out.push_back('0' + code / 100);
    out.push_back('0' + (code / 10) % 10);
    out.push_back('0' + code % 10);

    out.append(" ");
    out.append(HttpStatusToReason(res.status));
    out.append("\r\n");

    // 2. Headers
    for(const auto& [key, value] : res.headers.GetHeaderMap()) {
        out.append(key);
        out.append(": ");
        out.append(value);
        out.append("\r\n");
    }

    // End headers
    out.append("\r\n");

    // Split
    // Its a file operation: Header only needed
    if(res.IsFileOperation())
        return out;

    // Its a text operation: Body is also needed
    out.append(res.body);

    return out;
}

} // namespace WFX::Http
