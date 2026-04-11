#include "http_connection.hpp"
#include "http/request/http_request.hpp"
#include "http/response/http_response.hpp"
#include "shared/apis/http_api.hpp"
#include "utils/pool/buffer_pool.hpp"

namespace WFX::Http {

using namespace WFX::Shared; // For every single abi type

// vvv Ip Address Methods vvv
WFXIpAddress& WFXIpAddress::operator=(const WFXIpAddress& other)
{
    type = other.type;
    port = other.port;

    switch(type) {
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
    std::size_t len = (type == AF_INET) ? 4 : 16;

    return port == other.port
            && type == other.type
            && memcmp(ip.raw, other.ip.raw, len) == 0;
}

// Helper functions
std::string_view WFXIpAddress::GetIpStr() const
{
    // Use thread-local static buffer to avoid heap allocation
    thread_local char ipStrBuf[INET6_ADDRSTRLEN] = {};

    const void* addr = (type == AF_INET)
        ? static_cast<const void*>(&ip.v4)
        : static_cast<const void*>(&ip.v6);

    // Convert to printable form
    if(inet_ntop(type, addr, ipStrBuf, sizeof(ipStrBuf)))
        return std::string_view(ipStrBuf);

    return std::string_view("ip-malformed");
}

const char* WFXIpAddress::GetIpType() const
{
    return type == AF_INET ? "IPv4" : "IPv6";
}

bool WFXIpAddress::ToSockAddr(sockaddr_storage& out, socklen_t& len) const
{
    switch(type) {
        case AF_INET:
        {
            auto* addr = reinterpret_cast<sockaddr_in*>(&out);
            addr->sin_family = AF_INET;
            addr->sin_port   = htons(port);
            addr->sin_addr   = ip.v4;

            len = sizeof(sockaddr_in);

            return true;
        }
        case AF_INET6:
        {
            auto* addr = reinterpret_cast<sockaddr_in6*>(&out);
            addr->sin6_family = AF_INET6;
            addr->sin6_port   = htons(port);
            addr->sin6_addr   = ip.v6;

            len = sizeof(sockaddr_in6);

            return true;
        }
        default:
            return false;
    }
}

// vvv Connection Context Methods vvv
void ConnectionContext::ResetContext()
{
    rwBuffer.ResetBuffer();
    
    if(requestInfo)  { delete requestInfo;  requestInfo  = nullptr; }
    if(responseInfo) { delete responseInfo; responseInfo = nullptr; }

    CleanupStreamGenerator();

    // Clear all flags except 'endpointState', tis special
    const bool keep = endpointState;
    __Flags = 0;
    endpointState = keep;

    // Rest of the stuff
    expectedBodyLength = 0;
    eventType          = EventType::EVENT_ACCEPT;
    parseState         = 0;
    trackBytes         = 0;
    socket             = WFX_INVALID_SOCKET;
    fileInfo           = FileInfo{};
    connInfo           = WFXIpAddress{};
    asyncData          = AsyncData{};
    clientContext      = nullptr;
    endpointContext    = nullptr;
}

void ConnectionContext::ClearContext()
{
    rwBuffer.ClearBuffer();

    if(requestInfo)  requestInfo->ClearInfo();
    if(responseInfo) responseInfo->ClearInfo();

    CleanupStreamGenerator();

    isFileOperation       = 0;
    isStreamOperation     = 0;
    isAsyncTimerOperation = 0;
    streamChunked         = 0;
    expectedBodyLength    = 0;
    trackBytes            = 0;
    fileInfo              = FileInfo{};
    asyncData             = AsyncData{};
    clientContext         = nullptr;
    endpointContext       = nullptr;
}

void ConnectionContext::CleanupStreamGenerator()
{
    if(streamGenerator.ctx && streamGenerator.Destroy)
        streamGenerator.Destroy(streamGenerator.ctx);

    streamGenerator.ctx     = nullptr;
    streamGenerator.Next    = nullptr;
    streamGenerator.Destroy = nullptr;
}

void ConnectionContext::SetParseState(HttpParseState newState)
{
    parseState = static_cast<std::uint16_t>(newState);
}

void ConnectionContext::SetConnectionState(ConnectionState newState)
{
    connectionState = static_cast<std::uint16_t>(newState);
}

void ConnectionContext::SetEndpointState(EndpointState newState)
{
    endpointState = static_cast<std::uint16_t>(newState);
}

void ConnectionContext::SetEndpointStatus(EndpointStatus newStatus)
{
    endpointStatus = static_cast<std::uint16_t>(newStatus);
}

HttpParseState ConnectionContext::GetParseState() const
{
    return static_cast<HttpParseState>(parseState);
}

ConnectionState ConnectionContext::GetConnectionState() const
{
    return static_cast<ConnectionState>(connectionState);
}

EndpointState ConnectionContext::GetEndpointState() const
{
    return static_cast<EndpointState>(endpointState);
}

EndpointStatus ConnectionContext::GetEndpointStatus() const
{
    return static_cast<EndpointStatus>(endpointStatus);
}

bool ConnectionContext::IsEndpoint() const
{
    return GetEndpointState() != EndpointState::ENDPOINT_NONE;
}

bool ConnectionContext::IsAsyncOperation() const
{
    return asyncData.AsyncComplete != nullptr;
}

} // namespace WFX::Http