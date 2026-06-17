#ifndef WFX_MACOS_KQUEUE_CONNECTION_HPP
#define WFX_MACOS_KQUEUE_CONNECTION_HPP

#include "config/config.hpp"
#include "http/connection/http_connection.hpp"
#include "http/limits/ip_limiter/ip_limiter.hpp"
#include "http/ssl/http_ssl.hpp"
#include "utils/diagnostics/metric_tracer.hpp"
#include "utils/fileops/filecache.hpp"
#include "utils/pool/bitmap_pool.hpp"
#include "utils/timer/timer_wheel.hpp"
#include "utils/timer/timer_heap.hpp"

#include <sys/event.h>
#include <atomic>

namespace WFX::OSSpecific {

using namespace WFX::Http;
using namespace WFX::Utils;
using namespace WFX::Core;
using namespace WFX::Shared;

using SteadyClock = std::chrono::steady_clock;
using ConnectionPool = BitmapPool<ConnectionContext>;

using EndpointContainer = std::pair<EndpointContext, ConnectionPool>;
using EndpointPool = std::vector<EndpointContainer>;

class KqueueConnectionHandler : public HttpConnectionHandler {
public:
    KqueueConnectionHandler(bool useHttps);
    ~KqueueConnectionHandler();

public:
    void Initialize(const std::string& host, std::uint16_t port) override;
    void SetEngineCallback(ReceiveCallback onData) override;
    std::uint16_t AllocateEndpoint(std::string_view host, std::string_view port, std::uint32_t cLimit,
                                   std::uint32_t ifLimit, bool useTLS) override;

public:
    void ResumeReceive(ConnectionContext* ctx) override;
    void Write(ConnectionContext* ctx, std::string_view buffer = {}) override;
    void WriteFile(ConnectionContext* ctx, std::string path) override;
    EndpointStatus WriteEndpoint(ConnectionContext* ctx, std::uint32_t endpointIndex, const std::byte* ptr,
                                 std::uint32_t size) override;
    void Stream(ConnectionContext* ctx, StreamGenerator generator, bool streamChunked) override;
    void Close(ConnectionContext* ctx, bool forceClose = false) override;

public:
    void Run() override;
    void Stop() override;
    void RefreshExpiry(ConnectionContext* ctx, std::uint16_t timeoutSeconds) override;
    bool RefreshAsyncTimer(ConnectionContext* ctx, std::uint32_t delayMs, AsyncData asyncData) override;

private: // Helper Functions
    ConnectionContext* GetConnection(std::uint16_t endpointIndex = 0xFFFF);
    void ReleaseConnection(ConnectionContext* ctx, bool freeOnly = false);

    std::uint64_t NowMs();
    bool SetNonBlocking(int fd);
    bool SetNoSigPipe(int fd);
    bool EnsureFileReady(ConnectionContext* ctx, std::string path);
    bool EnsureReadReady(ConnectionContext* ctx);
    bool ResolveHost(const char* host, const char* port, sockaddr_storage* outAddr, socklen_t* outLen);
    bool ResolveIP(const sockaddr_storage& inAddr, WFXIpAddress& out);

    void Receive(ConnectionContext* ctx);
    void SendFile(ConnectionContext* ctx);
    void ResumeStream(ConnectionContext* ctx);
    void HandleAsyncCallback(ConnectionContext* ctx, AsyncResult res, bool destroy);
    void HandleTimeoutTimer();
    void HandleAsyncTimer();
    void HandleHandshake(ConnectionContext* ctx, std::int16_t filter);
    void HandleWriteReady(ConnectionContext* ctx, std::int16_t filter);
    void UpdateAsyncTimer();

    std::uint64_t PackKqueueData(ConnectionContext* ctx);
    bool RegisterKqueue(ConnectionContext* ctx, int op);
    bool TryHandshake(ConnectionContext* ctx, EventType onSuccess);
    bool CreateAndConnect(ConnectionContext* ctx, EndpointContext& epCtx);

    void WrapAccept(ConnectionContext* ctx);
    EndpointStatus WrapConnect(ConnectionContext* cctx, EndpointContainer& ecnt);
    void DrainAllConnections();
    ConnectionContext* EnsureAcceptSlot();
    ssize_t WrapRead(ConnectionContext* ctx, char* buf, std::size_t len);
    ssize_t WrapWrite(ConnectionContext* ctx, const char* buf, std::size_t len);
    ssize_t WrapFile(ConnectionContext* ctx, int fd, off_t* offset, std::size_t count);

private:
    Config& config_ = GetConfig();
    Logger& logger_ = GetLogger();
    FileCache& fileCache_ = GetFileCache();
    BufferPool& pool_ = GetBufferPool();
    WorkerMetrics* metrics_ = nullptr;

    IpLimiter ipLimiter_ = {pool_};
    ReceiveCallback onReceive_ = {};
    std::atomic<bool> running_ = true;
    bool useHttps_ = false;

private:
    constexpr static char CHUNK_END[] = "0\r\n\r\n";
    constexpr static ssize_t SWITCH_FILE_TO_STREAM = std::numeric_limits<ssize_t>::min();
    constexpr static std::uint16_t MAX_DISTINCT_ENDPOINTS = std::numeric_limits<std::uint16_t>::max() - 1;
    constexpr static std::uint16_t CLIENT_CONNECTION_TAG = 0xFFFF;

    constexpr static int INVOKE_TIMEOUT_COOLDOWN = 5;
    constexpr static int INVOKE_TIMEOUT_DELAY = 1;

    constexpr static uintptr_t TIMEOUT_TIMER_IDENT = UINTPTR_MAX - 1;
    constexpr static uintptr_t ASYNC_TIMER_IDENT = UINTPTR_MAX - 2;

    constexpr static int KQ_ADD = 0;
    constexpr static int KQ_MOD = 1;
    constexpr static int KQ_DEL = 2;
    constexpr static int KQ_ADD_WRITE = 3;
    constexpr static int KQ_DROP_WRITE = 4;
    constexpr static int KQ_REARM_READ = 5;

    void FinishWriteCycle(ConnectionContext* ctx);

private:
    TimerWheel timerWheel_;
    TimerHeap timerHeap_;
    SteadyClock::time_point startTime_ = SteadyClock::now();

private:
    int listenFd_ = -1;
    int kqFd_ = -1;
    std::uint16_t maxEvents_ = config_.osSpecificConfig.maxEvents;

    std::unique_ptr<HttpWFXSSL> sslHandler_ = nullptr;
    std::unique_ptr<struct kevent[]> events_ = nullptr;

private:
    ConnectionPool connections_ = {config_.networkConfig.maxConnections};
    EndpointPool endpoints_ = {};
};

} // namespace WFX::OSSpecific

#endif // WFX_MACOS_KQUEUE_CONNECTION_HPP
