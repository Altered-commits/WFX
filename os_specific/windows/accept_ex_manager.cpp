#include "accept_ex_manager.hpp"

namespace WFX::OSSpecific {

AcceptExManager::AcceptExManager(BufferPool& allocator)
    : allocator_(allocator)
{}

bool AcceptExManager::Initialize(WFXSocket listenSocket, HANDLE iocp)
{
    listenSocket_ = listenSocket;
    iocp_         = iocp;

    // Load AcceptEx and GetAcceptExSockaddrs
    GUID guidAcceptEx             = WSAID_ACCEPTEX;
    GUID guidGetAcceptExSockaddrs = WSAID_GETACCEPTEXSOCKADDRS;
    DWORD bytes                   = 0;

    int result = WSAIoctl(
        listenSocket_,
        SIO_GET_EXTENSION_FUNCTION_POINTER,
        &guidAcceptEx,
        sizeof(guidAcceptEx),
        &lpfnAcceptEx,
        sizeof(lpfnAcceptEx),
        &bytes,
        nullptr,
        nullptr
    );

    if(result == SOCKET_ERROR || !lpfnAcceptEx)
        return logger_.Error("[AcceptExManager]: Failed to load AcceptEx"), false;

    result = WSAIoctl(
        listenSocket_,
        SIO_GET_EXTENSION_FUNCTION_POINTER,
        &guidGetAcceptExSockaddrs,
        sizeof(guidGetAcceptExSockaddrs),
        &lpfnGetAcceptExSockaddrs,
        sizeof(lpfnGetAcceptExSockaddrs),
        &bytes,
        nullptr,
        nullptr
    );

    if(result == SOCKET_ERROR || !lpfnGetAcceptExSockaddrs)
        return logger_.Error("[AcceptExManager]: Failed to load GetAcceptExSockaddrs"), false;

    // We know exactly how much we using, so reserve it
    std::uint32_t maxAcceptSlots = config_.osSpecificConfig.maxAcceptSlots;
    activeSlotsWordCount_        = (maxAcceptSlots + 63) / 64;

    if(activeSlotsWordCount_ * 64 < maxAcceptSlots)
        logger_.Fatal("[AcceptExManager]: activeSlotsBits_ insufficient for maxAcceptSlots (", 
                    activeSlotsWordCount_ * 64, " bits < ", maxAcceptSlots, " slots)");

    contexts_.resize(maxAcceptSlots);

    // Correct atomic initialization
    activeSlotsBits_ = std::make_unique<std::atomic<uint64_t>[]>(activeSlotsWordCount_);
    for(std::size_t i = 0; i < activeSlotsWordCount_; ++i)
        activeSlotsBits_[i].store(0, std::memory_order_relaxed);

    // Initialize AcceptEx slots
    for(std::uint32_t i = 0; i < maxAcceptSlots; ++i) {
        if(!PostAcceptAtSlot(i)) {
            logger_.Error("[AcceptExManager]: Failed to initialize AcceptEx at slot ", i);
            return false;
        }
    }

    logger_.Info("[AcceptExManager]: Initialized ", maxAcceptSlots, " concurrent AcceptEx slots");
    return true;
}

void AcceptExManager::DeInitialize()
{
    std::uint32_t maxAcceptSlots = config_.osSpecificConfig.maxAcceptSlots;

    for(int i = 0; i < maxAcceptSlots; ++i)
    {
        if(!IsSlotSet(i))
            continue;

        PerIoContext& ctx = contexts_[i];

        if(ctx.socket != WFX_INVALID_SOCKET)
        {
            closesocket(ctx.socket);
            ctx.socket = WFX_INVALID_SOCKET;
        }

        ClearSlot(i);
    }

    logger_.Info("[AcceptExManager]: DeInitialized ", maxAcceptSlots, " AcceptEx slots");
}

void AcceptExManager::HandleAcceptCompletion(PerIoContext* ctx)
{
    const int slot   = GetSlotFromPointer(ctx);
    WFXSocket client = ctx->socket;

    if(client == WFX_INVALID_SOCKET) {
        logger_.Error("[AcceptExManager]: Invalid socket on completion.");
        RepostAcceptAtSlot(slot);
        return;
    }

    // Low-level socket close
    auto ForceCloseSocket = [&]() {
        linger soLinger{1, 0};
        setsockopt(client, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&soLinger), sizeof(soLinger));
        closesocket(client);
    };

