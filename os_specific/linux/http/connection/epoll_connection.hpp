#ifndef WFX_LINUX_USE_IO_URING

#ifndef WFX_LINUX_EPOLL_CONNECTION_HPP
#define WFX_LINUX_EPOLL_CONNECTION_HPP

#include "config/config.hpp"
#include "http/connection/http_connection.hpp"
#include "http/limits/ip_limiter/ip_limiter.hpp"
#include "http/ssl/http_ssl.hpp"
#include "utils/filecache/filecache.hpp"
#include "utils/buffer_pool/buffer_pool.hpp"
#include "utils/timer/timer_wheel/timer_wheel.hpp"
#include "utils/timer/timer_heap/timer_heap.hpp"

#include <sys/epoll.h>

namespace WFX::OSSpecific {

using namespace WFX::Http;  // For 'HttpConnectionHandler', 'ReceiveCallback', 'ConnectionContext', ...
using namespace WFX::Utils; // For 'Logger', 'RWBuffer', ...
using namespace WFX::Core;  // For 'Config'

using SteadyClock = std::chrono::steady_clock;

class EpollConnectionHandler : public HttpConnectionHandler {
public:
    EpollConnectionHandler(bool useHttps);
    ~EpollConnectionHandler();

public: // Initializing
    void Initialize(const std::string& host, int port)                                 override;
    void SetEngineCallbacks(ReceiveCallback onData, CompletionCallback onComplete)     override;
    
public: // I/O Operations
    void ResumeReceive(ConnectionContext* ctx)                                         override;
    void Write(ConnectionContext* ctx, std::string_view buffer = {})                   override;
    void WriteFile(ConnectionContext* ctx, std::string path)                           override;
    void Stream(ConnectionContext* ctx, StreamGenerator generator, bool streamChunked) override;
    void Close(ConnectionContext* ctx, bool forceClose = false)                        override;
    
public: // Main Functions
    void Run()                                                                      override;
    void RefreshExpiry(ConnectionContext* ctx, std::uint16_t timeoutSeconds)        override;
    bool RefreshAsyncTimer(ConnectionContext* ctx, std::uint32_t delayMilliseconds) override;
    void Stop()                                                                     override;

private: // Helper Functions
    std::int64_t       AllocSlot(std::uint64_t* bitmap, std::uint32_t numWords, std::uint32_t maxSlots);
    void               FreeSlot(std::uint64_t* bitmap, std::uint32_t idx);
    ConnectionContext* GetConnection();
    void               ReleaseConnection(ConnectionContext* ctx);
    
    std::uint64_t      NowMs();
    bool               SetNonBlocking(int fd);
    bool               EnsureFileReady(ConnectionContext* ctx, std::string path);
    bool               EnsureReadReady(ConnectionContext* ctx);
    bool               ResolveHostToIpv4(const char* host, in_addr* outAddr);
    
    void               Receive(ConnectionContext* ctx);
    void               SendFile(ConnectionContext* ctx);
    void               ResumeStream(ConnectionContext* ctx);
    void               PollAgain(ConnectionContext* ctx, EventType eventType, std::uint32_t events);
    void               UpdateAsyncTimer();
    
    void               WrapAccept(ConnectionContext* ctx, int clientFd);
    ssize_t            WrapRead(ConnectionContext* ctx, char* buf, std::size_t len);
    ssize_t            WrapWrite(ConnectionContext* ctx, const char* buf, std::size_t len);
    ssize_t            WrapFile(ConnectionContext* ctx, int fd, off_t* offset, std::size_t count);

private: // Misc
    Config&            config_     = Config::GetInstance();
    Logger&            logger_     = Logger::GetInstance();
    FileCache&         fileCache_  = FileCache::GetInstance();
    BufferPool&        pool_       = BufferPool::GetInstance();

    IpLimiter          ipLimiter_  = {pool_};
    ReceiveCallback    onReceive_  = {};
    CompletionCallback onComplete_ = {};
    std::atomic<bool>  running_    = true;
    bool               useHttps_   = false;

private: // Constexpr stuff
    constexpr static char    CHUNK_END[]           = "0\r\n\r\n";
    constexpr static ssize_t SWITCH_FILE_TO_STREAM = std::numeric_limits<ssize_t>::min();

    constexpr static int INVOKE_TIMEOUT_COOLDOWN = 5; // In seconds
    constexpr static int INVOKE_TIMEOUT_DELAY    = 1; // In seconds

private: // Timeout handler
    TimerWheel              timerWheel_;
    TimerHeap               timerHeap_      = {pool_};
    SteadyClock::time_point startTime_      = SteadyClock::now();
    int                     timeoutTimerFd_ = -1;
    int                     asyncTimerFd_   = -1;

private: // Epoll + SSL
    int           listenFd_  = -1;
    int           epollFd_   = -1;
    std::uint16_t maxEvents_ = config_.osSpecificConfig.maxEvents;

    std::unique_ptr<HttpWFXSSL>    sslHandler_ = nullptr;
    std::unique_ptr<epoll_event[]> events_     = nullptr;

private: // Connection Context
    std::unique_ptr<ConnectionContext[]> connections_ = nullptr;
    std::unique_ptr<std::uint64_t[]>     connBitmap_  = nullptr;
    std::uint32_t                        connWords_   = 0;

    // TODO: FOR DEBUG ONLY, REMOVE IT AFTER
    std::uint64_t numConnectionsAlive_ = 0;
};

} // namespace WFX::OSSpecific

#endif // WFX_LINUX_EPOLL_CONNECTION_HPP

#endif // !WFX_LINUX_USE_IO_URING