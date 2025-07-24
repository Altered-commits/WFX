#include "http_connection.hpp"

namespace WFX::Http {

// vvv Ip Address methods vvv
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
            memset(&ip, 0, sizeof(ip)); // To be safe on invalid type
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

// vvv Connection Context methods vvv
HttpConnectionState ConnectionContext::GetState()
{
    return connState.load(std::memory_order_acquire);
}

void ConnectionContext::SetState(HttpConnectionState newState)
{
    HttpConnectionState cur = connState.load(std::memory_order_acquire);
    while(true)
        if(connState.compare_exchange_strong(cur, newState, std::memory_order_acq_rel))
            return;
}

bool ConnectionContext::TransitionTo(HttpConnectionState newState) {
    HttpConnectionState currentState = connState.load(std::memory_order_acquire);

    while(true) {
        // Transitions from OCCUPIED / CLOSING_IMMEDIATE -> ANYTHING are forbidden
        if(currentState == HttpConnectionState::CLOSING_IMMEDIATE
            || currentState == HttpConnectionState::OCCUPIED)
            return false;

        // Transition from CLOSING_DEFAULT to CLOSING_IMMEDIATE is allowed, rest is forbidden
        if(currentState == HttpConnectionState::CLOSING_DEFAULT
            && newState != HttpConnectionState::CLOSING_IMMEDIATE)
            return false;

        if(connState.compare_exchange_strong(currentState, newState, std::memory_order_acq_rel))
            return true;
    }
}

} // namespace WFX::Http