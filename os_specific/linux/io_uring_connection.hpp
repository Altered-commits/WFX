#ifdef WFX_LINUX_USE_IO_URING

#ifndef WFX_LINUX_IO_URING_CONNECTION_HPP
#define WFX_LINUX_IO_URING_CONNECTION_HPP

#include "config/config.hpp"
#include "http/connection/http_connection.hpp"
#include "http/limits/ip_limiter/ip_limiter.hpp"
#include "utils/buffer_pool/buffer_pool.hpp"

#include <liburing.h>
#include <atomic>
#include <cstring>

namespace WFX::OSSpecific {

using namespace WFX::Http;  // For 'HttpConnectionHandler', 'ReceiveCallback', 'ConnectionContext', ...
using namespace WFX::Utils; // For 'Logger', 'RWBuffer', ...
using namespace WFX::Core;  // For 'Config'

struct AcceptSlot : public ConnectionTag {
    socklen_t        addrLen = 0;
    sockaddr_storage addr    = { 0 };
};

class IoUringConnectionHandler : public HttpConnectionHandler {
public:
    IoUringConnectionHandler() = default;
    ~IoUringConnectionHandler();

public: // Initializing
    void Initialize(const std::string& host, int port) override;
    void SetReceiveCallback(ReceiveCallback onData)    override;
    
public: // I/O Operations
    void ResumeReceive(ConnectionContext* ctx)                       override;
    void Write(ConnectionContext* ctx, std::string_view buffer = {}) override;
    void WriteFile(ConnectionContext* ctx, std::string path)         override;
    void Close(ConnectionContext* ctx)                               override;
    
public: // Main Functions
    void Run()                                                               override;
    void RefreshExpiry(ConnectionContext* ctx, std::uint16_t timeoutSeconds) override;
    void Stop()                                                              override;

private: // Helper Functions
    int                AllocSlot(std::uint64_t* bitmap, int numWords, int maxSlots);
    void               FreeSlot(std::uint64_t* bitmap, int idx);
    
    ConnectionContext* GetConnection();
    void               ReleaseConnection(ConnectionContext* ctx);
    AcceptSlot*        GetAccept();
    void               ReleaseAccept(AcceptSlot* slot);

    void               SetNonBlocking(int fd);
    bool               EnsureFileReady(ConnectionContext* ctx, std::string path);
    int                ResolveHostToIpv4(const char* host, in_addr* outAddr);

    void               AddAccept();
    void               AddRecv(ConnectionContext* ctx);
    void               AddSend(ConnectionContext* ctx, std::string_view msg);
    void               AddFile(ConnectionContext* ctx);
    void               SubmitBatch();

private:
    // Misc
    IpLimiter&        ipLimiter_  = IpLimiter::GetInstance();
    Config&           config_     = Config::GetInstance();
    Logger&           logger_     = Logger::GetInstance();
    std::atomic<bool> running_    = true;
    ReceiveCallback   onReceive_;
    BufferPool        pool_{1, 1024 * 1024, [](std::size_t currSize){ return currSize * 2; }};
    FileCache*        fileCache_  = GetGlobalState().fileCache;

private:
    // IoUring
    int      listenFd_ = -1;
    int      sqeBatch_ = 0;
    io_uring ring_     = { 0 };

private:
    // Connection Context
    std::unique_ptr<ConnectionContext[]> connections_;
    std::unique_ptr<std::uint64_t[]>     connBitmap_;
    // Connection Accept
    std::unique_ptr<AcceptSlot[]>        acceptSlots_;
    std::unique_ptr<std::uint64_t[]>     acceptBitmap_;
    // Derived size
    std::uint32_t connWords_      = 0;
    std::uint32_t acceptWords_    = 0;
};

} // namespace WFX::OSSpecific

#endif // WFX_LINUX_IO_URING_CONNECTION_HPP

#endif // WFX_LINUX_USE_IO_URING