    // Fast reject without acceptOp, for limiter failure or alloc fail
    auto FullReject = [&](const WFXIpAddress& ip, bool releaseLimiter) {
        if(releaseLimiter)
            connLimiter_.ReleaseConnection(ip);
        ForceCloseSocket();
        RepostAcceptAtSlot(slot);
    };

    // Used once acceptOp is alive
    auto AggressiveClose = [&](PostAcceptOp* acceptOp, bool repost) {
        connLimiter_.ReleaseConnection(acceptOp->ipAddr);
        ForceCloseSocket();
        allocator_.Release(acceptOp);
        if(repost)
            RepostAcceptAtSlot(slot);
    };

    SOCKADDR* localSockaddr     = nullptr;
    SOCKADDR* remoteSockaddr    = nullptr;
    int       localSockaddrLen  = 0;
    int       remoteSockaddrLen = 0;

    // GetAcceptExSockaddrs MUST be called before setsockopt or using the client socket
    lpfnGetAcceptExSockaddrs(
        ctx->buffer,
        0,
        sizeof(SOCKADDR_IN) + 16,
        sizeof(SOCKADDR_IN) + 16,
        &localSockaddr,
        &localSockaddrLen,
        &remoteSockaddr,
        &remoteSockaddrLen
    );

    // Pre-parse IP
    WFXIpAddress tempIpAddr;
    switch(remoteSockaddr->sa_family) {
        case AF_INET:
        {
            sockaddr_in* ipv4 = reinterpret_cast<sockaddr_in*>(remoteSockaddr);
            tempIpAddr.ip.v4  = ipv4->sin_addr;
            tempIpAddr.ipType = AF_INET;
            break;
        }
        case AF_INET6:
        {
            sockaddr_in6* ipv6 = reinterpret_cast<sockaddr_in6*>(remoteSockaddr);
            tempIpAddr.ip.v6  = ipv6->sin6_addr;
            tempIpAddr.ipType = AF_INET6;
            break;
        }
        default:
        {
            std::memset(&tempIpAddr.ip, 0, sizeof(tempIpAddr.ip));
            tempIpAddr.ipType = 0;
            break;
        }
    }

    // Limiter check
    if(!connLimiter_.AllowConnection(tempIpAddr)) {
        logger_.Warn("[AcceptExManager]: Connection limit reached!");
        FullReject(tempIpAddr, false);
        return;
    }

    // Allocate acceptOp only if allowed
    PostAcceptOp* acceptOp = nullptr;
    if(void* mem = allocator_.Lease(sizeof(PostAcceptOp)); mem)
        acceptOp = new (mem) PostAcceptOp();
    else {
        logger_.Error("[AcceptExManager]: Failed to allocate PostAcceptOp for accepted socket");
        FullReject(tempIpAddr, true);
        return;
    }

    acceptOp->socket         = client;
    acceptOp->operationType  = PerIoOperationType::ACCEPT_DEFERRED;
    acceptOp->ipAddr         = tempIpAddr;

    // Socket options
    setsockopt(client, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char*)&listenSocket_, sizeof(listenSocket_));

    if(!AssociateWithIOCP(client)) {
        logger_.Error("[AcceptExManager]: Failed to associate accepted socket with IOCP");
        AggressiveClose(acceptOp, true);
        return;
    }

    RepostAcceptAtSlot(slot);

    if(!PostQueuedCompletionStatus(iocp_, 0, 0, &(acceptOp->overlapped))) {
        logger_.Error("[AcceptExManager]: Failed to queue deferred accept for socket ", client);
        AggressiveClose(acceptOp, false);
    }
}

