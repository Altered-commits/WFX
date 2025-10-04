#include "http_connection.hpp"

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
    
    if(requestInfo) {
        delete requestInfo;
        requestInfo = nullptr;
    }

    if(fileInfo) {
        delete fileInfo;
        fileInfo = nullptr;
    }
    
    connectionState    = 0;
    isFileOperation    = 0;
    isShuttingDown     = 0;
    connInfo           = WFXIpAddress{};
    expectedBodyLength = 0;
    eventType          = EventType::EVENT_ACCEPT;
    parseState         = 0;
    trackBytes         = 0;
}

void ConnectionContext::ClearContext()
{
    rwBuffer.ClearBuffer();

    if(requestInfo)
        requestInfo->ClearInfo();
    
    if(fileInfo)
        *fileInfo = FileInfo{};

    isFileOperation    = 0;
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

} // namespace WFX::Http