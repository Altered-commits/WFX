// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_LINUX_USE_IO_URING

#ifndef WFX_LINUX_EPOLL_CONNECTION_HPP
#define WFX_LINUX_EPOLL_CONNECTION_HPP

#include "config/config.hpp"
#include "http/connection/http_connection.hpp"
#include "http/limits/ip_limiter/ip_limiter.hpp"
#include "http/ssl/http_ssl.hpp"
#include "utils/diagnostics/metric_tracer.hpp"
#include "utils/fileops/filecache.hpp"
#include "utils/pool/bitmap_pool.hpp"
#include "utils/timer/timer_wheel.hpp"
#include "utils/timer/timer_heap.hpp"

#include <sys/epoll.h>
#include <atomic>

namespace WFX::OSSpecific {

using namespace WFX::Http;   // For 'HttpConnectionHandler', 'ReceiveCallback', 'ClientCtx', ...
using namespace WFX::Utils;  // For 'Logger', 'RWBuffer', ...
using namespace WFX::Core;   // For 'Config'
using namespace WFX::Shared; // For 'EndpointStatus', 'AsyncData', ...

using SteadyClock = std::chrono::steady_clock;
using ClientPool = BitmapPool<ClientCtx>;
using EndpointPool = BitmapPool<EndpointCtx>;

// One entry per registered endpoint: metadata + fixed connection pool
struct EndpointEntry {
    EndpointMetadata meta;
    EndpointPool pool;

    explicit EndpointEntry(std::uint32_t connLimit) : pool(connLimit)
    {}

    // BitmapPool is move-only, so EndpointEntry must be too
    EndpointEntry(EndpointEntry&&) = default;
    EndpointEntry& operator=(EndpointEntry&&) = default;
    EndpointEntry(const EndpointEntry&) = delete;
    EndpointEntry& operator=(const EndpointEntry&) = delete;
};

class EpollConnectionHandler : public HttpConnectionHandler {
public:
    EpollConnectionHandler(bool useHttps);
    ~EpollConnectionHandler();

public: // Initializing
    void Initialize(const std::string& host, std::uint16_t port) override;
    void SetEngineCallback(ReceiveCallback onData) override;
    std::uint16_t AllocateEndpoint(const char* host, EndpointDesc desc, EndpointConfig config) override;

public: // Client operations
    void ResumeReceive(ClientCtx* ctx) override;
    void Write(ClientCtx* ctx, std::string_view buffer = {}) override;
    void WriteFile(ClientCtx* ctx, std::string path) override;
    void Stream(ClientCtx* ctx, StreamGenerator generator, bool streamChunked = true) override;
    void Close(ClientCtx* ctx, bool forceClose = false) override;
    void RefreshExpiry(ClientCtx* ctx, std::uint16_t timeoutSeconds) override;
    bool RefreshAsyncTimer(ClientCtx* ctx, std::uint32_t delayMs, AsyncData asyncData) override;

public: // Endpoint operations
    EndpointStatus SendPayload(ClientCtx* clientCtx, std::uint16_t endpointIdx, const void* req,
                               AsyncData asyncData) override;
    void SlotSend(EndpointCtx* slotCtx, const void* data, std::uint32_t size, AsyncData asyncData) override;
    void SlotReceive(EndpointCtx* slotCtx, AsyncData asyncData) override;
    void Close(EndpointCtx* ctx, bool forceClose = false, DisconnectReason reason = DisconnectReason::ERROR) override;
    void RefreshExpiry(EndpointCtx* ctx, std::uint16_t timeoutSeconds) override;

public: // Engine control
    void Run() override;
    void Stop() override;

private: // Connection management
    ClientCtx* GetClientConnection();
    EndpointCtx* GetEndpointConnection(std::uint16_t endpointIdx);
    void ReleaseClient(ClientCtx* ctx);
    void ReleaseEndpoint(EndpointCtx* ctx, DisconnectReason reason = DisconnectReason::ERROR);
    void ReturnEndpointToPool(EndpointCtx* ctx);

private: // I/O
    bool Receive(ClientCtx* ctx);
    bool Receive(EndpointCtx* ctx);
    bool EnsureReadReady(ClientCtx* ctx);
    bool EnsureReadReady(EndpointCtx* ctx);
    bool EnsureFileReady(ClientCtx* ctx, std::string path);
    void SendFile(ClientCtx* ctx);
    void ResumeStream(ClientCtx* ctx);

private: // Write paths (private, public Write is client-only)
    void Write(EndpointCtx* ctx);

private: // Epoll dispatch
    void HandleClientEvent(ClientCtx* ctx, std::uint32_t ev, std::uint16_t gen);
    void HandleEndpointEvent(EndpointCtx* ctx, std::uint32_t ev, std::uint16_t gen);
    void HandleClientEpollIn(ClientCtx* ctx);
    void HandleEndpointEpollIn(EndpointCtx* ctx);
    void HandleClientWriteReady(ClientCtx* ctx, std::uint32_t ev);
    void HandleEndpointWriteReady(EndpointCtx* ctx, std::uint32_t ev);

private: // Handshake
    void HandleClientHandshake(ClientCtx* ctx, std::uint32_t ev);
    void HandleEndpointHandshake(EndpointCtx* ctx, std::uint32_t ev);

