#ifndef WFX_LINUX_USE_IO_URING

#ifndef WFX_LINUX_EPOLL_CONNECTION_HPP
#define WFX_LINUX_EPOLL_CONNECTION_HPP

#include "config/config.hpp"
#include "http/connection/http_connection.hpp"
#include "http/limits/ip_limiter/ip_limiter.hpp"
#include "http/ssl/http_ssl.hpp"
#include "utils/fileops/filecache.hpp"
#include "utils/pool/bitmap_pool.hpp"
#include "utils/timer/timer_wheel.hpp"
#include "utils/timer/timer_heap.hpp"

#include <sys/epoll.h>
#include <atomic>

namespace WFX::OSSpecific {

using namespace WFX::Http;   // For 'HttpConnectionHandler', 'ReceiveCallback', 'ConnectionContext', ...
using namespace WFX::Utils;  // For 'Logger', 'RWBuffer', ...
using namespace WFX::Core;   // For 'Config'
using namespace WFX::Shared; // For 'EndpointStatus', 'AsyncData', ...

using HandshakeCb    = std::function<void(void)>;
using SteadyClock    = std::chrono::steady_clock;
using ConnectionPool = BitmapPool<ConnectionContext>;

// Each endpoint has a fixed pool of connections (no dynamic growth allowed)
// Think of it as a map of Endpoint -> ConnectionPool
// We use an index based container instead of an unordered_map because maps are not cache friendly
using EndpointContainer = std::pair<EndpointContext, ConnectionPool>;
using EndpointPool      = std::vector<EndpointContainer>;

class EpollConnectionHandler : public HttpConnectionHandler {
public:
    EpollConnectionHandler(bool useHttps);
    ~EpollConnectionHandler();

public: // Initializing
    void          Initialize(const std::string& host, std::uint16_t port) override;
    void          SetEngineCallback(ReceiveCallback onData)               override;
    std::uint16_t AllocateEndpoint(
        std::string_view host, std::string_view port, std::uint32_t cLimit, std::uint32_t ifLimit, bool useTLS
    ) override;
    
public: // I/O Operations
    void           ResumeReceive(ConnectionContext* ctx)                                         override;
    void           Write(ConnectionContext* ctx, std::string_view buffer = {})                   override;
    void           WriteFile(ConnectionContext* ctx, std::string path)                           override;
    EndpointStatus WriteEndpoint(
        ConnectionContext* ctx, std::uint32_t endpointIndex, const std::byte* ptr, std::uint32_t size
    ) override;
    void           Stream(ConnectionContext* ctx, StreamGenerator generator, bool streamChunked) override;
    void           Close(ConnectionContext* ctx, bool forceClose = false)                        override;
    
public: // Main Functions
    void Run()                                                                                 override;
    void Stop()                                                                                override;
    void RefreshExpiry(ConnectionContext* ctx, std::uint16_t timeoutSeconds)                   override;
    bool RefreshAsyncTimer(ConnectionContext* ctx, std::uint32_t delayMs, AsyncData asyncData) override;

private: // Helper Functions
    ConnectionContext* GetConnection(std::uint16_t endpointIndex = 0xFFFF);
    void               ReleaseConnection(ConnectionContext* ctx, bool freeOnly = false);
    
    std::uint64_t      NowMs();
    bool               SetNonBlocking(int fd);
    bool               EnsureFileReady(ConnectionContext* ctx, std::string path);
    bool               EnsureReadReady(ConnectionContext* ctx);
    bool               ResolveHost(const char* host, const char* port, sockaddr_storage* outAddr, socklen_t* outLen);
    bool               ResolveIP(const sockaddr_storage& inAddr, WFXIpAddress& out);

    void               Receive(ConnectionContext* ctx);
    void               SendFile(ConnectionContext* ctx);
    void               ResumeStream(ConnectionContext* ctx);
    void               HandleAsyncCallback(ConnectionContext* ctx, AsyncResult res, bool destroy);
    void               HandleTimeoutTimer(int sfd);
    void               HandleAsyncTimer(int sfd);
    void               HandleHandshake(ConnectionContext* ctx, std::uint32_t ev);
    void               HandleWriteReady(ConnectionContext* ctx, std::uint32_t ev);
    void               UpdateAsyncTimer();

    std::uint64_t      PackEpollData(ConnectionContext* ctx);
    bool               RegisterEpoll(ConnectionContext* ctx, int op);
    bool               TryHandshake(ConnectionContext* ctx, EventType onSuccess);
    bool               CreateAndConnect(ConnectionContext* ctx, EndpointContext& epCtx);
    
    void               WrapAccept(ConnectionContext* ctx);
    EndpointStatus     WrapConnect(ConnectionContext* cctx, EndpointContainer& ecnt);
    ssize_t            WrapRead(ConnectionContext* ctx, char* buf, std::size_t len);
    ssize_t            WrapWrite(ConnectionContext* ctx, const char* buf, std::size_t len);
    ssize_t            WrapFile(ConnectionContext* ctx, int fd, off_t* offset, std::size_t count);

private: // Misc
    Config&            config_     = GetConfig();
    Logger&            logger_     = GetLogger();
    FileCache&         fileCache_  = GetFileCache();
    BufferPool&        pool_       = GetBufferPool();

    IpLimiter          ipLimiter_  = {pool_};
    ReceiveCallback    onReceive_  = {};
    std::atomic<bool>  running_    = true;
    bool               useHttps_   = false;

private: // Constexpr stuff
    constexpr static char          CHUNK_END[]            = "0\r\n\r\n";
    constexpr static ssize_t       SWITCH_FILE_TO_STREAM  = std::numeric_limits<ssize_t>::min();
    constexpr static std::uint16_t MAX_DISTINCT_ENDPOINTS = std::numeric_limits<std::uint16_t>::max() - 1;
    constexpr static std::uint16_t CLIENT_CONNECTION_TAG  = 0xFFFF; // Tag to differentiate between client and endpoint

    constexpr static int INVOKE_TIMEOUT_COOLDOWN = 5; // In seconds
    constexpr static int INVOKE_TIMEOUT_DELAY    = 1; // In seconds

private: // Timeout handler
    TimerWheel              timerWheel_;
    TimerHeap               timerHeap_;
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
    ConnectionPool connections_ = {config_.networkConfig.maxConnections};
    EndpointPool   endpoints_   = {};

    // TODO: FOR DEBUG ONLY, REMOVE IT AFTER
    std::uint64_t numConnectionsAlive_ = 0;
};

} // namespace WFX::OSSpecific

#endif // WFX_LINUX_EPOLL_CONNECTION_HPP

#endif // !WFX_LINUX_USE_IO_URING