void AcceptExManager::HandleSocketOptions(SOCKET client)
{
    int one = 1;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof(one));
    // setsockopt(client, SOL_SOCKET, SO_REUSEADDR, (char*)&one, sizeof(one));
}

void AcceptExManager::SetSlot(std::size_t slot)
{
    std::size_t word = slot >> 6;
    std::size_t bit  = slot & 63;
    activeSlotsBits_[word].fetch_or(1ULL << bit, std::memory_order_relaxed);
}

void AcceptExManager::ClearSlot(std::size_t slot)
{
    std::size_t word = slot >> 6;
    std::size_t bit  = slot & 63;
    activeSlotsBits_[word].fetch_and(~(1ULL << bit), std::memory_order_relaxed);
}

bool AcceptExManager::IsSlotSet(std::size_t slot)
{
    std::size_t word = slot >> 6;
    std::size_t bit  = slot & 63;
    return (activeSlotsBits_[word].load(std::memory_order_relaxed) & (1ULL << bit)) != 0;
}

std::size_t AcceptExManager::GetSlotFromPointer(PerIoContext* ctx)
{
    // Current Address - Base Address gives us the index based on how pointer arithmetic works. Pretty cool
    return ((reinterpret_cast<uintptr_t>(ctx) - reinterpret_cast<uintptr_t>(&contexts_[0]))
                /
            sizeof(PerIoContext));
}

bool AcceptExManager::AssociateWithIOCP(WFXSocket sock)
{
    return CreateIoCompletionPort((HANDLE)sock, iocp_, (ULONG_PTR)sock, 0);
}

void AcceptExManager::RepostAcceptAtSlot(std::size_t slot)
{
    ClearSlot(slot);

    // Try original slot — fast path
    if(PostAcceptAtSlot(slot)) return;

    // Fallback scan across bitset
    std::size_t fallbackSlot = static_cast<std::size_t>(-1);
    for(std::size_t word = 0; word < activeSlotsWordCount_; ++word) {
        uint64_t available = ~activeSlotsBits_[word].load(std::memory_order_relaxed);
        if(available == 0) continue;

#if defined(_MSC_VER)
        unsigned long bitIndex;
        if(_BitScanForward64(&bitIndex, available)) {
            fallbackSlot = word * 64 + bitIndex;
            break;
        }
#else
        int bitIndex = __builtin_ffsll(available); // 1-based index
        if(bitIndex != 0) {
            fallbackSlot = word * 64 + (bitIndex - 1);
            break;
        }
#endif
    }

    // Only log if fallback also fails
    bool success = (fallbackSlot != static_cast<std::size_t>(-1)) &&
                   PostAcceptAtSlot(fallbackSlot);

    if(!success)
        logger_.Error("AcceptExManager: All AcceptEx slots exhausted — recovery failed.");
}

bool AcceptExManager::PostAcceptAtSlot(std::size_t slot)
{
    PerIoContext& ctx = contexts_[slot];
    ctx.overlapped    = { 0 };

    ctx.socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if(ctx.socket == WFX_INVALID_SOCKET) {
        logger_.Error("[AcceptExManager]: WSASocket failed at slot ", slot, ", err=", WSAGetLastError());
        return false;
    }

    ctx.operationType = PerIoOperationType::ACCEPT;

    DWORD bytesReceived = 0;
    BOOL result = lpfnAcceptEx(
        listenSocket_,
        ctx.socket,
        ctx.buffer,
        0,
        sizeof(SOCKADDR_IN) + 16,
        sizeof(SOCKADDR_IN) + 16,
        &bytesReceived,
        &ctx.overlapped
    );

    if(!result && WSAGetLastError() != ERROR_IO_PENDING) {
        logger_.Error("[AcceptExManager]: AcceptEx failed at slot ", slot, ", err=", WSAGetLastError());
        closesocket(ctx.socket);
        ctx.socket = WFX_INVALID_SOCKET;
        return false;
    }

    SetSlot(slot);
    return true;
}

} // namespace WFX::OSSpecific