#include "http_connection.hpp"
#include "http/response/http_response.hpp"
#include "shared/apis/http_api.hpp"

namespace WFX::Http {

// vvv Ip Address Methods vvv
WFXIpAddress& WFXIpAddress::operator=(const WFXIpAddress& other)
{
    ipType = other.ipType;

    switch(ipType)
    {
        case AF_INET:
            memcpy(&ip.v4, &other.ip.v4, sizeof(in_addr));
            break;
        
        case AF_INET6:
            memcpy(&ip.v6, &other.ip.v6, sizeof(in6_addr));
            break;

        default:
            memset(&ip, 0, sizeof(ip));
            break;
    }

    return *this;
}

bool WFXIpAddress::operator==(const WFXIpAddress& other) const
{
    if(ipType != other.ipType)
        return false;

    return memcmp(ip.raw, other.ip.raw, ipType == AF_INET ? 4 : 16) == 0;
}

// Helper functions
std::string_view WFXIpAddress::GetIpStr() const
{
    // Use thread-local static buffer to avoid heap allocation
    thread_local char ipStrBuf[INET6_ADDRSTRLEN] = {};

    const void* addr = (ipType == AF_INET)
        ? static_cast<const void*>(&ip.v4)
        : static_cast<const void*>(&ip.v6);

    // Convert to printable form
    if(inet_ntop(ipType, addr, ipStrBuf, sizeof(ipStrBuf)))
        return std::string_view(ipStrBuf);

    return std::string_view("ip-malformed");
}

const char* WFXIpAddress::GetIpType() const
{
    return ipType == AF_INET ? "IPv4" : "IPv6";
}

// vvv Connection Context Methods vvv
void ConnectionContext::ResetContext()
{
    rwBuffer.ResetBuffer();
    coroStack.clear();
    
    if(requestInfo)  { delete requestInfo;  requestInfo  = nullptr; }
    if(responseInfo) { delete responseInfo; responseInfo = nullptr; }
    if(fileInfo)     { delete fileInfo;     fileInfo     = nullptr; }
    
    connectionState    = 0;
    isFileOperation    = 0;
    isStreamOperation  = 0;
    isShuttingDown     = 0;
    streamChunked      = 0;
    connInfo           = WFXIpAddress{};
    expectedBodyLength = 0;
    eventType          = EventType::EVENT_ACCEPT;
    parseState         = 0;
    trackBytes         = 0;
}

void ConnectionContext::ClearContext()
{
    rwBuffer.ClearBuffer();
    coroStack.clear();

    if(requestInfo)  requestInfo->ClearInfo();
    if(responseInfo) responseInfo->ClearInfo();
    if(fileInfo)     *fileInfo = FileInfo{};

    isFileOperation    = 0;
    isStreamOperation  = 0;
    streamChunked      = 0;
    expectedBodyLength = 0;
    trackBytes         = 0;
    // eventType          = EventType::EVENT_ACCEPT;
    // connInfo           = WFXIpAddress{};
    // timeoutTick        = 0;
    // parseState         = 0;
}

void ConnectionContext::SetParseState(HttpParseState newState)
{
    parseState = static_cast<std::uint8_t>(newState);
}

void ConnectionContext::SetConnectionState(ConnectionState newState)
{
    connectionState = static_cast<std::uint8_t>(newState);
}

HttpParseState ConnectionContext::GetParseState() const
{
    return static_cast<HttpParseState>(parseState);
}

ConnectionState ConnectionContext::GetConnectionState() const
{
    return static_cast<ConnectionState>(connectionState);
}

bool ConnectionContext::IsAsyncOperation()
{
    return !coroStack.empty();
}

bool ConnectionContext::TryFinishCoroutines()
{
    if(coroStack.empty())
        return true;

    // THE MOST IMPORTANT THING, ASYNC FUNCTIONS EXPECT US TO SET CTX (current connection context)-
    // -VIA HTTP API. AND WE WILL SET IT TO NULLPTR ONCE WE ARE DONE USING IT, WE DON'T WANT DANGLING-
    // -POINTERS
    auto httpApi = WFX::Shared::GetHttpAPIV1();
    httpApi->SetGlobalPtrData(this);

    // Execute from top to bottom, each of it is stateless
    // Now if it hasn't finished after resuming, break out of the loop, -
    // -do not try to finish rest of it as they depend on each other
    while(!coroStack.empty()) {
        auto& coro = coroStack.back();
        coro->Resume();

        if(coro->IsFinished())
            coroStack.pop_back(); // Remove finished coroutine
        else {
            httpApi->SetGlobalPtrData(nullptr);
            return false;
        }
    }

    // We finished all the stuff, signal caller to handle response creation now
    httpApi->SetGlobalPtrData(nullptr);
    return true;
}

} // namespace WFX::Http