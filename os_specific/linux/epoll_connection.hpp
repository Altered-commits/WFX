// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

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
#include <type_traits>

// IMPORTANT: these headers are used exclusively for the DNS background-refresh-
// -mechanism (short-lived std::threads capped by a counting_semaphore, handing-
// -results back to the epoll thread via a mutex-guarded queue + eventfd). The-
// -rest of the engine is single-threaded by design and must never need locking;-
// -do not reach for these outside the DNS refresh path
#include <mutex>
#include <semaphore>

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

// DNS refresh result, posted from a background resolver thread back to the epoll-
// -thread via dnsResultEventFd_. One entry per completed refresh
struct DnsResult {
    bool success;
    std::uint16_t endpointIdx;
    std::uint32_t minTtlSeconds;
    ResolvedAddrs addrs;
    std::string wakeupError;
};

// A slot parked for a backoff reconnect, drained on the timeout tick once wakeAtMs is due
// generationId guards against the slot being freed and reused under the same pool index
struct PendingReconnect {
    std::uint64_t wakeAtMs;
    std::uint16_t endpointIdx;
    std::uint16_t generationId;
    std::uint32_t slotIdx;
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
    StringView NegotiatedProtocol(EndpointCtx* slotCtx) override;
    void Close(EndpointCtx* ctx, bool forceClose = false, DisconnectReason reason = DisconnectReason::ERROR) override;
    void RefreshExpiry(EndpointCtx* ctx, std::uint16_t timeoutSeconds) override;

public: // Engine control
    void Run() override;
    void Stop() override;

private: // Connection management
    ClientCtx* GetClientConnection();
    EndpointCtx* GetEndpointConnection(std::uint16_t endpointIdx);
    EndpointCtx* FindMultiplexableSlot(std::uint16_t endpointIdx, EndpointMetadata& meta);
    void ReleaseClient(ClientCtx* ctx);
    void ReleaseEndpoint(EndpointCtx* ctx, DisconnectReason reason = DisconnectReason::ERROR);
    void ReturnEndpointToPool(EndpointCtx* ctx);

private: // I/O
    bool EnsureFileReady(ClientCtx* ctx, const std::string& path);
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

private: // Shared ClientCtx/EndpointCtx templates
    // Drives one SSL handshake step off its return value: success sets 'onSuccess',-
    // -WANT_READ/WANT_WRITE sets 'stayState' and waits for the next epoll event
    template <typename Ctx> bool TryHandshake(Ctx* ctx, EventType onSuccess, EventType stayState);

    // Arms EPOLLIN|EPOLLOUT|EPOLLET, or issues EPOLL_CTL_DEL. PackEpollData is overloaded-
    // -per ctx type; that's the only thing that actually differs between the two
    template <typename Ctx> bool RegisterEpoll(Ctx* ctx, int op);

    // Lazily allocates ctx->rwBuffer's read side, closing ctx on allocation failure
    template <typename Ctx> bool EnsureReadReady(Ctx* ctx);

    // Drains the socket until EAGAIN (ET mode). outEof (endpoint only): set true when the-
    // -peer closed mid-receive instead of closing the slot, so the caller can run one last-
    // -isEof parse to finalize a close-delimited body
    template <typename Ctx> bool Receive(Ctx* ctx, bool* outEof = nullptr);

    // Shared SSL-shutdown state machine behind both public Close() overrides. Returns true-
    // -once synchronous cleanup (RegisterEpoll DEL + Release*) should run now; false means-
    // -either there was nothing to do, or the event loop must wait for an in-progress-
    // -shutdown to finish (the shutdown eventType is already armed on ctx in that case)
    template <typename Ctx> bool CloseCommon(Ctx* ctx, bool forceClose);

private: // Endpoint-specific
    void ValidateEndpoint(const char* host, const EndpointDesc& desc, const EndpointConfig& config);
    std::uint64_t ComputeNextDnsRefresh(std::uint32_t minTtlSeconds, std::uint32_t userOverrideSeconds,
                                        const std::string& hostname);
    void FinalizeEndpointRequest(EndpointCtx* ctx, EndpointMetadata& meta, bool success);
    // Split out of SendPayload so the non-multiplexed path (the overwhelming majority of-
    // -endpoints) stays byte-for-byte untouched. Only called when desc.hasCapacity is set
    EndpointStatus SendPayloadMultiplexed(ClientCtx* clientCtx, std::uint16_t endpointIdx, const void* req,
                                          AsyncData asyncData, EndpointEntry& entry, std::uint64_t pendingCoalesceKey);
    // Shared serialize step used by both SendPayload/SendPayloadMultiplexed and-
    // -FlushDeferredRequest (a fresh connect with onConnect defers serializing until the-
    // -handshake finishes, see EndpointCtx::pendingConnectReq). Returns EndpointStatus::SUCCESS-
    // -on success; caller does its own cleanup on any other status
    EndpointStatus SerializeSingleSlot(EndpointCtx* slotCtx, EndpointMetadata& meta, const void* req);
    EndpointStatus SerializeMultiplexed(EndpointCtx* slotCtx, EndpointMetadata& meta, const void* req,
                                        std::uint64_t* streamKey);
    void HandleEndpointWriteComplete(EndpointCtx* ctx);
    void HandleEndpointReceive(EndpointCtx* ctx, bool isEof);
    // Resolves and erases a single completed stream from ctx->pendingStreams (fires its client-
    // -callback / coalesce waiters, destroys its parse state), leaving the shared slot and every-
    // -other in-flight stream on it untouched. No-op if key isn't found (already resolved/stale)
    void ResolveMultiplexedStream(EndpointCtx* ctx, EndpointEntry& entry, std::uint64_t key);
    void HandlePrewarm();

