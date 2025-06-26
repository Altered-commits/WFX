#include "http_parser.hpp"

namespace WFX::Http {

bool HttpParser::Parse(ConnectionContext& ctx, HttpRequest& outRequest)
{
    // Sanity checks
    if(!ctx.buffer || ctx.dataLength == 0)
        return false;

    const char* data = ctx.buffer;
    std::size_t size = ctx.dataLength;
    std::size_t pos  = 0;

    if(!ParseRequest(data, size, pos, outRequest))
        return false;

    if(!ParseHeaders(data, size, pos, outRequest.headers))
        return false;

    if(!ParseBody(data, size, pos, outRequest))
        return false;

    return true;
}

// vvv Parse Helpers vvv
bool HttpParser::ParseRequest(const char* data, std::size_t size, std::size_t& pos, HttpRequest& outRequest)
{
    std::size_t nextPos = 0;
    std::string_view line;
    if(!SafeFindCRLF(data, size, pos, nextPos, line))
        return false;

    pos = nextPos;

    std::size_t mEnd = line.find(' ');
    if(mEnd == std::string_view::npos)
        return false;

    std::string_view methodStr = line.substr(0, mEnd);
    outRequest.method = ParseHttpMethod(methodStr);
    if(outRequest.method == HttpMethod::UNKNOWN)
        return false;

    std::size_t pathStart = mEnd + 1;
    std::size_t pathEnd   = line.find(' ', pathStart);
    if(pathEnd == std::string_view::npos)
        return false;

    outRequest.path = std::string_view(data + pathStart, pathEnd - pathStart);

    std::string_view versionStr = line.substr(pathEnd + 1);
    outRequest.version = ParseHttpVersion(versionStr);
    if(outRequest.version == HttpVersion::UNKNOWN)
        return false;

    return true;
}

bool HttpParser::ParseHeaders(const char* data, std::size_t size, std::size_t& pos, HttpHeaders& outHeaders)
{
    std::size_t headerCount      = 0;
    std::size_t headerTotalBytes = 0;
    std::size_t nextPos          = 0;
    std::string_view line;

    while(true) {
        if(!SafeFindCRLF(data, size, pos, nextPos, line))
            return false;

        std::size_t lineBytes = nextPos - pos;
        headerTotalBytes += lineBytes;
        if(headerTotalBytes > MAX_HEADER_TOTAL_SIZE)
            return false;

        pos = nextPos;

        if(line.empty())
            break;

        std::size_t colon = line.find(':');
        if(colon == std::string_view::npos || colon == 0)
            return false;

        std::string_view key = line.substr(0, colon);
        std::string_view val = Trim(line.substr(colon + 1));

        outHeaders.SetHeader(key, val);

        if(++headerCount > MAX_HEADERS_TOTAL_COUNT)
            return false;
    }

    return true;
}

bool HttpParser::ParseBody(const char* data, std::size_t size, std::size_t& pos, HttpRequest& outRequest)
{
    std::size_t contentLength = 0;

    if(outRequest.headers.HasHeader("Content-Length")) {
        std::string_view lenStr = outRequest.headers.GetHeader("Content-Length");
        
        auto [ptr, ec] = std::from_chars(lenStr.data(), lenStr.data() + lenStr.size(), contentLength);
        if(ec != std::errc{} || contentLength > MAX_BODY_TOTAL_SIZE)
            return false;

        if(pos + contentLength > size)
            return false;

        outRequest.body = std::string_view(data + pos, contentLength);
    }
    else
        outRequest.body = {};

    return true;
}

HttpMethod HttpParser::ParseHttpMethod(std::string_view method)
{
    if(method == "GET")  return HttpMethod::GET;
    if(method == "POST") return HttpMethod::POST;
    
    return HttpMethod::UNKNOWN;
}

HttpVersion HttpParser::ParseHttpVersion(std::string_view version)
{
    if(version == "HTTP/1.1") return HttpVersion::HTTP_1_1;
    if(version == "HTTP/1.0") return HttpVersion::HTTP_1_0;
    if(version == "HTTP/2.0") return HttpVersion::HTTP_2_0;
    
    return HttpVersion::UNKNOWN;
}

// vvv Helpers vvv
bool HttpParser::SafeFindCRLF(const char* data, std::size_t size, std::size_t from,
                                std::size_t& outNextPos, std::string_view& outLine)
{
    if(from >= size)
        return false;

    const char* start = data + from;
    const char* end   = static_cast<const char*>(memchr(start, '\r', size - from));
    if(!end || end + 1 >= data + size || *(end + 1) != '\n')
        return false;

    outNextPos = static_cast<std::size_t>(end - data) + 2;
    outLine    = std::string_view(start, static_cast<std::size_t>(end - start));
    return true;
}

std::string_view HttpParser::Trim(std::string_view sv)
{
    std::size_t start = 0;
    while(start < sv.size() && (sv[start] == ' ' || sv[start] == '\t'))
        ++start;

    std::size_t end = sv.size();
    while(end > start && (sv[end - 1] == ' ' || sv[end - 1] == '\t'))
        --end;

    return sv.substr(start, end - start);
}

} // WFX::Http