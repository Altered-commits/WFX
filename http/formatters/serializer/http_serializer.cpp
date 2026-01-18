#include "http_serializer.hpp"

#include "config/config.hpp"
#include "utils/crypt/string.hpp"
#include "utils/fileops/filesystem.hpp"

namespace WFX::Http {

namespace HttpSerializer {

SerializedHttpResponse SerializeToBuffer(HttpResponse& res, RWBuffer& buffer)
{
    auto& networkConfig = WFX::Core::Config::GetInstance().networkConfig;

    // Ensure write buffer is initialized
    if(!buffer.IsWriteInitialized() && !buffer.InitWriteBuffer(networkConfig.maxSendBufferSize))
        return {SerializeResult::SERIALIZE_BUFFER_FAILED, {}};

    auto* meta = buffer.GetWriteMeta();
    if(!meta)
        return {SerializeResult::SERIALIZE_BUFFER_FAILED, {}};

    // We use std::string_view to ref the data inside the variant without copying it
    std::string_view bodyView = std::visit([](auto&& val) -> std::string_view {
        using T = std::decay_t<decltype(val)>;
        if constexpr(std::is_same_v<T, std::string_view>)
            return val;
        else if constexpr(std::is_same_v<T, std::string>)
            return std::string_view(val);
        else
            return {}; // std::monostate, StreamGenerator
    }, res.body);

    // Calculate total header size
    std::string_view reason = HttpStatusToReason(res.status);
    std::size_t headerSize = 9 + 3 + 1 + reason.size() + 2; 

    // Headers: "Key: Value\r\n"
    for(const auto& [k, v] : res.headers.GetHeaderMap())
        headerSize += k.size() + 2 + v.size() + 2;
    // Final "\r\n"
    headerSize += 2;

    // Determine Total Size needed in buffer
    bool        includeBody = !res.IsFileOperation() && !res.IsStreamOperation();
    std::size_t totalSize   = headerSize + (includeBody ? bodyView.size() : 0);

    if(totalSize > meta->bufferSize)
        return {SerializeResult::SERIALIZE_BUFFER_TOO_SMALL, {}};

    // Serialize the response
    buffer.AppendData("HTTP/1.", 7);
    buffer.AppendData(res.version == HttpVersion::HTTP_1_1 ? "1 " : "0 ", 2);
    
    std::uint16_t code = static_cast<std::uint16_t>(res.status);
    char codeStr[4];
    codeStr[0] = '0' + (code / 100);
    codeStr[1] = '0' + ((code / 10) % 10);
    codeStr[2] = '0' + (code % 10);
    codeStr[3] = ' ';
    buffer.AppendData(codeStr, 4);
    
    buffer.AppendData(reason.data(), static_cast<uint32_t>(reason.size()));
    buffer.AppendData("\r\n", 2);

    // Headers
    for(const auto& [k, v] : res.headers.GetHeaderMap()) {
        buffer.AppendData(k.data(), static_cast<uint32_t>(k.size()));
        buffer.AppendData(": ", 2);
        buffer.AppendData(v.data(), static_cast<uint32_t>(v.size()));
        buffer.AppendData("\r\n", 2);
    }

    buffer.AppendData("\r\n", 2);

    if(includeBody && !bodyView.empty()) {
        buffer.AppendData(bodyView.data(), static_cast<uint32_t>(bodyView.size()));
        return {SerializeResult::SERIALIZE_SUCCESS, {}};
    }

    if(!includeBody)
        return {SerializeResult::SERIALIZE_SUCCESS, std::string(bodyView)};

    return {SerializeResult::SERIALIZE_SUCCESS, {}};
}

} // namespace HttpSerializer

} // namespace WFX::Http