#include "http_connection.hpp"
#include "http/request/http_request.hpp"
#include "http/response/http_response.hpp"
#include "shared/apis/http_api.hpp"

namespace WFX::Http {

// vvv Ip Address Methods vvv
WFXIpAddress& WFXIpAddress::operator=(const WFXIpAddress& other)
{
    ipType = other.ipType;

    switch(ipType) {
        case AF_INET:  memcpy(&ip.v4, &other.ip.v4, sizeof(in_addr));  break;
        case AF_INET6: memcpy(&ip.v6, &other.ip.v6, sizeof(in6_addr)); break;
        default:       memset(&ip, 0, sizeof(ip));                     break;
    }

    return *this;
}

bool WFXIpAddress::operator==(const WFXIpAddress& other) const
{
    if(ipType != other.ipType)
        return false;

    switch(ipType) {
        case AF_INET:  return memcmp(ip.raw, other.ip.raw, 4) == 0;
        case AF_INET6: return memcmp(ip.raw, other.ip.raw, 16) == 0;
        default:       return false;
    }
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
    parentCoro.Reset();
    
    if(requestInfo)  { delete requestInfo;  requestInfo  = nullptr; }
    if(responseInfo) { delete responseInfo; responseInfo = nullptr; }

    CleanupStreamGenerator();

    // Clear all flags except 'endpointState', tis special
    const bool keep = endpointState;
    __Flags = 0;
    endpointState = keep;

    // Rest of the stuff
    fileInfo           = FileInfo{};
    connInfo           = WFXIpAddress{};
    expectedBodyLength = 0;
    eventType          = EventType::EVENT_ACCEPT;
    parseState         = 0;
    trackBytes         = 0;
    socket             = WFX_INVALID_SOCKET;
    clientContext      = nullptr;
    endpointContext    = nullptr;
}

void ConnectionContext::ClearContext()
{
    rwBuffer.ClearBuffer();
    parentCoro.Reset();

    if(requestInfo)  requestInfo->ClearInfo();
    if(responseInfo) responseInfo->ClearInfo();

    CleanupStreamGenerator();

    fileInfo              = FileInfo{};
    isFileOperation       = 0;
    isStreamOperation     = 0;
    isAsyncTimerOperation = 0;
    streamChunked         = 0;
    expectedBodyLength    = 0;
    trackBytes            = 0;
    clientContext         = nullptr;
    endpointContext       = nullptr;
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
    return static_cast<bool>(parentCoro);
}

void ConnectionContext::CleanupStreamGenerator()
{
    if(streamGenerator.ctx && streamGenerator.Destroy)
        streamGenerator.Destroy(streamGenerator.ctx);

    streamGenerator.ctx     = nullptr;
    streamGenerator.Next    = nullptr;
    streamGenerator.Destroy = nullptr;
}

Async::Status ConnectionContext::TryFinishCoroutines()
{
    /*
     * So return value logic is simple:
     *  - NONE means ignore it and move on
     *  - COMPLETED means serialize the response
     *  - OTHERS are just errors
     */
    // Sanity checks, is the connection still alive or no
    if(GetConnectionState() == ConnectionState::CONNECTION_CLOSE)
        return Async::Status::NONE;

    // Handle is somehow fucked up. Either it was completed somehow or idk
    // Just assume everythings fine yk, life is rainbows and sunshines
    if(!static_cast<bool>(parentCoro))
        return Async::Status::NONE;

    // THE MOST IMPORTANT THING, ASYNC FUNCTIONS EXPECT US TO SET CTX (current connection context)-
    // -VIA HTTP API, BECAUSE THEY USE IT INTERNALLY
    auto httpApi = WFX::Shared::GetHttpAPIV1();
    httpApi->SetGlobalPtrData(this);

    parentCoro.Resume();
    if(!parentCoro.IsFinished())
        return Async::Status::NONE;

    // WE WILL SET IT TO NULLPTR ONCE WE ARE DONE USING IT, WE DON'T WANT DANGLING POINTERS
    httpApi->SetGlobalPtrData(nullptr);

    // Check final coroutine status via 'BasePromise' (possible because its inherited) and,-
    // -if this was middleware, extract 'MiddlewareAction' into the engine pipeline
    auto& base = parentCoro.GetPromise<Async::BasePromise>();
    if(base.error_ != Async::Status::NONE)
        return base.error_;

    if(trackAsync.GetELevel() == ExecutionLevel::MIDDLEWARE) {
        auto& promise = parentCoro.GetPromise<Async::Promise<MiddlewareAction>>();
        *trackAsync.GetMAction() = promise.value_;
    }

    return Async::Status::COMPLETED;
}

} // namespace WFX::Http