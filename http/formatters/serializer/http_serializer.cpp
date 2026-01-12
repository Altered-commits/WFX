#include "http_serializer.hpp"

#include "config/config.hpp"
#include "utils/crypt/string.hpp"
#include "utils/fileops/filesystem.hpp"

namespace WFX::Http {

SerializedHttpResponseDeprecated HttpSerializer::Serialize(HttpResponse& res)
{
    // WTF
    auto headerSizeHint = WFX::Core::Config::GetInstance().networkConfig.headerReserveHintSize;

    const std::string_view bodyView = std::visit([](const auto& val) -> std::string_view {
        using T = std::decay_t<decltype(val)>;

        if constexpr(std::is_same_v<T, std::monostate> || std::is_same_v<T, StreamGenerator>)
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

SerializedHttpResponse HttpSerializer::SerializeToBuffer(HttpResponse& res, RWBuffer& buffer)
{
    auto& networkConfig = WFX::Core::Config::GetInstance().networkConfig;

    // Ensure write buffer is initialized
    if(!buffer.IsWriteInitialized() && !buffer.InitWriteBuffer(networkConfig.maxSendBufferSize))
        return {SerializeResult::SERIALIZE_BUFFER_FAILED, {}};

    auto* meta = buffer.GetWriteMeta();
    if(!meta) return {SerializeResult::SERIALIZE_BUFFER_FAILED, {}};

    std::string bodyView = std::visit([](auto&& val) -> std::string {
        using T = std::decay_t<decltype(val)>;

        if constexpr(std::is_same_v<T, std::monostate> || std::is_same_v<T, StreamGenerator>)
            return {};
        else if constexpr(std::is_same_v<T, std::string>)
            return std::move(val); 
        else
            return std::string(val);
    }, res.body);
    
    std::string out;
    out.reserve(networkConfig.headerReserveHintSize + (res.IsFileOperation() ? 0 : bodyView.size()));

    // Minimal version of Serialize(), but into a string first
    out.append("HTTP/1.");
    out.push_back(res.version == HttpVersion::HTTP_1_1 ? '1' : '0');
    out.push_back(' ');

    std::uint16_t code = static_cast<std::uint16_t>(res.status);
    out.push_back('0' + code / 100);
    out.push_back('0' + (code / 10) % 10);
    out.push_back('0' + code % 10);
    out.push_back(' ');
    out.append(HttpStatusToReason(res.status));
    out.append("\r\n");

    for(const auto& [k, v] : res.headers.GetHeaderMap())
        out.append(k).append(": ").append(v).append("\r\n");

    out.append("\r\n");

    // Buffer is so small that even headers don't fit
    if(out.size() > meta->bufferSize)
        return {SerializeResult::SERIALIZE_BUFFER_TOO_SMALL, {}};
    
    // Buffer exists, but not enough space right now
    if(!buffer.AppendData(out.data(), static_cast<std::uint32_t>(out.size())))
        return {SerializeResult::SERIALIZE_BUFFER_INSUFFICIENT, {}};

    // File / Stream operation: headers only
    if(res.IsFileOperation() || res.IsStreamOperation())
        return {SerializeResult::SERIALIZE_SUCCESS, bodyView};

    // Text operation: append body
    if(!bodyView.empty()) {
        // Body alone is larger than buffer, cannot ever fit
        if(bodyView.size() > meta->bufferSize)
            return {SerializeResult::SERIALIZE_BUFFER_TOO_SMALL, {}};

        // Body didn’t fit in current space, but could fit after flush
        if(!buffer.AppendData(bodyView.data(), static_cast<std::uint32_t>(bodyView.size())))
            return {SerializeResult::SERIALIZE_BUFFER_INSUFFICIENT, {}};
    }

    return {SerializeResult::SERIALIZE_SUCCESS, {}};
}

} // namespace WFX::Http