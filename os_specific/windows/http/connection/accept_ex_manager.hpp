#ifndef WFX_WINDOWS_ACCEPT_EX_MANAGER_HPP
#define WFX_WINDOWS_ACCEPT_EX_MANAGER_HPP

#define WIN32_LEAN_AND_MEAN

#include "config/config.hpp"
#include "http/connection/http_connection.hpp"
#include "http/limits/ip_limiter/ip_limiter.hpp"
#include "utils/logger/logger.hpp"

#include <ws2ipdef.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <vector>
#include <mutex>

namespace WFX::OSSpecific {

using namespace WFX::Core;  // For 'Config'
using namespace WFX::Utils; // For 'Logger'
using namespace WFX::Http;  // All the stuff from http_connection.hpp

// I/O operation types for tracking overlapped operations
enum class PerIoOperationType {
    INVALID = -1,
    ARM_RECV,
    RECV,
    SEND,
    SEND_FILE,
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

struct PerTransmitFileContext : PerIoBase {
    HANDLE fileHandle         = INVALID_HANDLE_VALUE;
    TRANSMIT_FILE_BUFFERS tfb = { 0 };
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
    void        SetSlot(std::size_t slot);
    void        ClearSlot(std::size_t slot);
    bool        IsSlotSet(std::size_t slot);
    std::size_t GetSlotFromPointer(PerIoContext* ctx);
    bool        AssociateWithIOCP(WFXSocket sock);
    void        RepostAcceptAtSlot(std::size_t slot);
    bool        PostAcceptAtSlot(std::size_t slot);

private:
    // IMP
    LPFN_ACCEPTEX             lpfnAcceptEx             = nullptr;
    LPFN_GETACCEPTEXSOCKADDRS lpfnGetAcceptExSockaddrs = nullptr;

    WFXSocket   listenSocket_;
    HANDLE      iocp_;
    BufferPool& allocator_;
    Logger&     logger_      = Logger::GetInstance();
    IpLimiter&  connLimiter_ = IpLimiter::GetInstance();
    Config&     config_      = Config::GetInstance();

    std::vector<PerIoContext> contexts_;
    
    std::unique_ptr<std::atomic<std::size_t>[]> activeSlotsBits_;
    std::size_t activeSlotsWordCount_ = 0;
};

} // namespace WFX::OSSpecific

#endif // WFX_WINDOWS_ACCEPT_EX_MANAGER_HPP