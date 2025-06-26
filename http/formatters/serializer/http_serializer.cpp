#include "http_serializer.hpp"

namespace WFX::Http {

std::string HttpSerializer::Serialize(HttpResponse& res)
{
    std::string out;
    out.reserve(HEADER_RESERVE_SIZE_HINT + res.body.size());

    char convBuf[24]; // For numeric conversions to strings

    // 1. HTTP version and status
    out.append("HTTP/1.");
    out += (res.version == HttpVersion::HTTP_1_1 ? '1' : '0');
    out.append(" ");

    // status code
    uint16_t code = static_cast<uint16_t>(res.status);
    int len = snprintf(convBuf, sizeof(convBuf), "%u", code);
    out.append(convBuf, len);

    out.append(" ");
    out.append(HttpStatusToReason(res.status));
    out.append("\r\n");

    // 2. Headers
    bool hasContentLength = false;
    for(const auto& [key, value] : res.headers.GetHeaderMap()) {
        out.append(key);
        out.append(": ");
        out.append(value);
        out.append("\r\n");

        if(!hasContentLength && key.size() == 14 &&
            std::equal(key.begin(), key.end(), "Content-Length", [](char a, char b) {
                return tolower(a) == tolower(b);
            }))
            hasContentLength = true;
    }

    // 3. Inject Content-Length if missing
    if(!hasContentLength) {
        out.append("Content-Length: ");

        int len = snprintf(convBuf, sizeof(convBuf), "%zu", res.body.size());

        out.append(convBuf, len);
        out.append("\r\n");
    }

    // 4. End headers
    out.append("\r\n");

    // 5. Append body
    out.append(res.body);

    return out;
}

} // namespace WFX::Http
