#include "http_parser.hpp"

#include "config/config.hpp"
#include "utils/backport/string.hpp"
#include "utils/crypt/string.hpp"

namespace WFX::Http {

using namespace WFX::Utils; // For 'I have no idea'
using namespace WFX::Core; // For 'Config'

HttpParseState HttpParser::Parse(ConnectionContext* ctx)
{
    ReadMetadata* readMeta = ctx->rwBuffer.GetReadMeta();
    const char*   data     = ctx->rwBuffer.GetReadData();

    // Sanity checks
    if(!data || readMeta->dataLength == 0)
        return HttpParseState::PARSE_ERROR;

    // Config variables
    std::uint32_t maxBufferSize      = Config::GetInstance().networkConfig.maxRecvBufferSize;
    std::uint32_t maxBodyTotalSize   = Config::GetInstance().networkConfig.maxBodyTotalSize;
    std::uint32_t maxHeaderTotalSize = Config::GetInstance().networkConfig.maxHeaderTotalSize;

    // Connection Context variables
    std::uint32_t& trackBytes = ctx->trackBytes;
    std::size_t    size       = readMeta->dataLength;

    // Ensure requestInfo is allocated. If not, lazy initialize it
    if(!ctx->requestInfo)
        ctx->requestInfo = new HttpRequest{};

    HttpRequest& request = *ctx->requestInfo;

    // Our very cool State Machine handling different states of parser
    switch(static_cast<HttpParseState>(ctx->GetParseState()))
    {
        // In the case of it being idle, and some data arrives, we can safely fallthrough
        // As this only gets called if any data exists or arrives
        case HttpParseState::PARSE_IDLE:
            ctx->SetParseState(HttpParseState::PARSE_INCOMPLETE_HEADERS);
            [[fallthrough]];

        case HttpParseState::PARSE_INCOMPLETE_HEADERS:
        {
            std::size_t headerEnd = 0;
            // Even if we werent able to find header end, update trackBytes so we don't start reading-
            // -from beginning everytime.
            if(!SafeFindHeaderEnd(data, size, trackBytes, headerEnd)) {
                // Even if we haven't reached the end of header, check if the data we received exceeds-
                // -header limit. If it does, GG
                if(size > maxHeaderTotalSize)
                    return HttpParseState::PARSE_ERROR;

                trackBytes = size;
                return HttpParseState::PARSE_INCOMPLETE_HEADERS;
            }

            // We found the end of the headers. Now check if the total header size-
            // -(from 'GET /...' to '\r\n\r\n') exceeds the limit
            if(headerEnd > maxHeaderTotalSize)
                return HttpParseState::PARSE_ERROR;

            // Update trackBytes to match the headerEnd position
            trackBytes = headerEnd;

            std::size_t pos = 0;
            // Parsing of requests will be done from starting
            if(!ParseRequest(data, size, pos, request))
                return HttpParseState::PARSE_ERROR;

            if(!ParseHeaders(data, size, pos, request.headers))
                return HttpParseState::PARSE_ERROR;

            // Now we check the type, whether its streaming data or all at once kinda stuff or expect header
            auto expectHeader        = request.headers.GetHeader("Expect");
            auto contentLengthHeader = request.headers.GetHeader("Content-Length");
            auto encodingHeader      = request.headers.GetHeader("Transfer-Encoding");

            bool hasExpectHeader        = !expectHeader.empty() && StringCanonical::InsensitiveStringCompare(expectHeader, "100-continue");
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

                // Sanity check: are we about to exceed our max buffer size or max body size?
                // If so, reject oversized payload
                if(
                    contentLen > maxBodyTotalSize
                    || contentLen > maxBufferSize - 1
                    || headerEnd > maxBufferSize - 1 - contentLen
                ) {
                    // If client sent "Expect", reply with 417 else fail the response
                    if(hasExpectHeader)
                        return HttpParseState::PARSE_EXPECT_417;

                    return HttpParseState::PARSE_ERROR;
                }

                if(hasExpectHeader) {
                    // Set the state so the next time parser returns to this, it knows to parse body not header
                    ctx->SetParseState(HttpParseState::PARSE_INCOMPLETE_BODY);
                    return HttpParseState::PARSE_EXPECT_100;
                }

                // Body exists
                if(contentLen > 0) {
                    // Calc total body which recv got till now
                    std::size_t availableBody = size - trackBytes;
                    
                    // Still waiting for more body data
                    if(availableBody < contentLen) {
                        // In INCOMPLETE_BODY, this means: wait until ctx.dataLength >= trackBytes
                        trackBytes = headerEnd + contentLen;
                        ctx->expectedBodyLength = contentLen;
                        ctx->SetParseState(HttpParseState::PARSE_INCOMPLETE_BODY);
                        return HttpParseState::PARSE_INCOMPLETE_BODY;
                    }
                    
                    // Body is fully received, just parse body and return success
                    if(!ParseBody(data, size, pos, contentLen, request))
                        return HttpParseState::PARSE_ERROR;

                    ctx->SetParseState(HttpParseState::PARSE_SUCCESS);
                    return HttpParseState::PARSE_SUCCESS;
                }
                // No body, only header
                else {
                    ctx->SetParseState(HttpParseState::PARSE_SUCCESS);
                    return HttpParseState::PARSE_SUCCESS;
                }
            }

            // Data is chunked / gzip / whatever [future support]
            if(hasEncodingHeader) {
                // Only 'chunked' is supported for now
                if(!StringCanonical::InsensitiveStringCompare(encodingHeader, "chunked"))
                    return HttpParseState::PARSE_ERROR;

                // Parser will not try to buffer the full body, instead user will handle chunks
                ctx->SetParseState(HttpParseState::PARSE_STREAMING_BODY);
                
                if(hasExpectHeader)
                    return HttpParseState::PARSE_EXPECT_100;

                return HttpParseState::PARSE_STREAMING_BODY;
            }

            // We just assume it's a header only request
            ctx->SetParseState(HttpParseState::PARSE_SUCCESS);
            return HttpParseState::PARSE_SUCCESS;
        }

        case HttpParseState::PARSE_INCOMPLETE_BODY:
        {
            if(readMeta->dataLength < trackBytes)
                return HttpParseState::PARSE_INCOMPLETE_BODY;

            HttpRequest& request = *ctx->requestInfo;
            std::size_t pos = trackBytes - ctx->expectedBodyLength;

            if(!ParseBody(data, size, pos, ctx->expectedBodyLength, request))
                return HttpParseState::PARSE_ERROR;

            ctx->SetParseState(HttpParseState::PARSE_SUCCESS);
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
    if(!StringCanonical::NormalizeURIPathInplace(outRequest.path))
        return false;

    std::string_view versionStr = line.substr(pathEnd + 1);
    outRequest.version = HttpVersionToEnum(versionStr);
    if(outRequest.version == HttpVersion::UNKNOWN)
        return false;

    return true;
}

bool HttpParser::ParseHeaders(const char* data, std::size_t size, std::size_t& pos, RequestHeaders& outHeaders)
{
    std::size_t headerCount = 0;
    std::size_t nextPos     = 0;
    std::string_view line;

    auto& networkConfig = Config::GetInstance().networkConfig;

    while(true) {
        if(!SafeFindCRLF(data, size, pos, nextPos, line))
            return false;

        std::size_t lineBytes = nextPos - pos;
        pos = nextPos;

        if(line.empty())
            break;

        std::size_t colon = line.find(':');
        if(colon == std::string_view::npos || colon == 0)
            return false;

        std::string_view key = line.substr(0, colon);
        std::string_view val = Trim(line.substr(colon + 1));

        // Null-terminate key and value in-place, so even if we use pointer as is, its harmless
        char* writableKey = const_cast<char*>(data + (key.data() - data));
        writableKey[key.size()] = '\0';

        char* writableVal = const_cast<char*>(data + (val.data() - data));
        writableVal[val.size()] = '\0';

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

    outRequest.body = std::string_view{data + pos, contentLen};
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