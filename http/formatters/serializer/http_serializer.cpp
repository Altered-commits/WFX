#include "http_serializer.hpp"

#include "config/config.hpp"
#include "utils/crypt/string.hpp"
#include "utils/filesystem/filesystem.hpp"

namespace WFX::Http {

SerializedHttpResponse HttpSerializer::Serialize(HttpResponse& res)
{
    // WTF
    auto headerSizeHint = WFX::Core::Config::GetInstance().networkConfig.headerReserveHintSize;

    const std::string_view bodyView = std::visit([](const auto& val) -> std::string_view {
        using T = std::decay_t<decltype(val)>;

        if constexpr(std::is_same_v<T, std::monostate>)
            return {};
        else
            return val;
    }, res.body);
    
    std::string out;
    out.reserve(headerSizeHint + (res.IsFileOperation() ? 0 : bodyView.size()));

    // 1. HTTP version and status
    out.append("HTTP/1.");
    out.push_back(res.version == HttpVersion::HTTP_1_1 ? '1' : '0');
    out.push_back(' ');

    std::uint16_t code = static_cast<std::uint16_t>(res.status);
    
    // Codes are 3 digits, so we can just push_back 3 times the 3 digits lmao
    // This is actually faster than doing snprintf or std::to_string
    out.push_back('0' + code / 100);
    out.push_back('0' + (code / 10) % 10);
    out.push_back('0' + code % 10);

    out.push_back(' ');
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
        return std::make_pair(std::move(out), bodyView);

    // Its a text operation: Body is also needed
    out.append(bodyView);

    return std::make_pair(std::move(out), std::string_view{});
}

} // namespace WFX::Http