    void HandleConnectFailure(EndpointCtx* ctx, EndpointEntry& entry, bool fatal,
                              DisconnectReason reason = DisconnectReason::ERROR);
    void ScheduleReconnect(EndpointCtx* ctx, EndpointEntry& entry);
    void HandleReconnects();
    std::uint32_t ComputeBackoffSeconds(const EndpointConfig& config, std::uint16_t attempt);

    void HandleDnsRefresh(std::uint16_t endpointIdx);
    void HandleDnsResultReady(int sfd);
    void FireOnConnect(EndpointCtx* ctx, EndpointEntry& entry);

    // Serializes a request that SendPayload/SendPayloadMultiplexed deferred until onConnect-
    // -succeeded (see EndpointCtx::pendingConnectReq). Returns false on a serialize failure,-
    // -having already notified the client and torn the slot down via FailDeferredRequest
    bool FlushDeferredRequest(EndpointCtx* ctx, EndpointEntry& entry);
    void FailDeferredRequest(EndpointCtx* ctx, EndpointStatus status);

private: // Wrap / low-level
    void WrapAccept(ClientCtx* ctx);
    EndpointStatus WrapConnect(EndpointCtx* ctx, EndpointEntry& entry);
    EndpointStatus CreateAndConnect(EndpointCtx* ctx, EndpointMetadata& epCtx);

    // WrapRead / WrapWrite take socket + sslConn directly
    ssize_t WrapRead(WFXSocket socket, void* sslConn, char* buf, std::size_t len);
    ssize_t WrapWrite(WFXSocket socket, void* sslConn, const char* buf, std::size_t len);
    ssize_t WrapFile(ClientCtx* ctx, int fd, off_t* offset, std::size_t count);

private: // Epoll helpers
    std::uint64_t PackEpollData(ClientCtx* ctx);
    std::uint64_t PackEpollData(EndpointCtx* ctx);

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
    WorkerMetrics* metrics_ = MetricTracer::Current();

    IpLimiter ipLimiter_;
    ReceiveCallback onReceive_ = {};
    std::atomic<bool> running_ = true;
    bool useHttps_ = false;

private: // Constants
    constexpr static char CHUNK_END[] = "0\r\n\r\n";
    constexpr static ssize_t SWITCH_FILE_TO_STREAM = std::numeric_limits<ssize_t>::min();
    constexpr static std::uint16_t MAX_DISTINCT_ENDPOINTS = std::numeric_limits<std::uint16_t>::max() - 1;
    constexpr static std::uint16_t CLIENT_CONNECTION_TAG = 0xFFFF;

    constexpr static int INVOKE_TIMEOUT_COOLDOWN = 5;
    constexpr static int INVOKE_TIMEOUT_DELAY = 1;

    // Sane bounds regardless of source: never hammer DNS faster than 5s (protects-
    // -against a misconfigured/malicious 0-1s TTL), never wait longer than 1hr (bounds-
    // -staleness even if TTL is absurdly large or userOverride is set very high)
    constexpr static std::uint32_t MIN_REFRESH_SECONDS = 5;
    constexpr static std::uint32_t MAX_REFRESH_SECONDS = 3600;

    // Caps concurrent background DNS resolver threads. 32 provides enough parallelism-
    // -for large deployments (hundreds of endpoints) while staying well within OS thread-
    // -limits on all supported hardware
    constexpr static std::uint16_t MAX_DNS_THREADS = 32;
    constexpr static std::uint32_t MAX_DNS_RESULT_QUEUE_SIZE = MAX_DNS_THREADS * 2;

private: // Timer state
    TimerWheel timerWheel_;
    TimerHeap timerHeap_;
    SteadyClock::time_point startTime_ = SteadyClock::now();
    int timeoutTimerFd_ = -1;
    int asyncTimerFd_ = -1;

private: // Reconnect state (single-threaded, drained on the timeout tick)
    std::vector<PendingReconnect> pendingReconnects_;
    std::uint64_t reconnectRngState_ = 0x9E3779B97F4A7C15ULL; // xorshift state for backoff jitter

private: // DNS state
    std::mutex dnsResultMutex_;
    std::counting_semaphore<MAX_DNS_THREADS> dnsThreadSemaphore_{MAX_DNS_THREADS};
    std::vector<DnsResult> dnsResultQueue_;
    int dnsResultEventFd_ = -1;

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
    static EpollConnectionHandler* GlobalInstance;
};

} // namespace WFX::OSSpecific

#endif // WFX_LINUX_EPOLL_CONNECTION_HPP