    // Both ClientCtx and EndpointCtx have sslConn + eventType. Template is the-
    // -cleanest option here without duplicating 8 lines twice
    template <typename Ctx> bool TryHandshake(Ctx* ctx, EventType onSuccess, EventType stayState)
    {
        switch(sslHandler_->Handshake(ctx->sslConn)) {
            case SSLReturn::SUCCESS:
                ctx->eventType = onSuccess;
                return true;

            case SSLReturn::WANT_READ:
            case SSLReturn::WANT_WRITE:
                ctx->eventType = stayState;
                return true;

            default:
                return false;
        }
    }

private: // Endpoint-specific
    void FinalizeEndpointRequest(EndpointCtx* ctx, EndpointDesc& desc, bool success);
    void HandleEndpointWriteComplete(EndpointCtx* ctx);
    void HandleEndpointReceive(EndpointCtx* ctx);
    void HandlePrewarm();
    void HandleDnsRefresh(std::uint16_t endpointIdx);
    void FireOnConnect(EndpointCtx* ctx, EndpointEntry& entry);

private: // Wrap / low-level
    void WrapAccept(ClientCtx* ctx);
    EndpointStatus WrapConnect(EndpointCtx* ctx, EndpointEntry& entry);
    bool CreateAndConnect(EndpointCtx* ctx, EndpointMetadata& epCtx);

    // WrapRead / WrapWrite take socket + sslConn directly
    ssize_t WrapRead(WFXSocket socket, void* sslConn, char* buf, std::size_t len);
    ssize_t WrapWrite(WFXSocket socket, void* sslConn, const char* buf, std::size_t len);
    ssize_t WrapFile(ClientCtx* ctx, int fd, off_t* offset, std::size_t count);

private: // Epoll helpers
    std::uint64_t PackEpollData(ClientCtx* ctx);
    std::uint64_t PackEpollData(EndpointCtx* ctx);
    bool RegisterEpoll(ClientCtx* ctx, int op);
    bool RegisterEpoll(EndpointCtx* ctx, int op);

private: // Timer
    void HandleTimeoutTimer(int sfd);
    void HandleAsyncTimer(int sfd);
    void UpdateAsyncTimer();

private: // Async callback
    // Both typed variants forward to this shared impl
    void AsyncCallbackImpl(void* ctxPtr, AsyncData& async, AsyncResult res, bool destroy);
    void HandleClientAsyncCallback(ClientCtx* ctx, AsyncResult res, bool destroy);
    void HandleEndpointAsyncCallback(EndpointCtx* ctx, AsyncResult res, bool destroy);

private: // Misc
    std::uint64_t NowMs();
    bool SetNonBlocking(int fd);
    bool ResolveTLSFromAuto(std::uint16_t port);
    bool ResolveHost(const char* host, const char* port, sockaddr_storage* outAddr, socklen_t* outLen);
    bool ResolveIP(const sockaddr_storage& inAddr, WFXIpAddress& out);

private: // Singletons / config
    Config& config_ = GetConfig();
    Logger& logger_ = GetLogger();
    FileCache& fileCache_ = GetFileCache();
    BufferPool& pool_ = GetBufferPool();
    WorkerMetrics* metrics_ = MetricTracer::Current();

    IpLimiter ipLimiter_ = {pool_};
    ReceiveCallback onReceive_ = {};
    std::atomic<bool> running_ = true;
    bool useHttps_ = false;

    int dnsEventFd_ = -1;

private: // Constants
    constexpr static char CHUNK_END[] = "0\r\n\r\n";
    constexpr static ssize_t SWITCH_FILE_TO_STREAM = std::numeric_limits<ssize_t>::min();
    constexpr static std::uint16_t MAX_DISTINCT_ENDPOINTS = std::numeric_limits<std::uint16_t>::max() - 1;
    constexpr static std::uint16_t CLIENT_CONNECTION_TAG = 0xFFFF;

    constexpr static int INVOKE_TIMEOUT_COOLDOWN = 5;
    constexpr static int INVOKE_TIMEOUT_DELAY = 1;

private: // Timer state
    TimerWheel timerWheel_;
    TimerHeap timerHeap_;
    SteadyClock::time_point startTime_ = SteadyClock::now();
    int timeoutTimerFd_ = -1;
    int asyncTimerFd_ = -1;

private: // Epoll + SSL
    int listenFd_ = -1;
    int epollFd_ = -1;
    std::uint16_t maxEvents_ = config_.osSpecificConfig.maxEvents;

    std::unique_ptr<HttpWFXSSL> sslHandler_ = nullptr;
    std::unique_ptr<epoll_event[]> events_ = nullptr;

private: // Pools
    ClientPool connections_ = {config_.networkConfig.maxConnections};
    std::vector<EndpointEntry> endpoints_ = {};

private: // Static
    static void OnSlotConnected(void* ud, AsyncResult result);
    static EpollConnectionHandler* instance_;
};

} // namespace WFX::OSSpecific

#endif // WFX_LINUX_EPOLL_CONNECTION_HPP

#endif // !WFX_LINUX_USE_IO_URING