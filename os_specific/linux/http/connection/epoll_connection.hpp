#ifndef WFX_LINUX_EPOLL_CONNECTION_HPP
#define WFX_LINUX_EPOLL_CONNECTION_HPP

#include "config/config.hpp"
#include "http/connection/http_connection.hpp"
#include "http/limits/ip_limiter/ip_limiter.hpp"
#include "os_specific/linux/utils/file_cache/file_cache.hpp"
#include "utils/buffer_pool/buffer_pool.hpp"

#include <sys/epoll.h>
#include <atomic>
#include <cstring>

namespace WFX::OSSpecific {

using namespace WFX::Http;  // For 'HttpConnectionHandler', 'ReceiveCallback', 'ConnectionContext', ...
using namespace WFX::Utils; // For 'Logger', 'RWBuffer', ...
using namespace WFX::Core;  // For 'Config'

class EpollConnectionHandler : public HttpConnectionHandler {
public:
    EpollConnectionHandler() = default;
    ~EpollConnectionHandler();

public: // Initializing
    void Initialize(const std::string& host, int port) override;
    void SetReceiveCallback(ReceiveCallback onData)    override;
    
public: // I/O Operations
    void ResumeReceive(ConnectionContext* ctx)                       override;
    void Write(ConnectionContext* ctx, std::string_view buffer = {}) override;
    void WriteFile(ConnectionContext* ctx, std::string path)         override;
    void Close(ConnectionContext* ctx)                               override;
    
public: // Main Functions
    void         Run()            override;
    HttpTickType GetCurrentTick() override;
    void         Stop()           override;

private: // Helper Functions
    int                AllocSlot(std::uint64_t* bitmap, int numWords, int maxSlots);
    void               FreeSlot(std::uint64_t* bitmap, int idx);
    ConnectionContext* GetConnection();
    void               ReleaseConnection(ConnectionContext* ctx);
    void               SetNonBlocking(int fd);
    bool               EnsureFileReady(ConnectionContext* ctx, std::string path);
    bool               EnsureReadReady(ConnectionContext* ctx);
    int                ResolveHostToIpv4(const char* host, in_addr* outAddr);
    void               Receive(ConnectionContext* ctx);
    void               SendFile(ConnectionContext* ctx);
    void               PollAgain(ConnectionContext* ctx, EventType eventType);

private:
    // Misc
    IpLimiter&        ipLimiter_  = IpLimiter::GetInstance();
    Config&           config_     = Config::GetInstance();
    Logger&           logger_     = Logger::GetInstance();
    std::atomic<bool> running_    = true;
    ReceiveCallback   onReceive_;
    BufferPool        pool_{1, 1024 * 1024, [](std::size_t currSize){ return currSize * 2; }};
    FileCache         fileCache_{config_.osSpecificConfig.fileCacheSize};

private:
    // Epoll
    int           listenFd_  = -1;
    int           epollFd_   = -1;
    std::uint16_t maxEvents_ = config_.osSpecificConfig.maxEvents;

    std::unique_ptr<epoll_event[]> events_ = nullptr;

private:
    // Connection Context
    std::unique_ptr<ConnectionContext[]> connections_ = nullptr;
    std::unique_ptr<std::uint64_t[]>     connBitmap_  = nullptr;
    std::uint32_t                        connWords_   = 0;
};

} // namespace WFX::OSSpecific

#endif // WFX_LINUX_EPOLL_CONNECTION_HPP