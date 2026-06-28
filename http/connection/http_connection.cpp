// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

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
    return port == other.port && type == other.type && memcmp(ip.raw, other.ip.raw, len) == 0;
}

// Helper functions
std::string_view WFXIpAddress::GetIpStr() const
{
    // Use thread-local static buffer to avoid heap allocation
    thread_local char ipStrBuf[INET6_ADDRSTRLEN] = {};

    const void* addr = (type == AF_INET) ? static_cast<const void*>(&ip.v4) : static_cast<const void*>(&ip.v6);

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
        case AF_INET: {
            auto* addr = reinterpret_cast<sockaddr_in*>(&out);
            addr->sin_family = AF_INET;
            addr->sin_port = htons(port);
            addr->sin_addr = ip.v4;

            len = sizeof(sockaddr_in);

            return true;
        }
        case AF_INET6: {
            auto* addr = reinterpret_cast<sockaddr_in6*>(&out);
            addr->sin6_family = AF_INET6;
            addr->sin6_port = htons(port);
            addr->sin6_addr = ip.v6;

            len = sizeof(sockaddr_in6);

            return true;
        }
        default:
            return false;
    }
}

// vvv Client Context Methods vvv
void ClientCtx::Reset()
{
    rwBuffer.ResetBuffer();

    if(requestInfo) {
        delete requestInfo;
        requestInfo = nullptr;
    }
    if(responseInfo) {
        delete responseInfo;
        responseInfo = nullptr;
    }

    CleanupStreamGenerator();

    flags = 0;
    handshakeDone = false;
    expectedBodyLength = 0;
    eventType = EventType::EVENT_ACCEPT;
    trackBytes = 0;
    socket = WFX_INVALID_SOCKET;
    fileInfo = FileInfo{};
    connInfo = WFXIpAddress{};
    asyncData = AsyncData{};
    endpointCtx = nullptr;
    // sslConn:      caller freed it before Reset()
    // generationId: managed by GetConnection()
}

void ClientCtx::Clear()
{
    rwBuffer.ClearBuffer();

    if(requestInfo)
        requestInfo->ClearInfo();
    if(responseInfo)
        responseInfo->Reset();

    CleanupStreamGenerator();

    isFileOperation = 0;
    isStreamOperation = 0;
    isAsyncTimerOperation = 0;
    streamChunked = 0;
    expectedBodyLength = 0;
    trackBytes = 0;
    fileInfo = FileInfo{};
    asyncData = AsyncData{};
    endpointCtx = nullptr;
}

void ClientCtx::CleanupStreamGenerator()
{
    if(streamGenerator.ctx && streamGenerator.Destroy)
        streamGenerator.Destroy(streamGenerator.ctx);

    streamGenerator.ctx = nullptr;
    streamGenerator.Next = nullptr;
    streamGenerator.Destroy = nullptr;
}

void ClientCtx::SetParseState(HttpParseState newState)
{
    parseState = static_cast<std::uint16_t>(newState);
}

void ClientCtx::SetConnectionState(ConnectionState newState)
{
    connectionState = static_cast<std::uint16_t>(newState);
}

HttpParseState ClientCtx::GetParseState() const
{
    return static_cast<HttpParseState>(parseState);
}

ConnectionState ClientCtx::GetConnectionState() const
{
    return static_cast<ConnectionState>(connectionState);
}

bool ClientCtx::IsAsyncOperation() const
{
    return asyncData.AsyncComplete != nullptr;
}

// vvv Endpoint Context Methods vvv
void EndpointCtx::Reset()
{
    rwBuffer.ResetBuffer();

    SetConnectionState(ConnectionState::CONNECTION_CLOSE);

    isShuttingDown = 0;
    inOnConnectPhase = 0;
    isAwaitingReconnect = 0;
    eventType = EventType::EVENT_ACCEPT;
    clientCtx = nullptr;
    asyncData = AsyncData{};
    coalesceKey = 0;
    reconnectAttempts = 0;
    socket = WFX_INVALID_SOCKET;
    // endpointState, endpointIdx:          preserved, TLS config and pool identity survive reset
    // sslConn:                             caller freed it before Reset()
    // generationId:                        managed by GetConnection()
    // slotState, parseStateObj, outputObj: managed by FinalizeEndpointRequest/ReleaseEndpoint
}

void EndpointCtx::SetConnectionState(ConnectionState newState)
{
    connectionState = static_cast<std::uint8_t>(newState);
}

void EndpointCtx::SetEndpointState(EndpointState newState)
{
    endpointState = static_cast<std::uint8_t>(newState);
}

ConnectionState EndpointCtx::GetConnectionState() const
{
    return static_cast<ConnectionState>(connectionState);
}

EndpointState EndpointCtx::GetEndpointState() const
{
    return static_cast<EndpointState>(endpointState);
}

bool EndpointCtx::IsAsyncOperation() const
{
    return asyncData.AsyncComplete != nullptr;
}

} // namespace WFX::Http