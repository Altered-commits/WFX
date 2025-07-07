#include "http_parser.hpp"

namespace WFX::Http {

using namespace WFX::Utils;

HttpParseState HttpParser::Parse(ConnectionContext& ctx)
{
    // Sanity checks
    if(!ctx.buffer || ctx.dataLength == 0)
        return HttpParseState::PARSE_ERROR;

    // Config variables
    std::uint32_t maxBufferSize = Config::GetInstance().networkConfig.maxRecvBufferSize;
    std::uint32_t maxBodySize   = Config::GetInstance().networkConfig.maxBodyTotalSize;

    // Ctx variabled
    std::uint8_t&  state      = ctx.parseState;
    std::uint32_t& trackBytes = ctx.trackBytes;
    const char*    data       = ctx.buffer;
    std::size_t    size       = ctx.dataLength;

    // Ensure requestInfo is allocated. If not, lazy initialize it
    if(!ctx.requestInfo)
        ctx.requestInfo = std::make_unique<HttpRequest>();

    HttpRequest& request = *ctx.requestInfo;

    // Our very cool State Machine handling different states of parser
    switch(static_cast<HttpParseState>(state))
    {
        case HttpParseState::PARSE_INCOMPLETE_HEADERS:
        {
            std::size_t headerEnd = 0;
            // Even if we werent able to fin header end, update trackBytes so we don't start reading-
            // -from beginning everytime.
            if(!SafeFindHeaderEnd(data, size, trackBytes, headerEnd)) {
                trackBytes = size;
                return HttpParseState::PARSE_INCOMPLETE_HEADERS;
            }

            // Update trackBytes to match the headerEnd position
            trackBytes = headerEnd;

            std::size_t pos = 0;
            // Parsing of requests will be done from starting
            if(!ParseRequest(data, size, pos, request))
                return HttpParseState::PARSE_ERROR;

            if(!ParseHeaders(data, size, pos, request.headers))
                return HttpParseState::PARSE_ERROR;

            // We also need to update connection alive status
            // If we do not find the header, we will assume client wants to close
            auto conn = request.headers.GetHeader("Connection");
            if(conn.empty() || CaseInsensitiveCompare(conn, "close"))
                ctx.shouldClose = true;
            
            // Now we check the type, whether its streaming data or all at once kinda stuff or expect header
            auto expectHeader        = request.headers.GetHeader("Expect");
            auto contentLengthHeader = request.headers.GetHeader("Content-Length");
            auto encodingHeader      = request.headers.GetHeader("Transfer-Encoding");

            bool hasExpectHeader        = !expectHeader.empty() && CaseInsensitiveCompare(expectHeader, "100-continue");
            bool hasContentLengthHeader = !contentLengthHeader.empty();
            bool hasEncodingHeader      = !encodingHeader.empty();
            
            // RFC Spec Violation: Both headers cannot be present at the same time
            if(hasEncodingHeader && hasContentLengthHeader)
                return HttpParseState::PARSE_ERROR;

            // Handle invalid Expect case: Expect present but no body indication
            if(hasExpectHeader && !hasContentLengthHeader && !hasEncodingHeader)
                return HttpParseState::PARSE_EXPECT_417;
            
            // Data should be fetched all at once
            if(hasContentLengthHeader) {
                std::size_t contentLen = 0;
                // Malformed Content-Length
                if(!StrToUInt64(contentLengthHeader, contentLen))
                    return HttpParseState::PARSE_ERROR;

                if(hasExpectHeader) {
                    if(contentLen > maxBodySize)
                        return HttpParseState::PARSE_EXPECT_417;

                    // Set the state so the next time parser returns to this, it knows to parse body not header
                    state = static_cast<std::uint8_t>(HttpParseState::PARSE_INCOMPLETE_BODY);
                    return HttpParseState::PARSE_EXPECT_100;
                }

                // Body exists
                if(contentLen > 0) {
                    // Sanity check: are we about to exceed our max buffer size? Reject oversized payload
                    if(contentLen > maxBufferSize - 1 || headerEnd > maxBufferSize - 1 - contentLen)
                        return HttpParseState::PARSE_ERROR;

                    // Calc total body which recv got till now
                    std::size_t availableBody = size - trackBytes;
                    
                    // Still waiting for more body data
                    if(availableBody < contentLen) {
                        // In INCOMPLETE_BODY, this means: wait until ctx.dataLength >= trackBytes
                        trackBytes = headerEnd + contentLen;
                        ctx.expectedBodyLength = contentLen;
                        state = static_cast<std::uint8_t>(HttpParseState::PARSE_INCOMPLETE_BODY);
                        return HttpParseState::PARSE_INCOMPLETE_BODY;
                    }
                    
                    // Body is fully received, just parse body and return success
                    if(!ParseBody(data, size, pos, contentLen, request))
                        return HttpParseState::PARSE_ERROR;

                    state = static_cast<std::uint8_t>(HttpParseState::PARSE_SUCCESS);
                    return HttpParseState::PARSE_SUCCESS;
                }
                // No body, only header
                else {
                    state = static_cast<std::uint8_t>(HttpParseState::PARSE_SUCCESS);
                    return HttpParseState::PARSE_SUCCESS;
                }
            }

            // Data is chunked / gzip / whatever [future support]
            if(hasEncodingHeader) {
                // Only 'chunked' is supported for now
                if(!CaseInsensitiveCompare(encodingHeader, "chunked"))
                    return HttpParseState::PARSE_ERROR;

                // Parser will not try to buffer the full body – instead user will handle chunks
                state = static_cast<std::uint8_t>(HttpParseState::PARSE_STREAMING_BODY);
                
                if(hasExpectHeader)
                    return HttpParseState::PARSE_EXPECT_100;

                return HttpParseState::PARSE_STREAMING_BODY;
            }

            // Neither encoding header nor content length header is present
            // This is valid in RFC spec under the conditions the request type isn't POST/PUT
            // We only support POST so yeah
            if(request.method == HttpMethod::POST)
                return HttpParseState::PARSE_ERROR;
            
            // We just assume it's a header only request
            state = static_cast<std::uint8_t>(HttpParseState::PARSE_SUCCESS);
            return HttpParseState::PARSE_SUCCESS;
        }
        
        case HttpParseState::PARSE_INCOMPLETE_BODY:
        {
            if(ctx.dataLength < trackBytes)
                return HttpParseState::PARSE_INCOMPLETE_BODY;

            HttpRequest& request = *ctx.requestInfo;
            std::size_t pos = trackBytes - ctx.expectedBodyLength;

            if(!ParseBody(data, size, pos, ctx.expectedBodyLength, request))
                return HttpParseState::PARSE_ERROR;

            state = static_cast<std::uint8_t>(HttpParseState::PARSE_SUCCESS);
            return HttpParseState::PARSE_SUCCESS;
        }
        
        // Not implemented [future]
        case HttpParseState::PARSE_STREAMING_BODY:
            return HttpParseState::PARSE_STREAMING_BODY;
        
        case HttpParseState::PARSE_SUCCESS:
            return HttpParseState::PARSE_SUCCESS;

        default:
            return HttpParseState::PARSE_ERROR;
    }
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
    outRequest.method = HttpMethodToEnum(methodStr);
    if(outRequest.method == HttpMethod::UNKNOWN)
        return false;

    std::size_t pathStart = mEnd + 1;
    std::size_t pathEnd   = line.find(' ', pathStart);
    if(pathEnd == std::string_view::npos || pathEnd == pathStart)
        return false;

    outRequest.path = std::string_view(data + pathStart, pathEnd - pathStart);

    // Normalize the path, reject if its malformed
    if(!NormalizeURIPathInplace(outRequest.path))
        return false;

    std::string_view versionStr = line.substr(pathEnd + 1);
    outRequest.version = HttpVersionToEnum(versionStr);
    if(outRequest.version == HttpVersion::UNKNOWN)
        return false;

    return true;
}

bool HttpParser::ParseHeaders(const char* data, std::size_t size, std::size_t& pos, RequestHeaders& outHeaders)
{
    std::size_t headerCount      = 0;
    std::size_t headerTotalBytes = 0;
    std::size_t nextPos          = 0;
    std::string_view line;

    auto& networkConfig = Config::GetInstance().networkConfig;

    while(true) {
        if(!SafeFindCRLF(data, size, pos, nextPos, line))
            return false;

        std::size_t lineBytes = nextPos - pos;
        headerTotalBytes += lineBytes;
        if(headerTotalBytes > networkConfig.maxHeaderTotalSize)
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

        if(++headerCount > networkConfig.maxHeaderTotalCount)
            return false;
    }

    return true;
}

bool HttpParser::ParseBody(const char* data, std::size_t size, std::size_t& pos, std::size_t contentLen, HttpRequest& outRequest)
{
    // Overflow check: position + contentLen must be within bounds
    if(pos > size || contentLen > size || pos + contentLen > size)
        return false;
    
    // Max body size constraint
    if(contentLen > Config::GetInstance().networkConfig.maxBodyTotalSize)
        return false;

    outRequest.body = std::string_view(data + pos, contentLen);
    pos += contentLen;

    return true;
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

bool HttpParser::SafeFindHeaderEnd(const char* data, std::size_t size, std::size_t from, std::size_t& outPos)
{
    if(size < 4 || from >= size - 3)
        return false;

    const char* start = data + from;
    const char* end   = data + size - 3;

    // I trust compiler to optimize this loop :)
    for(const char* p = start; p < end; ++p) {
        if(p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n') {
            outPos = static_cast<std::size_t>(p - data) + 4;
            return true;
        }
    }

    return false;
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