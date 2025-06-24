#ifndef WFX_WINDOWS_ACCEPT_EX_MANAGER_HPP
#define WFX_WINDOWS_ACCEPT_EX_MANAGER_HPP

#define WIN32_LEAN_AND_MEAN

#include "http/connection/http_connection.hpp"
#include "http/limits/ip_limiter/ip_limiter.hpp"
#include "utils/fixed_pool/fixed_pool.hpp"
#include "utils/logger/logger.hpp"

#include <ws2ipdef.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <array>
#include <mutex>
#include <functional>

namespace WFX::OSSpecific {

using namespace WFX::Utils; // For 'Logger'
using namespace WFX::Http;  // All the stuff from http_connection.hpp

// I/O operation types for tracking overlapped operations
enum class PerIoOperationType {
    INVALID = -1,
    ARM_RECV,
    RECV,
    SEND,
    ACCEPT,
    ACCEPT_DEFERRED
};

struct PerIoBase {
    OVERLAPPED         overlapped{};
    WFXSocket          socket        = WFX_INVALID_SOCKET;
    PerIoOperationType operationType = PerIoOperationType::INVALID;

    ~PerIoBase() = default;
};

struct PerIoData : PerIoBase {
    WSABUF wsaBuf{};
};

struct PerIoContext : PerIoBase {
    char buffer[2 * (sizeof(SOCKADDR_IN) + 16)];
};

/* Just to not be confused:
 * PostRecvOp -> Take the meaning literally, this is for 're-arming' via PostReceive function. IMP: Just used as a tag
 * PostAcceptOp -> For the operation performed after Accepting the connection (setting socket options and calling acceptCallback and stuff)
 */
struct PostRecvOp : PerIoBase {
    PostRecvOp() { operationType = PerIoOperationType::ARM_RECV; }
};
static PostRecvOp ARM_RECV_OP;

struct PostAcceptOp : PerIoBase {
    WFXIpAddress ipAddr;
};

class AcceptExManager {
public:
    AcceptExManager(BufferPool& allocator);

    bool Initialize(WFXSocket listenSocket, HANDLE iocp);
    void DeInitialize();
    void HandleAcceptCompletion(PerIoContext* ctx);
    void HandleSocketOptions(SOCKET);

private:
    inline void SetSlot(int index);
    inline void ClearSlot(int index);
    int         GetSlotFromPointer(PerIoContext* ctx);
    bool        AssociateWithIOCP(WFXSocket sock);
    void        RepostAcceptAtSlot(int slot);
    bool        PostAcceptAtSlot(int slot);

public:
    constexpr static int MAX_SLOTS = 4096;

private:
    // IMP
    LPFN_ACCEPTEX             lpfnAcceptEx             = nullptr;
    LPFN_GETACCEPTEXSOCKADDRS lpfnGetAcceptExSockaddrs = nullptr;

    WFXSocket   listenSocket_;
    HANDLE      iocp_;
    BufferPool& allocator_;
    Logger&     logger_      = Logger::GetInstance();
    IpLimiter&  connLimiter_ = IpLimiter::GetInstance();

    std::array<PerIoContext, MAX_SLOTS> contexts_;
    uint64_t                            activeSlotsBits_ = 0;
};

} // namespace WFX::OSSpecific

#endif // WFX_WINDOWS_ACCEPT_EX_MANAGER_HPP