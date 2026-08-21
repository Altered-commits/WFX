// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "epoll_connection.hpp"

#include "config/config.hpp"
#include "http/common/http_error_msgs.hpp"
#include "http/response/http_response.hpp"
#include "http/ssl/http_ssl_factory.hpp"
#include "shared/apis/http_api.hpp"
#include "shared/utils/memory.hpp"
#include "utils/diagnostics/crash_tracer.hpp"
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <fcntl.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h>

// Used exclusively for DNS background-refresh resolver threads, see HandleDnsRefresh
#include <thread>

namespace WFX::OSSpecific {

// vvv Helper Functions vvv
template <typename Fn> static auto RetryOnEintr(Fn&& fn) -> decltype(fn())
{
    // Retries a syscall-style operation while it fails with EINTR. 'fn' should perform exactly
    // one attempt of the underlying syscall and return its raw result (whatever a successful
    // call returns, typically >= 0). On any error other than EINTR, returns immediately with
    // that result so the caller can inspect errno itself.
    while(true) {
        auto result = fn();
        if(result >= 0 || errno != EINTR)
            return result;
    }
}

static constexpr EndpointStatus DisconnectReasonToStatus(DisconnectReason reason)
{
    switch(reason) {
        case DisconnectReason::HANDSHAKE_TIMEOUT:
            return EndpointStatus::HANDSHAKE_TIMEOUT;
        case DisconnectReason::TIMEOUT:
            return EndpointStatus::REQUEST_TIMEOUT;
        default:
            return EndpointStatus::INTERNAL_ERROR;
    }
}

// vvv Shared Templates vvv
template <typename Ctx> bool EpollConnectionHandler::TryHandshake(Ctx* ctx, EventType onSuccess, EventType stayState)
{
    switch(sslHandler_->Handshake(ctx->sslConn)) {
        case SSLReturn::SUCCESS:
            EnterState(ctx, onSuccess);
            return true;

        case SSLReturn::WANT_READ:
        case SSLReturn::WANT_WRITE:
            EnterState(ctx, stayState);
            return true;

        default:
            return false;
    }
}

template <typename Ctx> bool EpollConnectionHandler::RegisterEpoll(Ctx* ctx, int op)
{
    // Poll once, then we just won't touch 'epoll_ctl' again till we close connection
    // We will use 'ctx->eventType' to control the flow of data pretty much, preventing
    // any sort of race condition and such.
    // NOTE: For deletion cases, event must be 'nullptr'
    epoll_event ev{};
    epoll_event* evPtr = nullptr;

    if(op != EPOLL_CTL_DEL) {
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
        ev.data.u64 = PackEpollData(ctx);
        evPtr = &ev;
    }

    // Like any other syscall, this can be interrupted by signals, in which case just retry
    // For other errors just return false
    return RetryOnEintr([&] { return epoll_ctl(epollFd_, op, ctx->socket, evPtr); }) == 0;
}

template <typename Ctx> bool EpollConnectionHandler::EnsureReadReady(Ctx* ctx)
{
    auto& rwBuffer = ctx->rwBuffer;
    auto& netCfg = config_.networkConfig;

    if(rwBuffer.IsReadInitialized())
        return true;

    if(!rwBuffer.InitReadBuffer(netCfg.readBufferIncSize)) {
        logger_.Error("[Epoll]: Failed to init read buffer");
        Close(ctx);
        return false;
    }

    return true;
}

template <typename Ctx> bool EpollConnectionHandler::Receive(Ctx* ctx, bool* outEof)
{
    WFX_TRACE();

    if(!EnsureReadReady(ctx))
        return false;

    auto& rwBuffer = ctx->rwBuffer;
    bool gotData = false;

    constexpr EventType RECV_STATE =
        std::is_same_v<Ctx, ClientCtx> ? EventType::EVENT_RECV : EventType::EVENT_ENDPOINT_RECV;

    // Drain loop (ET mode: must read until EAGAIN)
    while(true) {
        ValidRegion region = rwBuffer.GetWritableReadRegion();
        if(!region.ptr || region.len == 0) {
            // ClientCtx routes through its own GrowReadBuffer, which rebases anything the parser
            // may have already pointed into the old buffer. This loop stays agnostic to what that
            // is, it just defers to ClientCtx instead of touching rwBuffer directly for that case.
            const bool grew = [&] {
                if constexpr(std::is_same_v<Ctx, ClientCtx>)
                    return ctx->GrowReadBuffer(config_.networkConfig.readBufferIncSize,
                                               config_.networkConfig.maxReadBufferSize);
                else
                    return rwBuffer.GrowReadBuffer(config_.networkConfig.readBufferIncSize,
                                                   config_.networkConfig.maxReadBufferSize);
            }();

            if(!grew) {
                // Fatal for one endless response, but not where bytes drain piece by piece
                // Stop reading and leave the rest in the socket so TCP stalls the sender
                if constexpr(std::is_same_v<Ctx, EndpointCtx>) {
                    if(IsIncrementallyConsumed(ctx))
                        break;
                }

                logger_.Warn("[Epoll]: Read buffer full, closing connection");
                Close(ctx);
                return false;
            }

            region = rwBuffer.GetWritableReadRegion();
        }

        const ssize_t n = WrapRead(ctx->socket, ctx->sslConn, region.ptr, region.len);

        // Fully handle SSL + TCP edge-triggered
        if(n > 0) {
            rwBuffer.AdvanceReadLength(n);
            gotData = true;
        }

        // Connection closed by peer
        else if(n == 0) {
            // If the caller can finalize on EOF (the request RECV path), don't tear the slot
            // down here. Report EOF and let it run one last isEof parse so a close-delimited
            // body (no Content-Length, no chunked) can be delivered before teardown.
            if(outEof) {
                *outEof = true;
                break;
            }

            Close(ctx);
            return false;
        }

        else {
            // Done reading for now, wait for more data in future
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                // A coroutine suspended in SlotReceive owns eventType until it resumes. Taking it
                // here strands it: no bytes fires no completion, and the next EPOLLIN routes to
                // the RECV handler instead.
                if constexpr(std::is_same_v<Ctx, EndpointCtx>) {
                    if(ctx->inOnConnectPhase)
                        break;
                }

                // Endpoint and client use different receive states so HandleWriteReady
                // and HandleEpollIn can route without an IsEndpoint() check.
                EnterState(ctx, RECV_STATE);
                break;
            }

            // Fatal error
            Close(ctx);
            return false;
        }
    }

    return gotData;
}

template <typename Ctx> bool EpollConnectionHandler::CloseCommon(Ctx* ctx, bool forceClose)
{
    if(!ctx)
        return false;

    // Force close bypasses any in-progress shutdown or state checks
    if(!forceClose && ctx->isShuttingDown)
        return false;

    ctx->isShuttingDown = 1;

    if(!ctx->sslConn)
        return true;

    // Skip clean shutdown, nuke it immediately
    if(forceClose) {
        sslHandler_->ForceShutdown(ctx->sslConn);
        ctx->sslConn = nullptr;
        return true;
    }

    auto res = sslHandler_->Shutdown(ctx->sslConn);

    // Shutdown finished or failed immediately. Proceed to synchronous cleanup
    if(res == SSLReturn::SUCCESS || res == SSLReturn::FATAL) {
        ctx->sslConn = nullptr;
        return true;
    }

    // Wait for the event loop to complete the shutdown. Endpoint and client shutdowns
    // are distinct so the event loop can route them without an 'IsEndpoint()' check.
    if constexpr(std::is_same_v<Ctx, ClientCtx>)
        EnterState(ctx, EventType::EVENT_SHUTDOWN);
    else
        EnterState(ctx, EventType::EVENT_ENDPOINT_SHUTDOWN);

    return false;
}
// ^^^ Shared ClientCtx/EndpointCtx templates ^^^

// Used by 'OnSlotConnected' to call back into the engine without a capture
EpollConnectionHandler* EpollConnectionHandler::GlobalInstance = nullptr;

// vvv Constructor & Destructor vvv
EpollConnectionHandler::EpollConnectionHandler(bool useHttps) : useHttps_(useHttps)
{
    GlobalInstance = this;

    // Decorrelate backoff jitter across worker processes. Without per-process entropy every worker
    // would share the same xorshift sequence and reconnect in lockstep, re-creating the thundering
    // herd the jitter exists to prevent. Mix pid + a clock + this; OR-in 1 so the state is never 0
    // (0 is the xorshift fixed point).
    std::uint64_t seed = static_cast<std::uint64_t>(::getpid());
    seed ^= static_cast<std::uint64_t>(SteadyClock::now().time_since_epoch().count()) * 0x9E3779B97F4A7C15ULL;
    seed ^= reinterpret_cast<std::uintptr_t>(this);
    reconnectRngState_ = seed | 1ULL;

    if(useHttps)
        sslHandler_ = CreateSSLHandler(true);
}

bool EpollConnectionHandler::EnsureClientSSL()
{
    // Outbound TLS is independent of what the server itself speaks, so the handler may not exist
    // yet on a plaintext server.
    // Built without the inbound context, which is the part that needs a certificate
    if(!sslHandler_)
        sslHandler_ = CreateSSLHandler(false);

    return sslHandler_ && sslHandler_->EnsureClientContext();
}

EpollConnectionHandler::~EpollConnectionHandler()
{
    // Free all endpoint slot allocations before fds close and before BufferPool
    // potentially destructs ahead of us. void* fields have no destructors so we
    // must call the user-supplied hooks explicitly.
    for(auto& entry : endpoints_) {
        auto& desc = entry.meta.desc;

        if(entry.meta.cachedTlsSession && sslHandler_) {
            sslHandler_->FreeCachedSession(entry.meta.cachedTlsSession);
            entry.meta.cachedTlsSession = nullptr;
        }

        for(std::uint32_t i = 0; i < entry.pool.GetSlots(); i++) {
            EndpointCtx* ctx = entry.pool.GetPtr(i);
            ctx->rwBuffer.ResetBuffer();
            if(ctx->parseStateObj && desc.destroyParseState) {
                desc.destroyParseState(ctx->parseStateObj);
                ctx->parseStateObj = nullptr;
            }
            if(ctx->outputObj && desc.destroyOutput) {
                desc.destroyOutput(ctx->outputObj);
                ctx->outputObj = nullptr;
            }
            if(ctx->slotState && desc.destroySlotState) {
                desc.destroySlotState(ctx->slotState);
                ctx->slotState = nullptr;
            }
        }
    }

    // Free all client slot allocations (rwBuffer + any still-live requestInfo/responseInfo, e.g.
    // a keep-alive connection that was still open when the server was asked to stop)
    for(std::uint32_t i = 0; i < connections_.GetSlots(); i++)
        connections_.GetPtr(i)->Reset();

    if(listenFd_ > 0) {
        close(listenFd_);
        listenFd_ = -1;
    }
    if(timeoutTimerFd_ > 0) {
        close(timeoutTimerFd_);
        timeoutTimerFd_ = -1;
    }
    if(dnsResultEventFd_ > 0) {
        close(dnsResultEventFd_);
        dnsResultEventFd_ = -1;
    }
    if(asyncTimerFd_ > 0) {
        close(asyncTimerFd_);
        asyncTimerFd_ = -1;
    }
    if(epollFd_ > 0) {
        close(epollFd_);
        epollFd_ = -1;
    }

    logger_.Info("[Epoll]: Cleaned up resources successfully");
}

// vvv Initializing Functions vvv
void EpollConnectionHandler::Initialize(const std::string& host, std::uint16_t port)
{
    WFX_TRACE();

    auto& osConfig = config_.osSpecificConfig;
    auto& networkConfig = config_.networkConfig;

    // Initialize memory for epoll events
    events_ = std::make_unique<epoll_event[]>(maxEvents_);

    // Resolve the address to either AF_INET6 or AF_INET
    sockaddr_storage addr;
    socklen_t addrLen;

    char portStr[6];
    auto [ptr, err] = std::to_chars(portStr, portStr + sizeof(portStr), port);
    if(err != std::errc{})
        logger_.Fatal("[Epoll]: Failed to convert port to string representation while resolving host");

    // Null terminate port
    *ptr = '\0';

    if(!ResolveHost(host.c_str(), portStr, &addr, &addrLen))
        logger_.Fatal("[Epoll]: Failed to resolve host '", host, '\'');

    // Socket family depends on what 'ResolveHost' gave us (AF_INET or AF_INET6)
    listenFd_ = socket(addr.ss_family, SOCK_STREAM, 0);
    if(listenFd_ < 0)
        logger_.Fatal("[Epoll]: Failed to create listening socket: ", strerror(errno));

    // If we got an AF_INET6 socket, we must explicitly disable IPV6_V6ONLY to allow
    // it to accept connections from both IPv4 and IPv6 clients.
    if(addr.ss_family == AF_INET6) {
        int no = 0;
        if(setsockopt(listenFd_, IPPROTO_IPV6, IPV6_V6ONLY, (void*)&no, sizeof(no)) < 0)
            logger_.Fatal("[Epoll]: Failed to disable IPV6_V6ONLY: ", strerror(errno));
    }

    // Set other socket options
    int opt = 1;
    if(setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        logger_.Fatal("[Epoll]: Failed to set SO_REUSEADDR: ", strerror(errno));

    if(setsockopt(listenFd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0)
        logger_.Fatal("[Epoll]: Failed to set SO_REUSEPORT: ", strerror(errno));

    if(!SetNonBlocking(listenFd_))
        logger_.Fatal("[Epoll]: Failed to make listening socket non-blocking: ", strerror(errno));

    // Finally bind and listen on the socket
    if(bind(listenFd_, (sockaddr*)&addr, addrLen) < 0)
        logger_.Fatal("[Epoll]: Failed to bind socket: ", strerror(errno));

    if(listen(listenFd_, static_cast<int>(osConfig.backlog)) < 0)
        logger_.Fatal("[Epoll]: Failed to listen: ", strerror(errno));

    epollFd_ = epoll_create1(0);
    if(epollFd_ < 0)
        logger_.Fatal("[Epoll]: Failed to create epoll: ", strerror(errno));

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listenFd_;
    if(epoll_ctl(epollFd_, EPOLL_CTL_ADD, listenFd_, &ev) < 0)
        logger_.Fatal("[Epoll]: Failed to add listening socket to epoll: ", strerror(errno));

    // vvv Initialize timeout handler vvv
    timerWheel_
        .Init(connections_.GetSlots(), 4096, 1, TimeUnit::SECONDS, [this](std::uint32_t connId, std::uint32_t extra) {
            // 'extra' contains a 16-bit value:
            //   >= CLIENT_CONNECTION_TAG -> client connection
            //   <  CLIENT_CONNECTION_TAG -> endpoint connection (index)
            if(extra >= CLIENT_CONNECTION_TAG) {
                ClientCtx* ctx = connections_.GetPtr(connId);

                // So the logic behind the if condition is, in normal sync path, if a connection is marked
                // 'close', it will trigger cleanup after it sent data so no need to clash with it. But on
                // the other hand, in the async / endpoint path, if a connection is marked 'close' and the
                // callback, for some odd reason, just hung up and isn't responding, we shouldn't care
                // about connection atp. WE CLOSE IT OURSELVES.
                if(ctx->GetConnectionState() != ConnectionState::CONNECTION_CLOSE || ctx->IsAsyncOperation())
                    Close(ctx, true);
            }
            else {
                // connId is an absolute timer wheel index. auxPool has its own range
                // (auxTimerBase onward), a side connection never reconnects/backs off.
                // FailAuxConnect resumes a still-suspended caller (mid-connect, or the
                // caller simply forgot to Close() after a successful handoff) before
                // tearing down; safe even if nothing is pending (no-op resume then close).
                auto& entry = endpoints_[extra];
                if(connId >= entry.meta.auxTimerBase) {
                    FailAuxConnect(entry.auxPool.GetPtr(connId - entry.meta.auxTimerBase), SlotStatus::IO_ERROR);
                    return;
                }

                EndpointCtx* ctx = entry.pool.GetPtr(connId - entry.meta.timerBase);

                // A timeout during the connect phase (TCP connect / TLS handshake /
                // onConnect) is a transient connect failure: route it through the funnel
                // so a background slot reconnects with backoff and a client-waiting slot
                // fails fast. A timeout during request/idle just closes as before.
                const EventType et = ctx->eventType;
                const bool connectPhase = et == EventType::EVENT_CONNECT || et == EventType::EVENT_ENDPOINT_HANDSHAKE ||
                                          et == EventType::EVENT_ENDPOINT_ONCONNECT;

                if(connectPhase)
                    HandleConnectFailure(ctx, entry, false, DisconnectReason::HANDSHAKE_TIMEOUT);
                else
                    Close(ctx, true, DisconnectReason::TIMEOUT);
            }
        });

    // Re-expand for any endpoints registered before Initialize was called
    for(auto& entry : endpoints_)
        timerWheel_.Expand(entry.pool.GetSlots() + entry.auxPool.GetSlots());

    timeoutTimerFd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if(timeoutTimerFd_ < 0)
        logger_.Fatal("[Epoll]: Failed to create timeout timer: ", strerror(errno));

    itimerspec ts{};
    ts.it_interval.tv_sec = INVOKE_TIMEOUT_COOLDOWN;
    ts.it_interval.tv_nsec = 0;
    ts.it_value.tv_sec = INVOKE_TIMEOUT_DELAY;
    ts.it_value.tv_nsec = 0;

    if(timerfd_settime(timeoutTimerFd_, 0, &ts, nullptr) < 0)
        logger_.Fatal("[Epoll]: Failed to set timeout timer: ", strerror(errno));

    epoll_event tev{};
    tev.events = EPOLLIN;
    tev.data.u64 = static_cast<std::uint64_t>(timeoutTimerFd_) & 0xFFFFFFFFULL; // Lower 32 bits = fd, upper 32 = 0
    if(epoll_ctl(epollFd_, EPOLL_CTL_ADD, timeoutTimerFd_, &tev) < 0)
        logger_.Fatal("[Epoll]: Failed to add timeout timer to epoll: ", strerror(errno));

    // vvv Initializing async timer vvv
    asyncTimerFd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if(asyncTimerFd_ < 0)
        logger_.Fatal("[Epoll]: Failed to create async timer: ", strerror(errno));

    epoll_event aev{};
    aev.events = EPOLLIN;
    aev.data.u64 = static_cast<std::uint64_t>(asyncTimerFd_) & 0xFFFFFFFFULL; // Lower 32 bits = fd, upper 32 = 0
    if(epoll_ctl(epollFd_, EPOLL_CTL_ADD, asyncTimerFd_, &aev) < 0)
        logger_.Fatal("[Epoll]: Failed to add async timer to epoll: ", strerror(errno));

    // vvv Initializing DNS refresh event vvv
    dnsResultEventFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if(dnsResultEventFd_ < 0)
        logger_.Fatal("[Epoll]: Failed to create DNS result eventfd: ", strerror(errno));

    epoll_event dnsEv{};
    dnsEv.events = EPOLLIN;
    dnsEv.data.u64 = static_cast<std::uint64_t>(dnsResultEventFd_) & 0xFFFFFFFFULL; // Lower 32 bits = fd, upper 32 = 0
    if(epoll_ctl(epollFd_, EPOLL_CTL_ADD, dnsResultEventFd_, &dnsEv) < 0)
        logger_.Fatal("[Epoll]: Failed to add DNS result eventfd to epoll: ", strerror(errno));
}

void EpollConnectionHandler::SetEngineCallbacks(ClientCtxCallback onData, ClientCtxCallback onClose)
{
    onReceive_ = std::move(onData);
    onClose_ = std::move(onClose);
}

std::uint16_t EpollConnectionHandler::AllocateEndpoint(const char* host, EndpointDesc desc, EndpointConfig config)
{
    WFX_TRACE();

    if(endpoints_.size() >= MAX_DISTINCT_ENDPOINTS)
        logger_.Fatal("[Epoll]: Too many distinct domain endpoints registered");

    // The endpoint index doubles as the per-endpoint metrics slot, so the metrics cap is the real
    // limit here. Hard-fail rather than assign an index past the array and read garbage later.
    const std::uint16_t maxEndpoints = config_.metricsConfig.maxEndpoints;
    if(endpoints_.size() >= maxEndpoints)
        logger_.Fatal("[Epoll]: Cannot register endpoint '", host, "', all ", maxEndpoints,
                      " endpoint metric slots are taken. Raise '[Metrics] max_endpoints' in wfx.toml");

    ValidateEndpoint(host, desc, config);

    // Scheme prefixes are not allowed. Use FORCE_REQUIRE or FORCE_INSECURE for
    // non-standard ports, or rely on port heuristics with AUTO.
    const std::string_view hostView{host};

    if(hostView.find("://") != std::string_view::npos)
        logger_.Fatal("[Epoll]: Endpoint host must not contain a scheme prefix, got: ", host,
                      ". Use 'hostname:port' format and set tlsConfig explicitly if needed");

    // Parse "hostname:port" (rfind handles IPv6 addresses correctly)
    auto colonPos = hostView.rfind(':');
    if(colonPos == std::string_view::npos)
        logger_.Fatal("[Epoll]: Endpoint host must be in 'host:port' format, got: ", host);

    std::string hostname{hostView.substr(0, colonPos)};
    std::string portStr{hostView.substr(colonPos + 1)};

    std::uint16_t port = 0;
    auto [ptr, ec] = std::from_chars(portStr.data(), portStr.data() + portStr.size(), port);
    if(ec != std::errc{})
        logger_.Fatal("[Epoll]: Failed to parse port from endpoint host: ", host);

    const std::uint16_t endpointIdx = static_cast<std::uint16_t>(endpoints_.size());
    std::uint32_t timerBase = connections_.GetSlots();

    // 'timerBase' used in RefreshExpiry(EndpointCtx*) overload, do check it out for info
    if(!endpoints_.empty()) {
        auto& last = endpoints_.back();
        timerBase = last.meta.timerBase + last.pool.GetSlots() + last.auxPool.GetSlots();
    }

    auto& entry = endpoints_.emplace_back(config.connLimit, config.auxConnLimit, config.exactSlots);
    auto& meta = entry.meta;
    auto& pool = entry.pool;
    auto& auxPool = entry.auxPool;

    meta.timerBase = timerBase;
    meta.auxTimerBase = timerBase + pool.GetSlots();
    meta.desc = desc;
    meta.config = config;
    meta.hostname = std::move(hostname);
    meta.port = port;

    const bool useTLS = EndpointUsesTls(config, port);

    // A TLS endpoint needs the outbound context whatever the server itself speaks
    // Brought up here so a broken SSL setup fails at startup rather than on the first request
    if(useTLS && !EnsureClientSSL())
        logger_.Fatal("[Epoll]: Failed to initialize client SSL for endpoint: ", host);

    // Resolve AFTER hostname/port are set on meta
    std::uint32_t minTtl = 0;
    if(!DNSResolver::Resolve(meta.hostname.c_str(), meta.port, meta.addrs, minTtl))
        logger_.Fatal("[Epoll]: Failed to resolve endpoint: ", host);

    meta.dnsNextRefreshSeconds = ComputeNextDnsRefresh(minTtl, config.dnsRefreshSeconds, meta.hostname);

    auto initCtx = [&](EndpointCtx* ctx, bool isAux) {
        ctx->endpointIdx = endpointIdx;
        ctx->isSideConnection = isAux;
        ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
        ctx->SetEndpointState(useTLS ? EndpointState::ENDPOINT_SECURE : EndpointState::ENDPOINT_INSECURE);
    };

    for(std::uint32_t i = 0; i < pool.GetSlots(); i++)
        initCtx(pool.GetPtr(i), false);

    for(std::uint32_t i = 0; i < auxPool.GetSlots(); i++)
        initCtx(auxPool.GetPtr(i), true);

    logger_.Info("[Epoll]: Endpoint allocated -- host='", meta.hostname, "' port=", meta.port,
                 " endpointIdx=", endpointIdx, " tls=", (useTLS ? "yes" : "no"), " connLimit=", config.connLimit,
                 " prewarm=", config.prewarm, " addrs=", meta.addrs.size(),
                 " nextDnsRefreshInSeconds=", (meta.dnsNextRefreshSeconds - NowMs() / 1000));

    return endpointIdx;
}

std::uint16_t EpollConnectionHandler::EndpointCount() const
{
    return static_cast<std::uint16_t>(endpoints_.size());
}

StringView EpollConnectionHandler::EndpointHostAt(std::uint16_t endpointIdx) const
{
    if(endpointIdx >= endpoints_.size())
        return {};

    const auto& hostname = endpoints_[endpointIdx].meta.hostname;
    return StringView{hostname.data(), static_cast<std::uint64_t>(hostname.size())};
}

// vvv Core I/O Operations vvv
void EpollConnectionHandler::ResumeReceive(ClientCtx* ctx)
{
    if(!EnsureReadReady(ctx))
        return;

    // We are ready to receive data now, set 'eventType' to EVENT_RECV
    EnterState(ctx, EventType::EVENT_RECV);
}

void EpollConnectionHandler::Write(ClientCtx* ctx, std::string_view msg)
{
    WFX_TRACE();

    // Case 1: Direct send (used only for static error codes)
    // NOTE: CHANGE OF PLANS, msg is fire and forget, i don't care if they get delivered
    // or not, if u want good error messages u will go the hard route anyways (res.Status().SendText()...)
    if(!msg.empty()) {
        (void)WrapWrite(ctx->socket, ctx->sslConn, msg.data(), msg.size());
        // We ignore result intentionally. If state says close -> close, else -> resume receive
        goto __CleanupOrRearm;
    }

    // Case 2: Send from write buffer
    else {
        auto* writeMeta = ctx->rwBuffer.GetWriteMeta();
        if(!writeMeta || writeMeta->writtenLength >= writeMeta->dataLength)
            goto __CleanupOrRearm;

        // PENDING: EnterState already done, DrainWriteBuffer arms EVENT_SEND. FAILED: Close
        // already done. Either way there's nothing left for this call to do.
        if(DrainWriteBuffer(ctx) != FlushStatus::COMPLETED)
            return;
    }

__CleanupOrRearm:
    // Special case, stream operation, stream the content via streamGenerator
    if(ctx->isStreamOperation) {
        ResumeStream(ctx);
        return;
    }

    // Special case, file operation, send it before anything else
    if(ctx->isFileOperation) {
        SendFile(ctx);
        return;
    }

    if(ctx->GetConnectionState() == ConnectionState::CONNECTION_CLOSE)
        Close(ctx);
    else {
        ctx->Clear();
        ResumeReceive(ctx);
    }
}

void EpollConnectionHandler::WriteFile(ClientCtx* ctx, std::string path)
{
    // Before we proceed, ensure stuffs ready for file operation
    if(!EnsureFileReady(ctx, path)) {
        ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
        Write(ctx, HttpError::INTERNAL_ERROR);
        return;
    }

    // Cool so for file operation, we first send headers, mark it as file operation
    // So when 'Write' completes, it should send file without any issue
    ctx->isFileOperation = 1;
    Write(ctx, {});
}

void EpollConnectionHandler::Stream(ClientCtx* ctx, StreamGenerator generator, bool streamChunked)
{
    // Sanity checks
    if(!generator.ctx || !generator.next) {
        logger_.Warn("[Epoll]: 'Stream()' called but received empty generator");
        Close(ctx);
        return;
    }

    // Store the generator function in context for future use
    ctx->streamGenerator = generator;

    // For streaming operations, we first want to finish writing out headers
    // and mark it as stream operation, so when 'Write' completes, it should
    // start the streaming process.
    ctx->isStreamOperation = 1;
    ctx->streamChunked = streamChunked;
    Write(ctx, {});
}

FlushStatus EpollConnectionHandler::FlushChunk(ClientCtx* ctx, bool isFinal, AsyncData onDone)
{
    WFX_TRACE();

    HttpResponse* res = ctx->responseInfo;
    if(!res || res->GetBodyKind() != BodyKind::AWAIT_STREAM) {
        Close(ctx);
        return FlushStatus::FAILED;
    }

    if(!isFinal && !res->HasPendingFlushData())
        return FlushStatus::COMPLETED;

    if(!res->PrepareFlushChunk(isFinal)) {
        Close(ctx);
        return FlushStatus::FAILED;
    }

    ctx->awaitFlushFinal = isFinal ? 1 : 0;

    const FlushStatus status = DrainWriteBuffer(ctx);

    if(status == FlushStatus::PENDING) {
        ctx->isAwaitFlush = 1;
        ctx->asyncData = onDone;
        return status;
    }

    if(status == FlushStatus::COMPLETED && !CompleteFlushRound(ctx))
        return FlushStatus::FAILED;

    return status;
}

void EpollConnectionHandler::Close(ClientCtx* ctx, bool forceClose)
{
    WFX_TRACE();

    if(!CloseCommon(ctx, forceClose))
        return;

    (void)RegisterEpoll(ctx, EPOLL_CTL_DEL);
    ReleaseClient(ctx);
}

void EpollConnectionHandler::Close(EndpointCtx* ctx, bool forceClose, DisconnectReason disconnectReason)
{
    WFX_TRACE();

    if(!CloseCommon(ctx, forceClose))
        return;

    (void)RegisterEpoll(ctx, EPOLL_CTL_DEL);
    ReleaseEndpoint(ctx, disconnectReason);
}

// vvv Endpoint Operations vvv
std::uint64_t EpollConnectionHandler::EncodeSlotHandle(EndpointCtx* slotCtx)
{
    // endpointIdx (16) | pool index (32) | generationId (16). generationId is never 0, so a
    // valid handle is never 0 either, which is what lets 0 mean "no pinned slot".
    const std::uint64_t idx = endpoints_[slotCtx->endpointIdx].pool.GetIndex(slotCtx);

    return (static_cast<std::uint64_t>(slotCtx->endpointIdx) << 48) | (idx << 16) | slotCtx->generationId;
}

EndpointCtx* EpollConnectionHandler::DecodeSlotHandle(std::uint64_t pinnedSlot)
{
    if(pinnedSlot == 0)
        return nullptr;

    const auto endpointIdx = static_cast<std::uint16_t>(pinnedSlot >> 48);
    const auto slotIdx = static_cast<std::uint32_t>((pinnedSlot >> 16) & 0xFFFFFFFFULL);
    const auto generationId = static_cast<std::uint16_t>(pinnedSlot & 0xFFFFULL);

    if(endpointIdx >= endpoints_.size())
        return nullptr;

    auto& entry = endpoints_[endpointIdx];
    if(slotIdx >= entry.pool.GetSlots())
        return nullptr;

    EndpointCtx* slotCtx = entry.pool.GetPtr(slotIdx);

    // A generation mismatch means this slot was torn down and recycled for an unrelated
    // connection since the handle was issued; isReserved catches an already-released handle.
    if(!slotCtx || slotCtx->generationId != generationId || !slotCtx->isReserved)
        return nullptr;

    return slotCtx;
}

std::uint64_t EpollConnectionHandler::ReserveSlot(std::uint16_t endpointIdx)
{
    WFX_TRACE();

    if(endpointIdx >= endpoints_.size())
        return 0;

    // Same allocation every request already uses; the only difference is that isReserved stops
    // the completion path from freeing the bitmap bit afterwards, so the pool can't hand this
    // slot to anyone else until ReleaseSlot.
    EndpointCtx* slotCtx = GetEndpointConnection(endpointIdx);
    if(!slotCtx)
        return 0;

    slotCtx->isReserved = 1;

    return EncodeSlotHandle(slotCtx);
}

void EpollConnectionHandler::ReleaseSlot(std::uint64_t pinnedSlot)
{
    WFX_TRACE();

    EndpointCtx* slotCtx = DecodeSlotHandle(pinnedSlot);
    if(!slotCtx)
        return;

    slotCtx->isReserved = 0;

    // Released mid-request: leave it alone, the completion path now sees isReserved cleared and
    // returns the slot to the pool itself once that request finishes.
    if(slotCtx->clientCtx)
        return;

    // Reserved but never actually connected (nothing was ever sent on it): there's no live
    // connection to hand back, so tear the slot down instead of idling it.
    if(slotCtx->GetConnectionState() == ConnectionState::CONNECTION_CLOSE)
        ReleaseEndpoint(slotCtx);
    else
        ReturnEndpointToPool(slotCtx);
}

EndpointStatus EpollConnectionHandler::ArmSendOrConnect(EndpointCtx* slotCtx, EndpointEntry& entry, bool freshConnect)
{
    // Final dispatch step both send paths end in. A closed slot starts the connect sequence,
    // an already-open one re-arms epoll for writing and swaps idle for request timeout.
    if(freshConnect)
        return WrapConnect(slotCtx, entry);

    EnterState(slotCtx, EventType::EVENT_ENDPOINT_SEND);
    RefreshExpiry(slotCtx, entry.meta.config.requestTimeoutSeconds);

    if(RegisterEpoll(slotCtx, EPOLL_CTL_MOD))
        return EndpointStatus::PENDING;

    logger_.Error("[Epoll]: 'ArmSendOrConnect -> RegisterEpoll(MOD)' failed for endpoint ", entry.meta.hostname, ": ",
                  strerror(errno));

    return EndpointStatus::EPOLL_ERROR;
}

EndpointStatus EpollConnectionHandler::SerializeSingleSlot(EndpointCtx* slotCtx, EndpointMetadata& meta,
                                                           const void* req)
{
    // Serializes req into the FULL write buffer (non-multiplexed slots only ever hold one
    // request at a time, so clearing first is safe), growing it as needed. Caller sets up
    // the clientCtx link and dispatches to WrapConnect / RegisterEpoll on success.
    auto& desc = meta.desc;
    auto& rwBuf = slotCtx->rwBuffer;

    rwBuf.ClearWriteBuffer();

    while(true) {
        auto* writeMeta = rwBuf.GetWriteMeta();
        std::uint32_t written = 0;
        std::uint64_t unusedStreamKey = 0; // desc.hasCapacity is null on this path, always ignored

        const SerializeResult sr = desc.serialize(slotCtx->slotState, req, rwBuf.GetWriteData(), writeMeta->bufferSize,
                                                  &written, &unusedStreamKey);

        if(sr == SerializeResult::OK) {
            writeMeta->dataLength = written;
            writeMeta->writtenLength = 0;
            endpointMetrics_[slotCtx->endpointIdx].bytesOut += written;
            return EndpointStatus::SUCCESS;
        }

        if(sr == SerializeResult::BUFFER_TOO_SMALL) {
            if(!rwBuf.GrowWriteBuffer(config_.networkConfig.sendBufferIncSize, config_.networkConfig.maxSendBufferSize))
                return EndpointStatus::INSUFFICIENT_BUFFER;

            continue;
        }

        return EndpointStatus::SERIALIZE_ERROR;
    }
}

EndpointStatus EpollConnectionHandler::SerializeMultiplexed(EndpointCtx* slotCtx, EndpointMetadata& meta,
                                                            const void* req, std::uint64_t* streamKey)
{
    // Serializes req into the TAIL of the write buffer (never clears it: other streams on this
    // multiplexed slot may still have bytes queued between writtenLength and dataLength that
    // must survive). Only advances dataLength once a valid streamKey comes back, so a rejected
    // serialize never corrupts bytes another in-flight stream is relying on. Caller still owns
    // registering the result in pendingStreams / coalescePending.
    auto& desc = meta.desc;
    auto& rwBuf = slotCtx->rwBuffer;

    // Reclaim bytes already on the wire before appending this stream, otherwise the append-only
    // tail silts dataLength up to the ceiling and every serialize past that fails INSUFFICIENT_BUFFER.
    rwBuf.CompactWriteBuffer();

    *streamKey = 0;

    while(true) {
        auto region = rwBuf.GetWritableWriteRegion();
        std::uint32_t written = 0;

        const SerializeResult sr = desc.serialize(slotCtx->slotState, req, region.ptr,
                                                  static_cast<std::uint32_t>(region.len), &written, streamKey);

        if(sr == SerializeResult::OK) {
            if(*streamKey == 0) {
                // Protocol contract violation: hasCapacity is set, so serialize() must always
                // assign a real key. Bail before touching dataLength so the bytes just written
                // are simply overwritten by the next attempt instead of corrupting the shared
                // connection's framing.
                logger_.Error("[Epoll]: multiplexed 'serialize' returned OK with streamKey=0 for endpoint ",
                              meta.hostname);
                return EndpointStatus::SERIALIZE_ERROR;
            }

            rwBuf.GetWriteMeta()->dataLength += written;
            endpointMetrics_[slotCtx->endpointIdx].bytesOut += written;
            return EndpointStatus::SUCCESS;
        }

        if(sr == SerializeResult::BUFFER_TOO_SMALL) {
            if(!rwBuf.GrowWriteBuffer(config_.networkConfig.sendBufferIncSize, config_.networkConfig.maxSendBufferSize))
                return EndpointStatus::INSUFFICIENT_BUFFER;

            continue;
        }

        return EndpointStatus::SERIALIZE_ERROR;
    }
}

EndpointStatus EpollConnectionHandler::SendPayload(ClientCtx* clientCtx, std::uint16_t endpointIdx, const void* req,
                                                   AsyncData asyncData, std::uint64_t pinnedSlot)
{
    if(endpointIdx >= endpoints_.size())
        return EndpointStatus::INVALID_KEY;

    auto& em = endpointMetrics_[endpointIdx];
    auto& entry = endpoints_[endpointIdx];
    auto& meta = entry.meta;
    auto& desc = meta.desc;

    // Resolved up front: a handle whose slot has since been recycled must fail the request
    // rather than silently falling back to an unrelated pooled connection.
    EndpointCtx* reservedCtx = nullptr;
    if(pinnedSlot != 0) {
        reservedCtx = DecodeSlotHandle(pinnedSlot);
        if(!reservedCtx)
            return EndpointStatus::INVALID_KEY;
    }

    std::uint64_t pendingCoalesceKey = 0;

    // If an identical in-flight request exists, park the client as a waiter
    // Key is computed once here and reused below when registering the primary
    // Do NOT set clientCtx->endpointCtx for waiters (ReleaseClient would otherwise
    // kill the in-flight slot when a waiter disconnects).
    // NOTE: Never coalesce a pinned send. The point of pinning is that these requests run on one
    // specific connection, and two callers holding separate reservations can observe different
    // connection-scoped state (an open transaction, session settings) despite identical bytes.
    if(!reservedCtx && desc.coalesceKey) {
        pendingCoalesceKey = desc.coalesceKey(req);
        if(pendingCoalesceKey != 0) {
            auto it = meta.coalescePending.find(pendingCoalesceKey);
            if(it != meta.coalescePending.end()) {
                it->second.waiters.push_back({clientCtx, clientCtx->generationId});
                clientCtx->asyncData = asyncData;
                em.coalesceHits++;
                return EndpointStatus::PENDING;
            }
        }
    }

    // Every request reaching here is a primary (waiters returned above), so stamp its start once
    // for latency. Set on the client, not the slot: it holds across whichever path serves the
    // request, and stays right even for a multiplexed slot carrying several clients at once.
    // Gated on latency being on so the clock read is never paid otherwise
    if(MetricTracer::LatencyEnabled())
        clientCtx->endpointStartUs = NowUs();

    // Multiplexing already shares one connection across concurrent requests, so pinning has
    // nothing to add there and the two would fight over slot ownership.
    if(desc.hasCapacity) {
        if(reservedCtx)
            return EndpointStatus::INVALID_KEY;

        return SendPayloadMultiplexed(clientCtx, endpointIdx, req, asyncData, entry, pendingCoalesceKey);
    }

    // A pinned send reuses its reserved slot; everything downstream (serialize, connect,
    // completion) is identical either way.
    EndpointCtx* slotCtx = reservedCtx ? reservedCtx : GetEndpointConnection(endpointIdx);
    if(!slotCtx) {
        em.poolExhausted++;
        return EndpointStatus::POOL_EXHAUSTED;
    }

    // Per-slot state survives across requests. Only create if not already present
    if(!slotCtx->slotState && desc.createSlotState)
        slotCtx->slotState = desc.createSlotState(desc.userCtx);

    // Per-request objects are fresh every time
    if(!slotCtx->parseStateObj && desc.createParseState)
        slotCtx->parseStateObj = desc.createParseState(slotCtx->slotState);

    if(!slotCtx->outputObj && desc.createOutput)
        slotCtx->outputObj = desc.createOutput(slotCtx->slotState);

    // A fresh connect with an onConnect hook must run the handshake before anything else
    // touches the write buffer. Serializing the request now and handing it to WrapConnect
    // would let FireOnConnect's own Send() append the handshake bytes AFTER the request
    // bytes already queued here, so the request would reach the wire before the handshake.
    // Defer: stash req and let FlushDeferredRequest serialize it once onConnect is READY
    const bool freshConnect = slotCtx->GetConnectionState() == ConnectionState::CONNECTION_CLOSE;

    if(desc.onConnect && freshConnect) {
        slotCtx->pendingConnectReq = req;
        slotCtx->coalesceKey = pendingCoalesceKey; // registered for real once FlushDeferredRequest serializes it
    }
    else {
        auto& rwBuf = slotCtx->rwBuffer;
        if(!rwBuf.IsWriteInitialized() && !rwBuf.InitWriteBuffer(config_.networkConfig.maxSendBufferSize)) {
            FinalizeEndpointRequest(slotCtx, meta, false);
            Close(slotCtx, true);
            return EndpointStatus::BUFFER_ERROR;
        }

        const EndpointStatus sr = SerializeSingleSlot(slotCtx, meta, req);
        if(sr != EndpointStatus::SUCCESS) {
            FinalizeEndpointRequest(slotCtx, meta, false);
            Close(slotCtx, true);
            return sr;
        }

        // Register in coalesce map after successful serialize (master)
        if(pendingCoalesceKey != 0) {
            auto& ce = meta.coalescePending[pendingCoalesceKey];
            ce.inflight = slotCtx;
            // ce.waiters starts empty, pushed to by subsequent SendPayload calls with the same key
            slotCtx->coalesceKey = pendingCoalesceKey;
        }
    }

    // Create a communication link between endpoint and client
    slotCtx->clientCtx = clientCtx;
    clientCtx->endpointCtx = slotCtx;
    clientCtx->asyncData = asyncData;

    const EndpointStatus result = ArmSendOrConnect(slotCtx, entry, freshConnect);

    // N U L L; so Close() -> ReleaseEndpoint() doesn't ALSO try to break the bad news to this client
    if(result != EndpointStatus::PENDING) {
        slotCtx->clientCtx = nullptr;
        clientCtx->endpointCtx = nullptr;

        // Count the attempt and its failure class, same as the async path would have
        em.requests++;

        RecordEndpointSendFailure(endpointIdx, result);
        FinalizeEndpointRequest(slotCtx, meta, false);
        Close(slotCtx, true);
    }
    else
        em.requests++;

    return result;
}

EndpointStatus EpollConnectionHandler::SendPayloadMultiplexed(ClientCtx* clientCtx, std::uint16_t endpointIdx,
                                                              const void* req, AsyncData asyncData,
                                                              EndpointEntry& entry, std::uint64_t pendingCoalesceKey)
{
    auto& em = endpointMetrics_[endpointIdx];
    auto& meta = entry.meta;
    auto& desc = meta.desc;

    // Prefer an already-open slot with spare capacity over opening a new connection: keeping
    // the connection count low is sort of the entire point of multiplexing.
    EndpointCtx* slotCtx = FindMultiplexableSlot(endpointIdx, meta);
    bool freshSlot = false;

    if(!slotCtx) {
        slotCtx = GetEndpointConnection(endpointIdx);
        if(!slotCtx) {
            em.poolExhausted++;
            return EndpointStatus::POOL_EXHAUSTED;
        }

        freshSlot = true;
    }

    // Per-slot (connection-level) state survives across requests, same as the single-slot path
    if(!slotCtx->slotState && desc.createSlotState)
        slotCtx->slotState = desc.createSlotState(desc.userCtx);

    // Per-request parse scratch belongs to THIS request alone, not the slot: a busy slot may
    // have several of these alive at once, tracked in slotCtx->pendingStreams. Output is NOT
    // created here: the protocol owns per-stream output internally (keyed by the streamKey it
    // assigns below) and only hands it to the engine via takeStreamOutput once finished.
    void* reqParseState = desc.createParseState ? desc.createParseState(slotCtx->slotState) : nullptr;
    std::uint64_t streamKey = 0; // assigned by serialize(); stays 0 when the request is deferred below

    auto cleanupReqParseState = [&]() {
        if(reqParseState && desc.destroyParseState)
            desc.destroyParseState(reqParseState);
    };

    // Only reachable when freshSlot is true: FindMultiplexableSlot only ever returns
    // already-connected slots. A fresh connect with an onConnect hook must run the
    // handshake before the request touches the write buffer (see SendPayload for why),
    // so defer serialize() to FlushDeferredRequest once onConnect is READY. slotState
    // fields that would normally only matter on the single-slot path (clientCtx,
    // coalesceKey, parseStateObj) are unused here otherwise, so they double as storage
    // for this stream's pending bits until FlushDeferredRequest moves them into a real
    // PendingStream entry.
    const bool freshConnect = slotCtx->GetConnectionState() == ConnectionState::CONNECTION_CLOSE;

    if(desc.onConnect && freshConnect) {
        slotCtx->pendingConnectReq = req;
        slotCtx->parseStateObj = reqParseState;
        slotCtx->coalesceKey = pendingCoalesceKey;
        slotCtx->clientCtx = clientCtx;
    }
    else {
        auto& rwBuf = slotCtx->rwBuffer;
        if(!rwBuf.IsWriteInitialized() && !rwBuf.InitWriteBuffer(config_.networkConfig.maxSendBufferSize)) {
            cleanupReqParseState();
            if(freshSlot)
                ReleaseEndpoint(slotCtx);

            return EndpointStatus::BUFFER_ERROR;
        }

        const EndpointStatus sr = SerializeMultiplexed(slotCtx, meta, req, &streamKey);
        if(sr != EndpointStatus::SUCCESS) {
            cleanupReqParseState();
            if(freshSlot)
                ReleaseEndpoint(slotCtx);

            return sr;
        }

        // Register in coalesce map after successful serialize (master). Per-stream, not per-slot:
        // PendingStream::coalesceKey, not EndpointCtx::coalesceKey, since several concurrently
        // in-flight streams on this one slot may each be coalescing under a different key.
        if(pendingCoalesceKey != 0) {
            auto& ce = meta.coalescePending[pendingCoalesceKey];
            ce.inflight = slotCtx;
        }

        if(!slotCtx->pendingStreams)
            slotCtx->pendingStreams = New<PendingStreamMap>();

        (*slotCtx->pendingStreams)[streamKey] =
            PendingStream{clientCtx, reqParseState, pendingCoalesceKey, clientCtx->generationId};

        clientCtx->streamKey = streamKey;

        // A reused idle slot goes back in use now that it carries a stream again
        MultiplexReacquireLease(slotCtx);
    }

    clientCtx->endpointCtx = slotCtx;
    clientCtx->asyncData = asyncData;

    // freshConnect is only reachable when freshSlot is true: FindMultiplexableSlot only ever
    // returns already-connected slots.
    const EndpointStatus result = ArmSendOrConnect(slotCtx, entry, freshConnect);

    if(result != EndpointStatus::PENDING) {
        clientCtx->endpointCtx = nullptr;
        clientCtx->streamKey = 0;

        // Count the attempt and its failure class, same as the async path would have
        em.requests++;
        RecordEndpointSendFailure(endpointIdx, result);

        if(pendingCoalesceKey != 0)
            meta.coalescePending.erase(pendingCoalesceKey);

        // Deferred case: WrapConnect failed before serialize() ever ran, so the protocol never
        // saw this request at all. Nothing was registered in pendingStreams, just drop what
        // was stashed on the slot and tear it down (freshSlot is always true here).
        if(desc.onConnect && freshConnect) {
            slotCtx->pendingConnectReq = nullptr;
            slotCtx->clientCtx = nullptr;
            slotCtx->coalesceKey = 0;
            slotCtx->parseStateObj = nullptr;
            cleanupReqParseState();
            ReleaseEndpoint(slotCtx);
        }
        else {
            slotCtx->pendingStreams->erase(streamKey);
            cleanupReqParseState();

            // Tell the protocol the engine is abandoning this stream so it can drop whatever
            // internal tracking it started at serialize() time. Matters most for the shared-slot
            // case below, where the slot (and the protocol's connection-level state) lives on.
            void* abandoned = desc.takeStreamOutput(slotCtx->slotState, streamKey);
            if(abandoned && desc.destroyOutput)
                desc.destroyOutput(abandoned);

            // A brand new slot that never even finished connecting has nothing else relying on
            // it, tear it down entirely. A shared slot that merely failed to re-arm epoll for
            // THIS request stays alive for every other stream still in flight on it.
            if(freshSlot)
                Close(slotCtx, true);
            // Erasing this stream may have emptied a reused slot, hand its lease back to idle
            else
                MultiplexReleaseLeaseIfIdle(slotCtx);
        }
    }
    else
        em.requests++;

    return result;
}

void EpollConnectionHandler::SlotSend(EndpointCtx* slotCtx, const void* data, std::uint32_t size, AsyncData asyncData)
{
    auto& rwBuf = slotCtx->rwBuffer;

    auto fireFailure = [&](SlotStatus status) {
        const AsyncResult fail{nullptr, 0, {.slotStatus = status}, AsyncStatus::IO_FAILURE};
        if(asyncData.asyncComplete)
            asyncData.asyncComplete(asyncData.userData, fail);
    };

    if(!rwBuf.IsWriteInitialized() && !rwBuf.InitWriteBuffer(config_.networkConfig.maxSendBufferSize)) {
        fireFailure(SlotStatus::BUFFER_ERROR);
        return;
    }

    if(!rwBuf.AppendWriteData(static_cast<const char*>(data), size, config_.networkConfig.sendBufferIncSize,
                              config_.networkConfig.maxSendBufferSize)) {
        fireFailure(SlotStatus::BUFFER_ERROR);
        return;
    }

    // 'asyncData' holds the SlotSend completion. 'HandleEndpointWriteComplete' fires it
    slotCtx->asyncData = asyncData;
    EnterState(slotCtx, EventType::EVENT_ENDPOINT_SEND);

    // Fail the operation immediately so that the user's onConnect coroutine
    // gets a definite answer rather than a slow hang.
    if(!RegisterEpoll(slotCtx, EPOLL_CTL_MOD)) {
        logger_.Error("[Epoll]: 'SlotSend -> RegisterEpoll(MOD)' failed: ", strerror(errno));
        fireFailure(SlotStatus::EPOLL_ERROR);
    }
}

void EpollConnectionHandler::SlotReceive(EndpointCtx* slotCtx, std::uint32_t consumed, AsyncData asyncData)
{
    auto fireFailure = [&](SlotStatus status) {
        const AsyncResult fail{nullptr, 0, {.slotStatus = status}, AsyncStatus::IO_FAILURE};
        if(asyncData.asyncComplete)
            asyncData.asyncComplete(asyncData.userData, fail);
    };

    // Trims bytes already used out of the PREVIOUS Receive() result, so a multi-round-trip
    // handshake doesn't get the same response redelivered. Kept local rather than calling
    // ConsumeParsedBytes: that helper Close()s the slot on a bad value, here fireFailure does.
    if(consumed > 0) {
        auto& rwBuf = slotCtx->rwBuffer;
        auto* readMeta = rwBuf.GetReadMeta();

        if(consumed > readMeta->dataLength) {
            logger_.Error("[Epoll]: 'SlotReceive' consumed=", consumed, " exceeds dataLength=", readMeta->dataLength);
            fireFailure(SlotStatus::IO_ERROR);
            return;
        }

        const std::uint32_t remaining = readMeta->dataLength - consumed;
        if(remaining > 0)
            std::memmove(rwBuf.GetReadData(), rwBuf.GetReadData() + consumed, remaining);

        readMeta->dataLength = remaining;
    }

    // EnsureReadReady closes the slot on allocation failure, but the caller's coroutine is
    // suspended waiting on this asyncData: without firing it here that coroutine never resumes.
    if(!EnsureReadReady(slotCtx)) {
        fireFailure(SlotStatus::BUFFER_ERROR);
        return;
    }

    // 'asyncData' holds the SlotReceive completion. 'HandleEpollIn' fires it when data arrives
    slotCtx->asyncData = asyncData;
    EnterState(slotCtx, EventType::EVENT_ENDPOINT_ONCONNECT);

    // Re-arm epoll so EPOLLIN fires even if backend data arrived while we were in the
    // SlotSend write phase. During that phase eventType was EVENT_ENDPOINT_SEND, so any
    // EPOLLIN edge that fired was consumed and ignored by HandleEpollIn's default case.
    // Without this MOD call, EPOLLIN would never re-fire in ET mode for that data
    if(!RegisterEpoll(slotCtx, EPOLL_CTL_MOD)) {
        logger_.Error("[Epoll]: 'SlotReceive -> RegisterEpoll(MOD)' failed: ", strerror(errno));
        fireFailure(SlotStatus::EPOLL_ERROR);
    }
}

void EpollConnectionHandler::SlotUpgradeTls(EndpointCtx* slotCtx, AsyncData asyncData)
{
    WFX_TRACE();

    auto& meta = endpoints_[slotCtx->endpointIdx].meta;

    auto fireFailure = [&](SlotStatus status) {
        const AsyncResult fail{nullptr, 0, {.slotStatus = status}, AsyncStatus::IO_FAILURE};
        if(asyncData.asyncComplete)
            asyncData.asyncComplete(asyncData.userData, fail);
    };

    // Nothing before this point needed TLS, so the outbound context may not be up yet
    if(!EnsureClientSSL()) {
        logger_.Error("[Epoll]: 'SlotUpgradeTls' could not initialize client SSL for endpoint '", meta.hostname, "'");
        fireFailure(SlotStatus::TLS_ERROR);
        return;
    }

    // Wrapping twice would leak the first SSL object and desync the connection. Endpoints that
    // want in-band upgrades must be configured plaintext so the engine never auto-wraps them.
    if(slotCtx->sslConn) {
        logger_.Error("[Epoll]: 'SlotUpgradeTls' called on an already-secure slot for endpoint '", meta.hostname, "'");
        fireFailure(SlotStatus::INVALID_STATE);
        return;
    }

    // Discards whatever's left in rwBuffer from the previous SlotReceive() call (e.g. the "220"
    // line's unconsumed tail), so it can't be replayed as though it arrived after the upgrade.
    slotCtx->rwBuffer.ClearReadBuffer();

    slotCtx->sslConn =
        sslHandler_->WrapClient(slotCtx->socket, meta.hostname.c_str(),
                                std::string_view{meta.config.alpnProtocols.data, meta.config.alpnProtocols.length},
                                &meta.cachedTlsSession);

    if(!slotCtx->sslConn) {
        logger_.Error("[Epoll]: 'SlotUpgradeTls -> WrapClient' failed for endpoint '", meta.hostname, "'");
        fireFailure(SlotStatus::TLS_ERROR);
        return;
    }

    slotCtx->SetEndpointState(EndpointState::ENDPOINT_SECURE);

    // Park on the handshake state and let HandleEndpointHandshake drive it to completion, rather
    // than stepping it here: that keeps one handshake implementation, and avoids resuming the
    // caller's coroutine synchronously from inside its own await_suspend. inOnConnectPhase is
    // already set (we are inside onConnect), which is what tells that handler to resume this
    // asyncData instead of firing onConnect again.
    slotCtx->asyncData = asyncData;
    EnterState(slotCtx, EventType::EVENT_ENDPOINT_HANDSHAKE);

    if(!RegisterEpoll(slotCtx, EPOLL_CTL_MOD)) {
        logger_.Error("[Epoll]: 'SlotUpgradeTls -> RegisterEpoll(MOD)' failed: ", strerror(errno));
        fireFailure(SlotStatus::EPOLL_ERROR);
    }
}

StringView EpollConnectionHandler::NegotiatedProtocol(EndpointCtx* slotCtx)
{
    if(!slotCtx->sslConn)
        return {};

    const std::string_view proto = sslHandler_->NegotiatedProtocol(slotCtx->sslConn);
    return StringView{proto.data(), proto.size()};
}

void EpollConnectionHandler::OpenSideConnection(EndpointCtx* ownerCtx, AsyncData asyncData)
{
    WFX_TRACE();

    auto& entry = endpoints_[ownerCtx->endpointIdx];

    EndpointCtx* auxCtx = GetAuxConnection(ownerCtx->endpointIdx);
    if(!auxCtx) {
        const AsyncResult fail{nullptr, 0, {.slotStatus = SlotStatus::BUFFER_ERROR}, AsyncStatus::IO_FAILURE};
        if(asyncData.asyncComplete)
            asyncData.asyncComplete(asyncData.userData, fail);

        return;
    }

    auxCtx->asyncData = asyncData;

    const EndpointStatus result = WrapConnect(auxCtx, entry);
    if(result != EndpointStatus::PENDING) {
        const SlotStatus mapped = result == EndpointStatus::SSL_FAILURE   ? SlotStatus::TLS_ERROR
                                  : result == EndpointStatus::EPOLL_ERROR ? SlotStatus::EPOLL_ERROR
                                                                          : SlotStatus::IO_ERROR;
        FailAuxConnect(auxCtx, mapped);
    }
}

void EpollConnectionHandler::CloseSideConnection(EndpointCtx* auxCtx)
{
    WFX_TRACE();

    if(!auxCtx)
        return;

    auto& entry = endpoints_[auxCtx->endpointIdx];
    const std::uint32_t idx = entry.auxPool.GetIndex(auxCtx);

    timerWheel_.Cancel(entry.meta.auxTimerBase + idx);
    (void)RegisterEpoll(auxCtx, EPOLL_CTL_DEL);

    if(auxCtx->sslConn) {
        sslHandler_->ForceShutdown(auxCtx->sslConn);
        auxCtx->sslConn = nullptr;
    }

    if(auxCtx->socket >= 0) {
        close(auxCtx->socket);
        auxCtx->socket = WFX_INVALID_SOCKET;
    }

    // Bump before Reset so a stale asyncData/generationId reference can't resolve to this
    // slot again once it's freed and reused for a different side connection.
    auxCtx->generationId++;
    if(auxCtx->generationId == 0)
        auxCtx->generationId = 1;

    auxCtx->Reset();
    entry.auxPool.FreeSlot(idx);
}

// vvv Main Functions vvv
void EpollConnectionHandler::Run()
{
    WFX_TRACE();

    // Just a simple sanity check before we do anything
    if(!onReceive_)
        logger_.Fatal("[Epoll]: 'onReceive_' was not initialized. Call 'SetEngineCallback' before calling 'Run'");

    // Handle endpoint pre-warming to improve performance
    HandlePrewarm();

    int sfd = 0;

    while(running_) {
        const int nfds = epoll_wait(epollFd_, events_.get(), maxEvents_, -1);
        if(nfds < 0) {
            // Interrupted by signal
            if(errno == EINTR)
                continue;
            break;
        }

        for(std::uint32_t i = 0; i < static_cast<std::uint32_t>(nfds); i++) {
            const std::uint32_t ev = events_[i].events;
            const std::uint64_t meta = events_[i].data.u64;
            const std::uint16_t gen = (meta >> 32) & 0xFFFF; // First half's lower 16 bits

            // Existing connection, handle it
            if(gen > 0)
                goto __HandleExistingConnection;

            sfd = static_cast<int>(meta & 0xFFFFFFFFULL);

            if(sfd == timeoutTimerFd_) {
                HandleTimeoutTimer(sfd);
                continue;
            }
            if(sfd == asyncTimerFd_) {
                HandleAsyncTimer(sfd);
                continue;
            }
            if(sfd == dnsResultEventFd_) {
                HandleDnsResultReady(sfd);
                continue;
            }

            // Accept new connections
            if(sfd == listenFd_) {
                while(true) {
                    sockaddr_storage addr{};
                    socklen_t len = sizeof(addr);

                    const int clientFd = accept4(listenFd_, (sockaddr*)&addr, &len, SOCK_NONBLOCK);
                    if(clientFd < 0) {
                        // Queue drained, stop
                        if(errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        continue;
                    }

                    // Extract IP info first
                    WFXIpAddress tmpIp;
                    if(!ResolveIP(addr, tmpIp)) {
                        close(clientFd);
                        continue;
                    }

                    // Try to grab a free connection slot for this client
                    ClientCtx* ctx = GetClientConnection();
                    if(!ctx) {
                        close(clientFd);
                        continue;
                    }

                    // Set connection info
                    ctx->socket = clientFd;
                    ctx->connInfo = tmpIp;

                    WrapAccept(ctx);
                }
                continue;
            }

        __HandleExistingConnection:
            // Get connection context
            // Also if you are confused with the below hardcoded numbers, check 'PackEpollData'
            // function, you can see how data is packed in event.u64 member field.
            const std::uint16_t endpointIdx = meta >> 48;
            const std::uint32_t poolIdx = meta & 0xFFFFFFFF;

            if(endpointIdx == CLIENT_CONNECTION_TAG) {
                ClientCtx* ctx = connections_.GetPtr(poolIdx);

                // If the slot's current generation doesn't match the event's generation, it means
                // this event is for a dead connection.
                if(ctx->generationId != gen)
                    continue;

                HandleClientEvent(ctx, ev, gen);
            }
            else {
                auto& entry = endpoints_[endpointIdx];

                const bool isAux = (poolIdx & AUX_CONNECTION_TAG_BIT) != 0;
                EndpointCtx* ctx =
                    isAux ? entry.auxPool.GetPtr(poolIdx & ~AUX_CONNECTION_TAG_BIT) : entry.pool.GetPtr(poolIdx);

                // If the slot's current generation doesn't match the event's generation, it means
                // this event is for a dead connection.
                if(ctx->generationId != gen)
                    continue;

                HandleEndpointEvent(ctx, ev, gen);
            }
        }

        // DNS refresh check, cheap time comparison, runs once per epoll wakeup
        const std::uint64_t nowSeconds = NowMs() / 1000;
        for(std::uint16_t i = 0; i < static_cast<std::uint16_t>(endpoints_.size()); i++) {
            auto& m = endpoints_[i].meta;
            if(nowSeconds >= m.dnsNextRefreshSeconds)
                HandleDnsRefresh(i);
        }
    }
}

void EpollConnectionHandler::RefreshExpiry(ClientCtx* ctx, std::uint16_t timeoutSeconds)
{
    const std::uint32_t idx = connections_.GetIndex(ctx);
    timerWheel_.Schedule(idx, CLIENT_CONNECTION_TAG, timeoutSeconds);
}

void EpollConnectionHandler::RefreshExpiry(EndpointCtx* ctx, std::uint16_t timeoutSeconds)
{
    // 'timerBase' offsets endpoint indices past all client slots and all preceding endpoint pools
    // so client slot N and endpoint slot N never collide in the timer wheel's meta_ array. auxPool
    // gets its own range past pool's ('auxTimerBase'), side connections never mix with it.
    auto& entry = endpoints_[ctx->endpointIdx];
    const std::uint32_t idx = ctx->isSideConnection ? entry.meta.auxTimerBase + entry.auxPool.GetIndex(ctx)
                                                    : entry.meta.timerBase + entry.pool.GetIndex(ctx);

    timerWheel_.Schedule(idx, ctx->endpointIdx, timeoutSeconds);
}

bool EpollConnectionHandler::RefreshAsyncTimer(ClientCtx* ctx, std::uint32_t delayMs, AsyncData asyncData)
{
    const std::uint32_t idx = connections_.GetIndex(ctx);
    const std::uint64_t expire = NowMs() + delayMs;

    // Timers are coalesced if they fall within +-10ms of each other
    if(!timerHeap_.Insert(idx, expire, 10)) {
        logger_.Warn("[Epoll]: Failed to refresh async timer");
        return false;
    }

    ctx->isAsyncTimerOperation = 1;
    ctx->asyncData = asyncData;

    UpdateAsyncTimer();

    return true;
}

void EpollConnectionHandler::Stop()
{
    running_ = false;
}

// vvv Slot state vvv
void EpollConnectionHandler::EnterState(ConnectionTag* ctx, EventType next)
{
    // Bit i of LEGAL_FROM[state] set means state i can legally precede it, keyed by enum value
    // (not list position) so a reorder of EventType can't desync this. Traced against every
    // 'eventType' write in this file. EVENT_ACCEPT is never a legal target, *_SHUTDOWN only
    // leads back to itself, both matching how the rest of the file already treats them.
    static constexpr auto LEGAL_FROM = [] {
        using ET = EventType;
        auto bit = [](ET e) constexpr { return static_cast<std::uint16_t>(1u << static_cast<unsigned>(e)); };

        std::array<std::uint16_t, 12> t{};
        t[static_cast<std::size_t>(ET::EVENT_HANDSHAKE)] = bit(ET::EVENT_ACCEPT) | bit(ET::EVENT_HANDSHAKE);
        t[static_cast<std::size_t>(ET::EVENT_RECV)] = bit(ET::EVENT_ACCEPT) | bit(ET::EVENT_HANDSHAKE) |
                                                      bit(ET::EVENT_RECV) | bit(ET::EVENT_SEND) |
                                                      bit(ET::EVENT_SEND_FILE);
        t[static_cast<std::size_t>(ET::EVENT_SEND)] =
            bit(ET::EVENT_RECV) | bit(ET::EVENT_SEND) | bit(ET::EVENT_SEND_FILE);
        t[static_cast<std::size_t>(ET::EVENT_SEND_FILE)] = t[static_cast<std::size_t>(ET::EVENT_SEND)];
        t[static_cast<std::size_t>(ET::EVENT_SHUTDOWN)] = bit(ET::EVENT_ACCEPT) | bit(ET::EVENT_HANDSHAKE) |
                                                          bit(ET::EVENT_RECV) | bit(ET::EVENT_SEND) |
                                                          bit(ET::EVENT_SEND_FILE) | bit(ET::EVENT_SHUTDOWN);

        t[static_cast<std::size_t>(ET::EVENT_CONNECT)] = bit(ET::EVENT_ACCEPT) | bit(ET::EVENT_CONNECT) |
                                                         bit(ET::EVENT_ENDPOINT_HANDSHAKE) |
                                                         bit(ET::EVENT_ENDPOINT_ONCONNECT);
        t[static_cast<std::size_t>(ET::EVENT_ENDPOINT_HANDSHAKE)] = t[static_cast<std::size_t>(ET::EVENT_CONNECT)];
        t[static_cast<std::size_t>(ET::EVENT_ENDPOINT_ONCONNECT)] = t[static_cast<std::size_t>(ET::EVENT_CONNECT)] |
                                                                    bit(ET::EVENT_ENDPOINT_ONCONNECT) |
                                                                    bit(ET::EVENT_ENDPOINT_SEND);
        t[static_cast<std::size_t>(ET::EVENT_ENDPOINT_SEND)] =
            t[static_cast<std::size_t>(ET::EVENT_ENDPOINT_ONCONNECT)] | bit(ET::EVENT_ENDPOINT_RECV);
        t[static_cast<std::size_t>(ET::EVENT_ENDPOINT_RECV)] =
            bit(ET::EVENT_CONNECT) | bit(ET::EVENT_ENDPOINT_ONCONNECT) | bit(ET::EVENT_ENDPOINT_SEND) |
            bit(ET::EVENT_ENDPOINT_RECV);
        t[static_cast<std::size_t>(ET::EVENT_ENDPOINT_SHUTDOWN)] =
            t[static_cast<std::size_t>(ET::EVENT_ENDPOINT_SEND)] | bit(ET::EVENT_ENDPOINT_SHUTDOWN);

        return t;
    }();

    const auto from = static_cast<std::size_t>(ctx->eventType);
    const auto to = static_cast<std::size_t>(next);

    if(!((LEGAL_FROM[to] >> from) & 1u))
        logger_.Fatal("[Epoll]: illegal EventType transition ", static_cast<int>(from), " -> ", static_cast<int>(to));

    ctx->eventType = next;
}

// vvv Helper Functions vvv
//  --- Connection Handlers ---
ClientCtx* EpollConnectionHandler::GetClientConnection()
{
    WFX_TRACE();

    ClientCtx* ctx = connections_.AllocSlot();
    if(!ctx)
        return nullptr;

    ctx->generationId++;
    metrics_->network.activeClientConns++;

    // If it wraps to 0, bump it to 1 cuz 0 is reserved for identifying fds such as Listen/Timer
    if(ctx->generationId == 0)
        ctx->generationId = 1;

    return ctx;
}

EndpointCtx* EpollConnectionHandler::GetEndpointConnection(std::uint16_t endpointIdx)
{
    WFX_TRACE();

    EndpointCtx* ctx = endpoints_[endpointIdx].pool.AllocSlot();
    if(!ctx)
        return nullptr;

    ctx->generationId++;
    ctx->isPooledIdle = 0;
    metrics_->network.activeEndpointConns++;
    endpointMetrics_[endpointIdx].slotsInUse++;

    // Anything still buffered belongs to the slot's previous life, not the response we are about
    // to ask for: an onPush message left incomplete is the way this happens.
    // Carrying it over would prepend those bytes to the next response and corrupt its parse.
    if(ctx->rwBuffer.IsReadInitialized())
        ctx->rwBuffer.ClearReadBuffer();

    // If it wraps to 0, bump it to 1 cuz 0 is reserved for identifying fds such as Listen/Timer
    if(ctx->generationId == 0)
        ctx->generationId = 1;

    return ctx;
}

EndpointCtx* EpollConnectionHandler::GetAuxConnection(std::uint16_t endpointIdx)
{
    WFX_TRACE();

    // No metrics/isPooledIdle bookkeeping: those track real pool capacity, auxPool is throwaway
    // and never goes through ReturnEndpointToPool/ReleaseEndpoint.
    EndpointCtx* ctx = endpoints_[endpointIdx].auxPool.AllocSlot();
    if(!ctx)
        return nullptr;

    ctx->generationId++;
    if(ctx->generationId == 0)
        ctx->generationId = 1;

    return ctx;
}

EndpointCtx* EpollConnectionHandler::FindMultiplexableSlot(std::uint16_t endpointIdx, EndpointMetadata& meta)
{
    WFX_TRACE();

    auto& pool = endpoints_[endpointIdx].pool;
    const std::uint32_t slots = pool.GetSlots();
    if(slots == 0)
        return nullptr;

    auto tryIdx = [&](std::uint32_t idx) -> EndpointCtx* {
        if(!pool.IsAllocated(idx))
            return nullptr;

        EndpointCtx* ctx = pool.GetPtr(idx);

        // Only a slot fully connected and cycling between requests (not still connecting,
        // handshaking, in onConnect, awaiting reconnect backoff, or already being torn down)
        // can safely take another request. isShuttingDown / connection-state checks are
        // required here specifically because this function (unlike AllocSlot) selects among
        // already-leased slots by inspecting live state rather than a free/leased bitmap bit. A slot
        // mid-teardown in Close()/ReleaseEndpoint() still reads eventType RECV/SEND and
        // is still bitmap-allocated for that entire window, so without this check a reentrant
        // SendPayload (e.g. from a coroutine resumed by this very teardown's waiter callbacks)
        // could attach a brand new client to a slot that's about to be Reset() and recycled for
        // an unrelated connection.
        const bool ready =
            !ctx->isShuttingDown && ctx->GetConnectionState() == ConnectionState::CONNECTION_ALIVE &&
            (ctx->eventType == EventType::EVENT_ENDPOINT_RECV || ctx->eventType == EventType::EVENT_ENDPOINT_SEND);

        return (ready && meta.desc.hasCapacity(ctx->slotState)) ? ctx : nullptr;
    };

    // Fast path: the slot we last multiplexed onto very likely still has room, avoids
    // a full scan on the common steady-state case.
    if(meta.lastMultiplexHintIdx < slots) {
        if(EndpointCtx* hit = tryIdx(meta.lastMultiplexHintIdx))
            return hit;
    }

    // Full scan ahead!
    for(std::uint32_t idx = 0; idx < slots; ++idx) {
        if(EndpointCtx* hit = tryIdx(idx)) {
            meta.lastMultiplexHintIdx = idx;
            return hit;
        }
    }

    return nullptr;
}

void EpollConnectionHandler::ReleaseClient(ClientCtx* ctx)
{
    WFX_TRACE();

    if(!ctx)
        return;

    metrics_->network.activeClientConns--;

    const std::uint32_t idx = connections_.GetIndex(ctx);

    // Cancelling timer in 'Close' kinda sucks cuz during async shutdown
    // the client might bail, never finish it, and we just be stuck in
    // closing state forever aaand timeout won't do anything cuz... we cancelled it.
    // So we close it here instead.
    timerWheel_.Cancel(idx);

    if(ctx->isAsyncTimerOperation) {
        if(timerHeap_.Remove(idx))
            UpdateAsyncTimer();
    }

    // Destroy orphaned coroutine frame if connection is dying
    // while an async operation is in-flight.
    HandleClientAsyncCallback(ctx, {}, true);

    // From client's POV, if the endpoint hasn't been set to nullptr after
    // endpoint operations complete, it means client closed before endpoint even
    // completed.
    if(ctx->endpointCtx) {
        EndpointCtx* epCtx = ctx->endpointCtx;

        // Multiplexed request: drop only this stream, the shared slot and every
        // other in-flight stream on it live on untouched.
        if(ctx->streamKey != 0 && epCtx->pendingStreams) {
            auto& meta = endpoints_[epCtx->endpointIdx].meta;
            auto& desc = meta.desc;

            auto it = epCtx->pendingStreams->find(ctx->streamKey);
            if(it != epCtx->pendingStreams->end()) {
                // Copy the entry out and erase it BEFORE any callback below. desc.takeStreamOutput
                // and the coalesce-waiter callbacks can synchronously reenter SendPayloadMultiplexed
                // (e.g. an app-level retry), which may insert into this same pendingStreams map and
                // trigger a rehash (using `it` afterward would then be undefined behavior).
                const PendingStream stream = it->second;
                epCtx->pendingStreams->erase(it);

                // Same coalesce-waiter failure this key's master death already gets on the
                // non-multiplexed path below (Close -> ReleaseEndpoint), just scoped to this
                // one stream instead of the whole slot.
                if(stream.coalesceKey != 0) {
                    auto cit = meta.coalescePending.find(stream.coalesceKey);
                    if(cit != meta.coalescePending.end()) {
                        std::vector<CoalesceWaiter> waiters = std::move(cit->second.waiters);
                        meta.coalescePending.erase(cit);
                        FailCoalesceWaiters(waiters, EndpointStatus::INTERNAL_ERROR);
                    }
                }

                if(stream.parseState && desc.destroyParseState)
                    desc.destroyParseState(stream.parseState);

                // Abandon the stream in the protocol too so it stops tracking a key nothing
                // will ever ask for again. May return an already-finished-but-not-yet-delivered
                // output (a benign race), which we simply free here.
                void* abandoned = desc.takeStreamOutput(epCtx->slotState, ctx->streamKey);
                if(abandoned && desc.destroyOutput)
                    desc.destroyOutput(abandoned);
            }
        }
        // Non-multiplexed: let onAbort try a graceful cancel if the protocol defines one, the
        // request isn't already streaming (DeliverStreamChunk isn't taught to tolerate an
        // orphaned-by-abort stream yet, a separate change), and onConnect isn't still running
        // on this same ctx. inOnConnectPhase matters: onConnect's coroutine may be suspended
        // mid-Send/Receive/UpgradeToTLS, owning slotCtx->asyncData for its own resume. FireOnAbort
        // unconditionally overwrites that same field, so firing it here would strand the
        // onConnect coroutine forever (its completion never arrives) while a second coroutine
        // starts on the same slot. Nothing meaningful is lost falling back to force-close: no
        // request has reached the backend yet at this point anyway.
        else {
            epCtx->clientCtx = nullptr;

            auto& epEntry = endpoints_[epCtx->endpointIdx];
            if(epEntry.meta.desc.onAbort && !epCtx->isStreaming && !epCtx->inOnConnectPhase) {
                epCtx->isAborted = 1;
                FireOnAbort(epCtx, epEntry);
            }
            else
                Close(epCtx, true);
        }

        ctx->endpointCtx = nullptr;
    }

    if(onClose_)
        onClose_(ctx);

    if(ctx->socket >= 0)
        close(ctx->socket);

    // Bump before Reset so any saved generationId in a CoalesceWaiter no longer
    // matches this slot, preventing a spurious delivery to a freed-but-not-reused slot.
    ctx->generationId++;
    if(ctx->generationId == 0)
        ctx->generationId = 1;

    ctx->Reset();
    connections_.FreeSlot(idx);
}

void EpollConnectionHandler::FailCoalesceWaiters(std::vector<CoalesceWaiter>& waiters, EndpointStatus status)
{
    for(auto& w : waiters) {
        // Stale waiter, its client already disconnected, skip it
        if(!w.clientCtx || w.clientCtx->generationId != w.generationId)
            continue;

        // Unlink first, then wake the waiter with a failure result
        w.clientCtx->endpointCtx = nullptr;

        AsyncResult failResult{};
        failResult.data = nullptr;
        failResult.dataLen = 0;
        failResult.status = AsyncStatus::IO_FAILURE;
        failResult.endpointStatus = status;

        HandleClientAsyncCallback(w.clientCtx, failResult, false);
    }
}

void EpollConnectionHandler::NotifyCoalesceWaiters(std::vector<CoalesceWaiter>& waiters, void* slotState,
                                                   void* outputObj, const EndpointDesc& desc)
{
    for(auto& w : waiters) {
        // Stale waiter, its client already disconnected, skip it
        if(!w.clientCtx || w.clientCtx->generationId != w.generationId)
            continue;

        // Unlink first, then hand this waiter its own clone of the result
        w.clientCtx->endpointCtx = nullptr;

        void* cloned = outputObj ? desc.cloneOutput(slotState, outputObj) : nullptr;

        // Ownership transfers to the waiter's EndpointOutput<T> RAII wrapper
        if(cloned)
            HandleClientAsyncCallback(w.clientCtx, {cloned, 0, {.unused = 0}, AsyncStatus::COMPLETED}, false);
        else {
            // No output to clone (protocol bug) or cloneOutput itself failed, report it instead
            AsyncResult failResult{};
            failResult.data = nullptr;
            failResult.dataLen = 0;
            failResult.endpointStatus = EndpointStatus::INTERNAL_ERROR;
            failResult.status = AsyncStatus::IO_FAILURE;
            HandleClientAsyncCallback(w.clientCtx, failResult, false);
        }
    }
}

void EpollConnectionHandler::ReleaseEndpoint(EndpointCtx* ctx, DisconnectReason disconnectReason)
{
    WFX_TRACE();

    if(!ctx)
        return;

    auto& em = endpointMetrics_[ctx->endpointIdx];

    // clientCtx is null for an idle slot AND for an aborted-but-still-in-flight one.
    // isPooledIdle tells them apart; only the idle case should skip the count.
    if(disconnectReason == DisconnectReason::TIMEOUT && !ctx->isPooledIdle)
        em.requestTimeouts++;

    // If this slot was idle-pooled (returned via ReturnEndpointToPool and never
    // re-leased), activeEndpointConns was already decremented at that point. Decrementing
    // again here would undercount. Only decrement for slots that were actively leased.
    if(!ctx->isPooledIdle) {
        metrics_->network.activeEndpointConns--;
        em.slotsInUse--;
    }

    ctx->isPooledIdle = 0;

    auto& entry = endpoints_[ctx->endpointIdx];
    auto& meta = entry.meta;
    auto& desc = meta.desc;

    const std::uint32_t idx = entry.pool.GetIndex(ctx);

    // SAME LOGIC PRETTY MUCH
    const std::uint32_t timerIdx = meta.timerBase + idx;
    timerWheel_.Cancel(timerIdx);

    // 'onDisconnect' fires before any state is destroyed so the user still has valid pointers
    if(ctx->slotState && desc.onDisconnect)
        desc.onDisconnect(ctx->slotState, disconnectReason);

    // Per-request objects (parse state, output) are cleaned up first
    FinalizeEndpointRequest(ctx, meta, false);

    // Per-slot state is destroyed last
    if(ctx->slotState && desc.destroySlotState) {
        desc.destroySlotState(ctx->slotState);
        ctx->slotState = nullptr;
    }

    // Remove from coalesce map and fail any parked waiters (O(1) via stored key)
    // slotState is already destroyed above (failure delivery doesn't need it)
    // For the COMPLETE path, coalesceKey was cleared and entry erased already, this is a no-op
    if(ctx->coalesceKey != 0) {
        auto& pending = meta.coalescePending;

        auto it = pending.find(ctx->coalesceKey);
        if(it != pending.end()) {
            FailCoalesceWaiters(it->second.waiters, DisconnectReasonToStatus(disconnectReason));
            pending.erase(it);
        }
        // coalesceKey = 0 handled by Reset() at end of ReleaseEndpoint
    }

    // If slot died during onConnect phase, destroy the suspended coroutine frame
    // to avoid leaking it. clientCtx notification is handled separately below.
    if(ctx->inOnConnectPhase)
        HandleEndpointAsyncCallback(ctx, {}, true);

    // Notify suspended client if the slot died mid-request
    // If client context exists, it means that the endpoint operation hasn't finished
    // but it somehow closed, in this case, just notify the client (as client is suspended
    // due to co_await).
    if(ctx->clientCtx) {
        ClientCtx* client = ctx->clientCtx;
        ctx->clientCtx = nullptr;
        client->endpointCtx = nullptr;

        AsyncResult result{};
        result.data = nullptr;
        result.dataLen = 0;
        result.status = AsyncStatus::IO_FAILURE;
        result.endpointStatus = DisconnectReasonToStatus(disconnectReason);

        HandleClientAsyncCallback(client, result, false);
    }

    if(ctx->socket >= 0)
        close(ctx->socket);

    ctx->Reset();
    entry.pool.FreeSlot(idx);
}

void EpollConnectionHandler::ReturnEndpointToPool(EndpointCtx* slotCtx)
{
    // Returns a keep-alive slot to the free list without closing the connection
    // The socket stays open and the slot context is intentionally NOT reset
    // Only the bitmap bit is cleared so GetEndpointConnection can lease the slot again for the next request
    auto& entry = endpoints_[slotCtx->endpointIdx];
    const std::uint32_t idx = entry.pool.GetIndex(slotCtx);

    // CRITICAL: cancel any leftover timer schedule first (e.g. a stale request-timeout
    // from a previous lease cycle) before arming the fresh idle timeout below.
    const std::uint32_t timerIdx = entry.meta.timerBase + idx;
    timerWheel_.Cancel(timerIdx);

    // A pinned slot stays leased to its holder across requests, so skip handing it back: the
    // bitmap bit (and the matching activeEndpointConns count) stay held until ReleaseSlot. The
    // idle timeout below still applies, so a forgotten reservation can't pin a connection forever.
    if(!slotCtx->isReserved) {
        entry.pool.FreeSlot(idx);
        metrics_->network.activeEndpointConns--;
        endpointMetrics_[slotCtx->endpointIdx].slotsInUse--;

        // Slot is now idle-pooled, open socket, no in-flight request
        // isPooledIdle marks that activeEndpointConns was already decremented above
        // ReleaseEndpoint checks this to avoid double-decrementing if the idle
        // timer actually fires before this slot is re-leased.
        slotCtx->isPooledIdle = 1;
    }

    // Slot reached a healthy idle state (fresh connect, successful reconnect, or a completed
    // keep-alive request), so clear any accumulated backoff attempts.
    slotCtx->reconnectAttempts = 0;

    // Arm idle timeout so the connection doesn't sit open forever without traffic
    RefreshExpiry(slotCtx, entry.meta.config.idleTimeoutSeconds);
}

void EpollConnectionHandler::MultiplexReleaseLeaseIfIdle(EndpointCtx* slotCtx)
{
    // Only an idle slot (no streams left) is not in use, and only decrement once per idle window
    if(slotCtx->isPooledIdle)
        return;

    if(slotCtx->pendingStreams && !slotCtx->pendingStreams->empty())
        return;

    metrics_->network.activeEndpointConns--;
    endpointMetrics_[slotCtx->endpointIdx].slotsInUse--;
    slotCtx->isPooledIdle = 1;
}

void EpollConnectionHandler::MultiplexReacquireLease(EndpointCtx* slotCtx)
{
    // No-op for a freshly leased slot (GetEndpointConnection already counted it), reacquires an
    // idle-pooled slot that FindMultiplexableSlot handed back for a new stream.
    if(!slotCtx->isPooledIdle)
        return;

    metrics_->network.activeEndpointConns++;
    endpointMetrics_[slotCtx->endpointIdx].slotsInUse++;
    slotCtx->isPooledIdle = 0;
}

//  --- MISC Handlers ---
std::uint64_t EpollConnectionHandler::NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - startTime_).count();
}

std::uint64_t EpollConnectionHandler::NowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - startTime_).count();
}

void EpollConnectionHandler::RecordEndpointCompletion(std::uint16_t endpointIdx, const EndpointDesc& desc,
                                                      const void* outputObj, std::uint64_t startUs)
{
    EndpointMetrics& em = endpointMetrics_[endpointIdx];
    em.completed++;

    // Sampled here, at the one place completed is bumped, so avg = sumUs / completed stays exact
    RecordEndpointLatency(endpointIdx, startUs);

    if(!desc.statusCode || !outputObj)
        return;

    const std::uint16_t code = desc.statusCode(outputObj);
    em.status1xx += (code >= 100 && code < 200);
    em.status2xx += (code >= 200 && code < 300);
    em.status3xx += (code >= 300 && code < 400);
    em.status4xx += (code >= 400 && code < 500);
    em.status5xx += (code >= 500 && code < 600);
}

void EpollConnectionHandler::RecordEndpointLatency(std::uint16_t endpointIdx, std::uint64_t startUs)
{
    // startUs stays 0 when latency is off (the stamp is gated), so this also skips the clock read
    if(startUs == 0)
        return;

    MetricTracer::RecordEndpointLatencyUs(endpointIdx, NowUs() - startUs);
}

void EpollConnectionHandler::RecordEndpointSendFailure(std::uint16_t endpointIdx, EndpointStatus status)
{
    // A synchronous failure skips the async HandleConnectFailure funnel, so classify it from the
    // status here instead of a DisconnectReason.
    EndpointMetrics& em = endpointMetrics_[endpointIdx];
    switch(status) {
        case EndpointStatus::CONNECT_FAILURE:
        case EndpointStatus::SOCKET_FAILURE:
            em.connectFailures++;
            break;
        case EndpointStatus::SSL_FAILURE:
            em.tlsFailures++;
            break;
        default:
            em.otherErrors++;
            break;
    }
}

bool EpollConnectionHandler::SetNonBlocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if(flags < 0)
        return false;

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool EpollConnectionHandler::EndpointUsesTls(const EndpointConfig& config, std::uint16_t port)
{
    switch(config.tlsConfig) {
        case EndpointTLSConfig::FORCE_REQUIRE:
            return true;
        case EndpointTLSConfig::FORCE_INSECURE:
            return false;
    }

    // Sorted TLS port array
    static constexpr std::uint16_t TLS_PORTS[] = {
        443,  // HTTPS          RFC 2818
        465,  // SMTPS          RFC 8314
        563,  // NNTPS          RFC 4642
        636,  // LDAPS          RFC 4513
        989,  // FTPS data      RFC 4217
        990,  // FTPS implicit  RFC 4217
        992,  // Telnet/TLS     RFC 5572
        993,  // IMAPS          RFC 8314
        995,  // POP3S          RFC 8314
        5061, // SIPS           RFC 3261
        5223, // XMPP TLS       IANA registered
        5349, // STUNS/TURNS    RFC 5389 / RFC 5766
        5671, // AMQPS          OASIS AMQP 1.0
        5684, // CoAPS          RFC 7252
        6380, // Redis TLS      IANA registered
        6514, // Syslog/TLS     RFC 5425
        6697, // IRC TLS        RFC 7194
        8443, // HTTPS alt      IANA registered
        8883, // MQTTS          OASIS MQTT
        9093, // Kafka TLS      IANA registered
    };

    // AUTO / DEFAULT case, do a binary search on ports
    std::size_t lo = 0, hi = std::size(TLS_PORTS);
    while(lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if(TLS_PORTS[mid] == port)
            return true;
        if(TLS_PORTS[mid] < port)
            lo = mid + 1;
        else
            hi = mid;
    }

    return false;
}

bool EpollConnectionHandler::EnsureFileReady(ClientCtx* ctx, const std::string& path)
{
    auto [fd, size] = fileCache_.GetFileDesc(path);
    if(fd < 0)
        return false;

    ctx->fileInfo.fd = fd;
    ctx->fileInfo.offset = 0;
    ctx->fileInfo.fileSize = size;

    return true;
}

bool EpollConnectionHandler::ResolveHost(const char* host, const char* port, sockaddr_storage* outAddr,
                                         socklen_t* outLen)
{
    addrinfo hints = {0};
    addrinfo* res = nullptr;

    hints.ai_family = AF_UNSPEC;     // Allow both IPv4 and IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP
    hints.ai_flags = AI_ADDRCONFIG;  // Only return addresses compatible with local interfaces

    if(getaddrinfo(host, port, &hints, &res) != 0)
        return false;

    bool found = false;
    if(res && res->ai_addrlen <= sizeof(sockaddr_storage)) {
        memcpy(outAddr, res->ai_addr, res->ai_addrlen);
        if(outLen)
            *outLen = res->ai_addrlen;

        found = true;
    }

    freeaddrinfo(res);
    return found;
}

bool EpollConnectionHandler::ResolveIP(const sockaddr_storage& addr, WFXIpAddress& out)
{
    const sockaddr* sa = reinterpret_cast<const sockaddr*>(&addr);

    switch(sa->sa_family) {
        case AF_INET: {
            out.ip.v4 = reinterpret_cast<const sockaddr_in*>(sa)->sin_addr;
            out.type = AF_INET;
            return true;
        }
        case AF_INET6: {
            const in6_addr& v6 = reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr;

            // A dual-stack listener (IPV6_V6ONLY disabled) hands back an ordinary IPv4 peer as
            // ::ffff:a.b.c.d, tagged AF_INET6: collapse it to AF_INET here, or NormalizeIp's
            // /64 IPv6 mask (coarser than the mapped prefix's fixed 96 bits) folds every such
            // peer into one shared ConnectionLimiter/RequestRateLimiter identity.
            if(IN6_IS_ADDR_V4MAPPED(&v6)) {
                std::memcpy(&out.ip.v4, &v6.s6_addr[12], sizeof(out.ip.v4));
                out.type = AF_INET;
            }
            else {
                out.ip.v6 = v6;
                out.type = AF_INET6;
            }

            return true;
        }
        default:
            return false;
    }
}

void EpollConnectionHandler::SendFile(ClientCtx* ctx)
{
    WFX_TRACE();

    // This is called in this order: WriteFile() -> Write() [Headers sent] -> SendFile()
    // This expects fileInfo to be set beforehand
    // If not, its UB. GG
    if(ctx->fileInfo.fd < 0 || ctx->fileInfo.fileSize <= 0) {
        logger_.Warn("[Epoll]: 'SendFile' expects 'ctx->fileInfo' to be set, got invalid data");
        Close(ctx);
        return;
    }

    auto& fileInfo = ctx->fileInfo;
    const int fd = fileInfo.fd;

    while(fileInfo.offset < fileInfo.fileSize) {
        const ssize_t n = WrapFile(ctx, fd, &fileInfo.offset, fileInfo.fileSize - fileInfo.offset);

        // Try to send more of file
        if(n > 0)
            continue;

        if(n < 0) {
            if(n == SWITCH_FILE_TO_STREAM) {
                ResumeStream(ctx);
                return;
            }

            // Partial progress, wait for event loop to notify us when it wants more data
            if(errno == EAGAIN || errno == EWOULDBLOCK)
                EnterState(ctx, EventType::EVENT_SEND_FILE);

            // Fatal error, close connection
            else
                Close(ctx);

            return;
        }

        // EOF / nothing sent
        break;
    }

    if(ctx->GetConnectionState() == ConnectionState::CONNECTION_CLOSE)
        Close(ctx);
    else {
        ctx->Clear();
        ResumeReceive(ctx);
    }
}

void EpollConnectionHandler::ResumeStream(ClientCtx* ctx)
{
    WFX_TRACE();

    // Paranoia check
    if(!ctx->streamGenerator.ctx || !ctx->streamGenerator.next) {
        logger_.Warn("[Epoll]: 'ResumeStream' function called but received empty generator");
        Close(ctx);
        return;
    }

    constexpr std::size_t HDR_RESERVE = 10; // max "%X\r\n" = 8 hex + \r\n
    auto& rwBuffer = ctx->rwBuffer;
    const bool chunked = static_cast<bool>(ctx->streamChunked);

    auto* writeMeta = rwBuffer.GetWriteMeta();
    if(!writeMeta) {
        Close(ctx);
        return;
    }

    // Loop: one iteration per chunk. Drives send() inline to skip the
    // epoll_ctl(MOD) + epoll_wait round trip when the socket stays writable.
    // On EAGAIN: yield via EVENT_SEND; EPOLLET re-fires EPOLLOUT when the kernel
    // drains the send buffer; Write() finishes the partial chunk then re-enters here.
    while(true) {
        writeMeta->dataLength = 0;
        writeMeta->writtenLength = 0;

        auto writeRegion = rwBuffer.GetWritableWriteRegion();
        if(!writeRegion.ptr || writeRegion.len == 0) {
            Close(ctx);
            return;
        }

        // Reserve header + trailer space upfront so chunkPtr points at payload start
        char* chunkPtr = writeRegion.ptr + (chunked ? HDR_RESERVE : 0);
        const std::size_t chunkCap = writeRegion.len - (chunked ? HDR_RESERVE + 2 : 0);

        auto result = ctx->streamGenerator.next(ctx->streamGenerator.ctx, {chunkPtr, chunkCap});
        RefreshExpiry(ctx, config_.networkConfig.idleTimeout);

        // A terminal action (STOP_*) may carry its last bytes in the same call, those still
        // need to be flushed below, not discarded. Only a non-terminal (CONTINUE) call with
        // zero bytes is a contract violation (no progress or completion signal either).
        const bool isTerminal = result.action != StreamAction::CONTINUE;
        if(!isTerminal && result.writtenBytes == 0) {
            Close(ctx);
            return;
        }

        // Sanity checks
        if(result.writtenBytes > UINT32_MAX) {
            Close(ctx);
            return;
        }

        if(isTerminal)
            ctx->SetConnectionState(result.action == StreamAction::STOP_AND_ALIVE_CONN
                                        ? ConnectionState::CONNECTION_ALIVE
                                        : ConnectionState::CONNECTION_CLOSE);

        if(result.writtenBytes > 0) {
            // Non-chunked: raw bytes go out as-is. Chunked: wrap them in "<size-hex>\r\n<data>\r\n"
            // (the size header is written right-aligned into HDR_RESERVE so no extra copy is needed).
            if(!chunked)
                writeMeta->dataLength = result.writtenBytes;
            else {
                char hdr[HDR_RESERVE + 1];
                const int hdrLen = snprintf(hdr, HDR_RESERVE, "%zX\r\n", result.writtenBytes);
                if(hdrLen <= 0 || hdrLen >= static_cast<int>(HDR_RESERVE)) {
                    Close(ctx);
                    return;
                }

                writeMeta->dataLength = HDR_RESERVE + result.writtenBytes + 2;

                std::memcpy(chunkPtr - hdrLen, hdr, hdrLen);

                rwBuffer.AdvanceWriteLength(HDR_RESERVE - hdrLen);
                chunkPtr[result.writtenBytes] = '\r';
                chunkPtr[result.writtenBytes + 1] = '\n';
            }

            // Write the final processed bytes to network
            const char* base = rwBuffer.GetWriteData();
            while(writeMeta->writtenLength < writeMeta->dataLength) {
                const ssize_t n = WrapWrite(ctx->socket, ctx->sslConn, base + writeMeta->writtenLength,
                                            writeMeta->dataLength - writeMeta->writtenLength);
                if(n > 0)
                    writeMeta->writtenLength += static_cast<std::uint32_t>(n);
                else if(errno == EAGAIN || errno == EWOULDBLOCK) {
                    EnterState(ctx, EventType::EVENT_SEND);
                    return;
                }
                else {
                    Close(ctx);
                    return;
                }
            }
        }

        if(isTerminal)
            break;

        // Chunk fully sent, next iteration
    }

    if(ctx->streamGenerator.destroy)
        ctx->streamGenerator.destroy(ctx->streamGenerator.ctx);

    const bool wasChunked = chunked; // ctx->streamChunked cleared below

    // Only STOP_AND_... states can reach here
    rwBuffer.ClearWriteBuffer();
    ctx->isStreamOperation = 0;
    ctx->streamChunked = 0;
    ctx->streamGenerator = {};

    // Write final chunk or finalize stream
    if(wasChunked)
        rwBuffer.AppendWriteData(CHUNK_END, sizeof(CHUNK_END) - 1, config_.networkConfig.sendBufferIncSize,
                                 config_.networkConfig.maxSendBufferSize)
            ? Write(ctx, {})
            : Close(ctx);
    else if(ctx->GetConnectionState() == ConnectionState::CONNECTION_ALIVE) {
        ctx->Clear();
        ResumeReceive(ctx);
    }
    else
        Close(ctx);
}

FlushStatus EpollConnectionHandler::DrainWriteBuffer(ClientCtx* ctx)
{
    WFX_TRACE();

    // Shared by Write's Case 2 and FlushChunk/ResumeFlushChunk. Callers decide what EVENT_SEND
    // should resume into (isAwaitFlush or not) before calling this, and what to do once it returns.
    auto& rwBuffer = ctx->rwBuffer;
    auto* writeMeta = rwBuffer.GetWriteMeta();
    const char* base = rwBuffer.GetWriteData();

    while(writeMeta->writtenLength < writeMeta->dataLength) {
        const ssize_t n = WrapWrite(ctx->socket, ctx->sslConn, base + writeMeta->writtenLength,
                                    writeMeta->dataLength - writeMeta->writtenLength);

        if(n > 0)
            writeMeta->writtenLength += static_cast<std::uint32_t>(n);
        else if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            EnterState(ctx, EventType::EVENT_SEND);
            return FlushStatus::PENDING;
        }
        else {
            Close(ctx);
            return FlushStatus::FAILED;
        }
    }

    return FlushStatus::COMPLETED;
}

bool EpollConnectionHandler::CompleteFlushRound(ClientCtx* ctx)
{
    WFX_TRACE();

    // Does not decide close vs keep-alive, HandleResponse does that later
    const bool isFinal = ctx->awaitFlushFinal;
    ctx->awaitFlushFinal = 0;

    if(!ctx->responseInfo->FinishFlushRound(isFinal)) {
        Close(ctx);
        return false;
    }

    return true;
}

void EpollConnectionHandler::ResumeFlushChunk(ClientCtx* ctx)
{
    WFX_TRACE();

    const FlushStatus status = DrainWriteBuffer(ctx);
    if(status == FlushStatus::PENDING)
        return;

    if(status == FlushStatus::FAILED)
        return; // Close(ctx) already ran inside DrainWriteBuffer

    ctx->isAwaitFlush = 0;

    if(!CompleteFlushRound(ctx))
        return; // CompleteFlushRound already closed ctx on failure

    HandleClientAsyncCallback(ctx, {nullptr, 0, {.unused = 0}, AsyncStatus::COMPLETED}, false);
}

void EpollConnectionHandler::AsyncCallbackImpl(void* ctxPtr, AsyncData& async, AsyncResult res, bool destroy)
{
    WFX_TRACE();

    auto& asyncRef = async;

    // Sanity checks, 'userData' in some edge cases maybe null, these shouldn't be
    if(!asyncRef.asyncComplete && !asyncRef.asyncDestroy)
        return;

    auto complete = asyncRef.asyncComplete;
    auto kill = asyncRef.asyncDestroy;
    auto ud = asyncRef.userData;

    asyncRef.asyncComplete = nullptr;
    asyncRef.asyncDestroy = nullptr;
    asyncRef.userData = nullptr;

    // Like all coroutines, this one would also require us to set type-erased 'ctx' at Http API
    Shared::GetHttpAPIExt1()->setGlobalPtrData(ctxPtr);

    if(destroy) {
        if(kill)
            kill(ud);
    }
    else {
        if(complete)
            complete(ud, res);
    }

    // And at the end, erase it
    Shared::GetHttpAPIExt1()->setGlobalPtrData(nullptr);
}

void EpollConnectionHandler::HandleClientAsyncCallback(ClientCtx* ctx, AsyncResult res, bool destroy)
{
    AsyncCallbackImpl(static_cast<void*>(ctx), ctx->asyncData, res, destroy);
}

void EpollConnectionHandler::HandleEndpointAsyncCallback(EndpointCtx* ctx, AsyncResult res, bool destroy)
{
    AsyncCallbackImpl(static_cast<void*>(ctx), ctx->asyncData, res, destroy);
}

void EpollConnectionHandler::HandleTimeoutTimer(int sfd)
{
    std::uint64_t expirations = 0;
    const ssize_t n = RetryOnEintr([&] { return read(sfd, &expirations, sizeof(expirations)); });

    if(n < 0) {
        // EAGAIN here would mean epoll reported readiness but the count was already
        // drained by the time we got here. Shouldn't normally happen since nothing
        // else reads this fd, but harmless if it does: skip this call, the level-triggered
        // registration means epoll will report it again if truly still ready.
        if(errno == EAGAIN || errno == EWOULDBLOCK)
            return;

        // Any other error means the read itself is broken. timeoutTimerFd_ is
        // registered level-triggered (no EPOLLET), so its readiness is cleared only
        // by a successful read here. If read() can no longer succeed at all, this
        // fd's state is now unknown/unrecoverable from inside this function. Worse,
        // if the fd somehow stays marked ready without being drained, epoll_wait
        // would return immediately on every iteration from here on, busy-spinning
        // the entire event loop at 100% CPU. Fail loudly rather than risk that.
        logger_.Fatal("[Epoll]: Timeout timer fd read failed: ", strerror(errno));
    }

    timerWheel_.Tick(NowMs() / 1000);

    // Fire any backoff reconnects that have come due
    HandleReconnects();
}

void EpollConnectionHandler::HandleAsyncTimer(int sfd)
{
    std::uint64_t expirations = 0;
    const ssize_t n = RetryOnEintr([&] { return read(sfd, &expirations, sizeof(expirations)); });

    if(n < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK)
            return;

        // Same reasoning as HandleTimeoutTimer: asyncTimerFd_ is level-triggered,
        // a broken read here risks either silently dead async timer delivery or
        // an undrained-readiness busy-spin on the event loop.
        logger_.Fatal("[Epoll]: Async timer fd read failed: ", strerror(errno));
    }

    const std::uint64_t newTick = NowMs();
    std::uint64_t connId = 0;

    while(timerHeap_.PopExpired(newTick, connId)) {
        ClientCtx* ctx = connections_.GetPtr(connId);
        ctx->isAsyncTimerOperation = 0;

        HandleClientAsyncCallback(ctx, {nullptr, 0, {.unused = 0}, AsyncStatus::COMPLETED}, false);
    }

    // Because the async timer is one shot, update it just in case there exists more
    // async-registered timers.
    UpdateAsyncTimer();
}

void EpollConnectionHandler::HandleClientHandshake(ClientCtx* ctx, std::uint32_t ev)
{
    WFX_TRACE();

    // SSL handshake for inbound client connections
    // On success: EVENT_RECV (client must send request first)
    if(!TryHandshake(ctx, EventType::EVENT_RECV, EventType::EVENT_HANDSHAKE)) {
        Close(ctx);
        return;
    }

    // Wait for handshake to finish
    if(ctx->eventType == EventType::EVENT_HANDSHAKE)
        return;

    // Handshake done, if data already arrived, process it immediately
    if(ev & EPOLLIN)
        HandleClientEpollIn(ctx);
}

void EpollConnectionHandler::HandleEndpointHandshake(EndpointCtx* ctx)
{
    WFX_TRACE();

    auto& entry = endpoints_[ctx->endpointIdx];
    auto& meta = entry.meta;

    // SSL handshake for outbound endpoint connections
    // On success: EVENT_ENDPOINT_ONCONNECT if onConnect hook exists (or this is a side connection,
    // which never runs desc.onConnect but still lands here for the caller to drive directly),
    // else EVENT_ENDPOINT_SEND
    const EventType onSuccess = (ctx->isSideConnection || meta.desc.onConnect) ? EventType::EVENT_ENDPOINT_ONCONNECT
                                                                               : EventType::EVENT_ENDPOINT_SEND;

    if(!TryHandshake(ctx, onSuccess, EventType::EVENT_ENDPOINT_HANDSHAKE)) {
        logger_.Error("[Epoll]: TLS handshake failed for endpoint '", meta.hostname, "'");

        if(ctx->isSideConnection)
            FailAuxConnect(ctx, SlotStatus::TLS_ERROR);
        else
            HandleConnectFailure(ctx, entry, false, DisconnectReason::ERROR, true);

        return;
    }

    // Wait for handshake to finish
    if(ctx->eventType == EventType::EVENT_ENDPOINT_HANDSHAKE)
        return;

    // Its alive, ITS ALIVE. ITS ALIVEEEUEEUUEEEE. IN THE NAME OF GOD, NOW I KNOW WHAT IT FEELS LIKE TO BE GOD
    // - Frankenstein
    ctx->SetConnectionState(ConnectionState::CONNECTION_ALIVE);

    // TLS handshake done, connectTimeoutSeconds phase is over. What happens next
    // depends on whether onConnect exists and whether a client is waiting:
    //   - onConnect hook exists -> FireOnConnect drives the handshake protocol,
    //     requestTimeoutSeconds covers onConnect + the eventual send/receive
    //   - no onConnect, but a client is waiting -> Write the already-serialized
    //     request, requestTimeoutSeconds covers the round trip
    //   - no onConnect, no client (prewarm) -> nothing to send, the refresh
    //     below is immediately superseded by ReturnEndpointToPool's idle timeout
    RefreshExpiry(ctx, entry.meta.config.requestTimeoutSeconds);

    // Already inside onConnect/side-connect means this handshake was an in-band upgrade
    // (SlotUpgradeTls), not the initial one: resume the suspended caller where it awaited
    // instead of starting onConnect/CompleteAuxConnect over from the top.
    if(ctx->inOnConnectPhase)
        HandleEndpointAsyncCallback(ctx, {nullptr, 0, {.slotStatus = SlotStatus::OK}, AsyncStatus::COMPLETED}, false);
    else if(ctx->isSideConnection)
        CompleteAuxConnect(ctx);
    else if(ctx->eventType == EventType::EVENT_ENDPOINT_ONCONNECT)
        FireOnConnect(ctx, entry);
    else if(ctx->clientCtx)
        Write(ctx);
    else {
        ReturnEndpointToPool(ctx);

        // Slot is idle-pooled at this point. If MOD fails the fd is left in an
        // inconsistent epoll registration state with no way to detect future
        // readability/writability. Force close it rather than leak a half-dead slot.
        if(!RegisterEpoll(ctx, EPOLL_CTL_MOD)) {
            logger_.Error("[Epoll]: 'RegisterEpoll(MOD)' failed for endpoint '", entry.meta.hostname,
                          "' after prewarm handshake: ", strerror(errno));
            Close(ctx, true);
        }
    }
}

void EpollConnectionHandler::HandleClientEvent(ClientCtx* ctx, std::uint32_t ev, std::uint16_t gen)
{
    WFX_TRACE();

    // SSL handshake dispatch
    if(ctx->eventType == EventType::EVENT_HANDSHAKE) {
        HandleClientHandshake(ctx, ev);
        return;
    }

    // SSL shutdown dispatch
    if(ctx->eventType == EventType::EVENT_SHUTDOWN) {
        auto res = sslHandler_->Shutdown(ctx->sslConn);
        switch(res) {
            // Shutdown still needs time, wait for more data
            case SSLReturn::WANT_READ:
            case SSLReturn::WANT_WRITE:
                break;

            // Success or Failure, manually shutdown the connection
            // Cannot call 'Close' cuz its not needed
            default:
                ctx->sslConn = nullptr;
                (void)RegisterEpoll(ctx, EPOLL_CTL_DEL);
                ReleaseClient(ctx);
                break;
        }
        return;
    }

    if(ev & (EPOLLERR | EPOLLHUP)) {
        Close(ctx);
        return;
    }

    if(ev & EPOLLIN)
        HandleClientEpollIn(ctx);

    // Re-check: HandleClientEpollIn may have closed this slot
    if((ev & EPOLLOUT) && ctx->generationId == gen)
        HandleClientWriteReady(ctx);
}

void EpollConnectionHandler::HandleEndpointEvent(EndpointCtx* ctx, std::uint32_t ev, std::uint16_t gen)
{
    WFX_TRACE();

    // SSL handshake dispatch
    if(ctx->eventType == EventType::EVENT_ENDPOINT_HANDSHAKE) {
        HandleEndpointHandshake(ctx);
        return;
    }

    // SSL shutdown dispatch
    if(ctx->eventType == EventType::EVENT_ENDPOINT_SHUTDOWN) {
        auto res = sslHandler_->Shutdown(ctx->sslConn);
        switch(res) {
            // Shutdown still needs time, wait for more data
            case SSLReturn::WANT_READ:
            case SSLReturn::WANT_WRITE:
                break;

            // Success or Failure, manually shutdown the connection
            // Cannot call 'Close' cuz its not needed
            default:
                ctx->sslConn = nullptr;
                (void)RegisterEpoll(ctx, EPOLL_CTL_DEL);
                ReleaseEndpoint(ctx);
                break;
        }
        return;
    }

    if(ev & (EPOLLERR | EPOLLHUP)) {
        // An EPOLLERR in the connect phase (an async ECONNREFUSED often arrives this way, not as a
        // writable socket with SO_ERROR) is a connect failure, so route it through the funnel to be
        // counted and reconnected instead of a bare Close. Handshake errors are handled above, so
        // only EVENT_CONNECT / onConnect reach here.
        const bool connectPhase =
            ctx->eventType == EventType::EVENT_CONNECT || ctx->eventType == EventType::EVENT_ENDPOINT_ONCONNECT;

        if(connectPhase)
            HandleConnectFailure(ctx, endpoints_[ctx->endpointIdx], false, DisconnectReason::ERROR);
        else
            Close(ctx);

        return;
    }

    if(ev & EPOLLIN)
        HandleEndpointEpollIn(ctx);

    // Re-check: HandleEndpointEpollIn may have closed this slot
    if((ev & EPOLLOUT) && ctx->generationId == gen)
        HandleEndpointWriteReady(ctx);
}

// Single EPOLLIN dispatch point for clients, routes by eventType
void EpollConnectionHandler::HandleClientEpollIn(ClientCtx* ctx)
{
    WFX_TRACE();

    switch(ctx->eventType) {
        case EventType::EVENT_RECV:
            // Read whatever is currently available on the socket
            if(Receive(ctx))
                onReceive_(ctx);

            return;

        default:
            // Any other state, connection is doing something else, ignore stray EPOLLIN
            return;
    }
}

// Single EPOLLIN dispatch point for endpoints, routes by eventType
void EpollConnectionHandler::HandleEndpointEpollIn(EndpointCtx* ctx)
{
    WFX_TRACE();

    switch(ctx->eventType) {
        case EventType::EVENT_ENDPOINT_RECV: {
            bool eof = false;
            const bool gotData = Receive(ctx, &eof);

            // Nothing to act on: EAGAIN with no new bytes, or Receive already closed on a fatal
            // error (ctx may be released, must not touch it).
            if(!eof && !gotData)
                return;

            // The parse callback only ever runs for a slot with a request in flight: clientCtx
            // for the single-slot path, pendingStreams for multiplexed (which never sets
            // clientCtx at all), or isAborted (client bailed but a real response is still owed).
            // Otherwise parse, passing eof through so a close-delimited body gets finalized on
            // the last call.
            //
            // With none of those, the slot is idle-pooled or prewarmed and nothing is awaiting
            // these bytes. That's server-initiated data (Postgres NOTIFY, Redis pub/sub), handed
            // to desc.onPush if the protocol wants it, else the slot is released as before.
            if(ctx->clientCtx || ctx->isAborted || (ctx->pendingStreams && !ctx->pendingStreams->empty()))
                HandleEndpointReceive(ctx, eof);
            else
                HandleEndpointPush(ctx, eof);

            return;
        }
        case EventType::EVENT_ENDPOINT_ONCONNECT: {
            // onConnect coroutine called SlotReceive and is now suspended waiting for data
            // Passing 'eof' matters: without it, Receive() bare-Closes on hangup and the failure
            // goes uncounted. Data-then-EOF still wakes the coroutine normally; EOF alone routes
            // through HandleConnectFailure so it's counted like any other connect-phase failure.
            bool eof = false;
            const bool gotData = Receive(ctx, &eof);

            if(gotData)
                HandleEndpointAsyncCallback(ctx,
                                            {ctx->rwBuffer.GetReadData(),
                                             ctx->rwBuffer.GetReadMeta()->dataLength,
                                             {.unused = 0},
                                             AsyncStatus::COMPLETED},
                                            false);
            else if(eof)
                HandleConnectFailure(ctx, endpoints_[ctx->endpointIdx], false, DisconnectReason::ERROR);

            return;
        }
        default:
            // Any other state, connection is doing something else, ignore stray EPOLLIN
            return;
    }
}

void EpollConnectionHandler::HandleClientWriteReady(ClientCtx* ctx)
{
    WFX_TRACE();

    switch(ctx->eventType) {
        // Client response write
        case EventType::EVENT_SEND:
            if(ctx->isAwaitFlush)
                ResumeFlushChunk(ctx);
            else
                Write(ctx, {});
            break;

        // Client file transfer
        case EventType::EVENT_SEND_FILE:
            SendFile(ctx);
            break;

        default:
            break;
    }
}

void EpollConnectionHandler::HandleEndpointWriteReady(EndpointCtx* ctx)
{
    WFX_TRACE();

    switch(ctx->eventType) {
        // Endpoint request write or SlotSend from inside onConnect
        case EventType::EVENT_ENDPOINT_SEND:
            Write(ctx);
            break;

        // Immediate connect completed (onConnect hook, or a side connection), hand it off now
        // Reached when connect() returned 0 synchronously and WrapConnect set
        // EVENT_ENDPOINT_ONCONNECT before RegisterEpoll(ADD + MOD); the MOD fires EPOLLOUT
        // immediately since the socket is already writable.
        //
        // RegisterEpoll always arms EPOLLIN|EPOLLOUT together, so once the coroutine/caller is
        // suspended on a Receive (same eventType, opposite meaning: "waiting for a reply" instead
        // of "just connected"), a still-writable socket can redeliver EPOLLOUT here.
        // inOnConnectPhase tells the two apart: only hand off while nothing is running yet
        // (reused as-is for side connections, which never touch desc.onConnect).
        case EventType::EVENT_ENDPOINT_ONCONNECT: {
            if(!ctx->inOnConnectPhase) {
                if(ctx->isSideConnection)
                    CompleteAuxConnect(ctx);
                else
                    FireOnConnect(ctx, endpoints_[ctx->endpointIdx]);
            }
        } break;

        // TCP connect completed, proceed to SSL handshake or directly to write/onConnect
        case EventType::EVENT_CONNECT: {
            int err = 0;
            socklen_t len = sizeof(err);

            auto& ep = endpoints_[ctx->endpointIdx];
            auto& meta = ep.meta;

            if(getsockopt(ctx->socket, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
                logger_.Error("[Epoll]: Connect failed for endpoint '", meta.hostname, "': ", strerror(err));

                if(ctx->isSideConnection)
                    FailAuxConnect(ctx, SlotStatus::IO_ERROR);
                else
                    HandleConnectFailure(ctx, ep, false);

                break;
            }

            ctx->SetConnectionState(ConnectionState::CONNECTION_ALIVE);

            // TLS required, try SSL handshake
            if(ctx->GetEndpointState() == EndpointState::ENDPOINT_SECURE) {
                ctx->sslConn = sslHandler_->WrapClient(ctx->socket, meta.hostname.c_str(),
                                                       std::string_view{meta.config.alpnProtocols.data,
                                                                        meta.config.alpnProtocols.length},
                                                       &meta.cachedTlsSession);

                if(!ctx->sslConn) {
                    if(ctx->isSideConnection)
                        FailAuxConnect(ctx, SlotStatus::TLS_ERROR);
                    else
                        HandleConnectFailure(ctx, ep, false, DisconnectReason::ERROR, true);
                    break;
                }

                HandleEndpointHandshake(ctx);
                break;
            }

            // Side connection, plain TCP: fully connected, hand it back to the caller directly
            // (never runs desc.onConnect, never touches Write/ReturnEndpointToPool below).
            if(ctx->isSideConnection) {
                CompleteAuxConnect(ctx);
                break;
            }

            // Plain TCP, no handshake needed. Connection phase is fully complete
            // Switch to request timeout now since onConnect / Write starts immediately
            RefreshExpiry(ctx, meta.config.requestTimeoutSeconds);

            // Plain TCP, check for onConnect hook before writing
            if(meta.desc.onConnect) {
                FireOnConnect(ctx, endpoints_[ctx->endpointIdx]);
                break;
            }

            // No onConnect hook. Only Write if a client request is actually
            // waiting in the buffer. A prewarm slot with no client has nothing
            // to send and should go straight to the idle pool instead.
            if(ctx->clientCtx)
                Write(ctx);
            else {
                EnterState(ctx, EventType::EVENT_ENDPOINT_RECV);
                ReturnEndpointToPool(ctx);
                if(!RegisterEpoll(ctx, EPOLL_CTL_MOD)) {
                    logger_.Error("[Epoll]: 'RegisterEpoll(MOD)' failed for endpoint '", meta.hostname,
                                  "' after prewarm connect: ", strerror(errno));
                    Close(ctx, true);
                }
            }
        } break;

        default:
            break;
    }
}

void EpollConnectionHandler::UpdateAsyncTimer()
{
    TimerNode* min = timerHeap_.GetMin();

    // Base check, nothing is pending so disarm the timer
    if(!min) {
        const itimerspec disarm{};
        timerfd_settime(asyncTimerFd_, 0, &disarm, nullptr);
        return;
    }

    const std::uint64_t now = NowMs();
    const std::uint64_t expire = min->delay;
    const std::uint64_t remain = (expire <= now) ? 1 : (expire - now);

    itimerspec ts{};
    ts.it_value.tv_sec = static_cast<decltype(ts.it_value.tv_sec)>(remain / 1000);
    ts.it_value.tv_nsec = static_cast<decltype(ts.it_value.tv_nsec)>((remain % 1000) * 1'000'000);
    ts.it_interval = {0, 0};

    if(RetryOnEintr([&] { return timerfd_settime(asyncTimerFd_, 0, &ts, nullptr); }) < 0)
        logger_.Error("[Epoll]: Failed to set async timer: ", strerror(errno));
}

void EpollConnectionHandler::ValidateEndpoint(const char* host, const EndpointDesc& desc, const EndpointConfig& config)
{
    // vvv EndpointDesc vvv
    if(!desc.serialize)
        logger_.Fatal("[Epoll]: EndpointDesc.serialize must not be null for endpoint: ", host);
    if(!desc.parse)
        logger_.Fatal("[Epoll]: EndpointDesc.parse must not be null for endpoint: ", host);

    // create/destroy pairs must both be present or both absent, otherwise leaks/double-frees
    if((desc.createParseState != nullptr) != (desc.destroyParseState != nullptr))
        logger_.Fatal("[Epoll]: EndpointDesc.createParseState and destroyParseState must both be set or both null "
                      "for endpoint: ",
                      host);
    if((desc.createOutput != nullptr) != (desc.destroyOutput != nullptr))
        logger_.Fatal("[Epoll]: EndpointDesc.createOutput and destroyOutput must both be set or both null "
                      "for endpoint: ",
                      host);
    if((desc.createSlotState != nullptr) != (desc.destroySlotState != nullptr))
        logger_.Fatal("[Epoll]: EndpointDesc.createSlotState and destroySlotState must both be set or both null "
                      "for endpoint: ",
                      host);
    if(desc.coalesceKey != nullptr && desc.cloneOutput == nullptr)
        logger_.Fatal("[Epoll]: EndpointDesc.cloneOutput must not be null when coalesceKey is set for endpoint: ",
                      host);
    if(desc.hasCapacity != nullptr && desc.takeStreamOutput == nullptr)
        logger_.Fatal("[Epoll]: EndpointDesc.takeStreamOutput must not be null when hasCapacity is set for endpoint: ",
                      host);

    // vvv EndpointConfig vvv
    if(config.connLimit == 0)
        logger_.Fatal("[Epoll]: EndpointConfig.connLimit must be > 0 for endpoint: ", host);

    // Timeout timer ticks at most once every INVOKE_TIMEOUT_COOLDOWN seconds, so any
    // timeout value below that fires no earlier than the next tick. The configured
    // value would silently lie about how soon it actually triggers.
    if(config.connectTimeoutSeconds < INVOKE_TIMEOUT_COOLDOWN)
        logger_.Fatal("[Epoll]: EndpointConfig.connectTimeoutSeconds must be >= ", INVOKE_TIMEOUT_COOLDOWN,
                      " (timer tick interval) for endpoint: ", host);
    if(config.requestTimeoutSeconds < INVOKE_TIMEOUT_COOLDOWN)
        logger_.Fatal("[Epoll]: EndpointConfig.requestTimeoutSeconds must be >= ", INVOKE_TIMEOUT_COOLDOWN,
                      " (timer tick interval) for endpoint: ", host);
    if(config.idleTimeoutSeconds < INVOKE_TIMEOUT_COOLDOWN)
        logger_.Fatal("[Epoll]: EndpointConfig.idleTimeoutSeconds must be >= ", INVOKE_TIMEOUT_COOLDOWN,
                      " (timer tick interval) for endpoint: ", host);

    if(config.maxReconnectAttempts > 0 && config.reconnectBackoffBase > config.reconnectBackoffMax)
        logger_.Fatal("[Epoll]: EndpointConfig.reconnectBackoffBase must be <= reconnectBackoffMax for endpoint: ",
                      host);
    if(config.prewarm > config.connLimit)
        logger_.Fatal("[Epoll]: EndpointConfig.prewarm (", config.prewarm, ") exceeds connLimit (", config.connLimit,
                      ") for endpoint: ", host);
}

std::uint64_t EpollConnectionHandler::ComputeNextDnsRefresh(std::uint32_t minTtlSeconds,
                                                            std::uint32_t userOverrideSeconds,
                                                            const std::string& hostname)
{
    // UINT32_MAX TTL means no real DNS involved (literal IP or loopback alias)
    // The address can never change, so refreshing is always a no-op regardless of
    // any user-configured interval. Schedule the furthest-out check the wheel allows.
    if(minTtlSeconds == UINT32_MAX)
        return (NowMs() / 1000) + MAX_REFRESH_SECONDS;

    std::uint32_t interval;

    // User explicitly set a refresh cadence (this is a CEILING, not an override
    // of TTL entirely). If the record's actual TTL is shorter, still honor it,
    // since refreshing later than the DNS-promised validity risks stale addresses.
    if(userOverrideSeconds > 0)
        interval = std::min(userOverrideSeconds, minTtlSeconds > 0 ? minTtlSeconds : userOverrideSeconds);

    // 0 = fully TTL-driven, refresh exactly when the DNS record says to
    else
        interval = minTtlSeconds > 0 ? minTtlSeconds : MAX_REFRESH_SECONDS;

    interval = std::clamp(interval, MIN_REFRESH_SECONDS, MAX_REFRESH_SECONDS);

    // Jitter range is 10% of interval, minimum 5 seconds regardless of interval size
    // Ensures meaningful spread even at MIN_REFRESH_SECONDS where 10% would be < 1s
    const std::uint32_t jitterRange = std::max(MIN_REFRESH_SECONDS, interval / 10);
    const std::uint32_t hash32 = static_cast<std::uint32_t>(std::hash<std::string>{}(hostname));
    const std::uint32_t jitter = static_cast<std::uint32_t>((static_cast<std::uint64_t>(hash32) * jitterRange) >> 32);

    return (NowMs() / 1000) + interval + jitter;
}

void EpollConnectionHandler::FinalizeEndpointRequest(EndpointCtx* ctx, EndpointMetadata& meta, bool success)
{
    auto& desc = meta.desc;

    // Cleans up per-request objects (parse state, output)
    // Called on every request completion, both success and failure paths
    // success=true: reset parse state for reuse rather than destroying it
    if(ctx->parseStateObj) {
        if(success && desc.resetParseState)
            desc.resetParseState(ctx->parseStateObj);
        else if(desc.destroyParseState) {
            desc.destroyParseState(ctx->parseStateObj);
            ctx->parseStateObj = nullptr;
        }
    }

    if(ctx->outputObj && desc.destroyOutput) {
        desc.destroyOutput(ctx->outputObj);
        ctx->outputObj = nullptr;
    }

    // Multiplexed slot: fail and tear down every stream still pending. Only reached from the
    // slot-teardown paths (parse error, connection drop); HandleEndpointReceive already erases
    // a stream's own map entry the moment that individual stream completes.
    //
    // Detach the map from ctx BEFORE touching any entry or firing any callback below. The
    // callbacks resume suspended client coroutines synchronously, and a completely ordinary
    // app-level retry pattern (co_await the same endpoint again on failure) can reenter
    // SendPayloadMultiplexed from inside one of those callbacks, before this function returns
    // and before the caller has necessarily marked ctx unavailable. With the map already
    // detached, SendPayloadMultiplexed sees ctx->pendingStreams == nullptr and lazily allocates
    // a fresh map instead of touching the live one this loop is iterating, otherwise a
    // reentrant insert into *ctx->pendingStreams while this range-for is executing is undefined
    // behavior (unordered_map insertion may rehash, invalidating the loop's iterator).
    if(ctx->pendingStreams) {
        const PendingStreamMap orphaned = std::move(*ctx->pendingStreams);
        Shared::Delete(ctx->pendingStreams);
        ctx->pendingStreams = nullptr;

        for(auto& [key, stream] : orphaned) {
            // Same per-stream coalesce cleanup ReleaseEndpoint does for the single-slot
            // coalesceKey below (fail every waiter, then erase), just scoped to this one
            // stream instead of the whole slot. Move the waiters out and erase the map entry
            // BEFORE firing any callback, same reasoning as detaching pendingStreams above:
            // a reentrant SendPayload with this exact coalesce key would otherwise push_back
            // into the very vector this loop is iterating (the entry isn't erased until after
            // the loop would have finished, so a reentrant call still finds and reuses it).
            if(stream.coalesceKey != 0) {
                auto it = meta.coalescePending.find(stream.coalesceKey);
                if(it != meta.coalescePending.end()) {
                    std::vector<CoalesceWaiter> waiters = std::move(it->second.waiters);
                    meta.coalescePending.erase(it);
                    FailCoalesceWaiters(waiters, EndpointStatus::INTERNAL_ERROR);
                }
            }

            if(stream.parseState && desc.destroyParseState)
                desc.destroyParseState(stream.parseState);

            // Abandon the stream in the protocol too. Whatever it hands back (finished or
            // not) is discarded: every path below is a failure delivery, never a success.
            void* abandoned = desc.takeStreamOutput(ctx->slotState, key);
            if(abandoned && desc.destroyOutput)
                desc.destroyOutput(abandoned);

            if(stream.clientCtx && stream.clientCtx->generationId == stream.generationId) {
                ClientCtx* client = stream.clientCtx;
                client->endpointCtx = nullptr;

                AsyncResult result{};
                result.data = nullptr;
                result.dataLen = 0;
                result.status = AsyncStatus::IO_FAILURE;
                result.endpointStatus = EndpointStatus::INTERNAL_ERROR;
                HandleClientAsyncCallback(client, result, false);
            }
        }
    }
}

void EpollConnectionHandler::HandleEndpointWriteComplete(EndpointCtx* slotCtx)
{
    WFX_TRACE();

    // If asyncData is set, this write was initiated by SlotSend from inside onConnect
    // Fire the SlotSend completion and stay in the onConnect phase
    if(slotCtx->asyncData.asyncComplete) {
        HandleEndpointAsyncCallback(slotCtx, {nullptr, 0, {.unused = 0}, AsyncStatus::COMPLETED}, false);
        return;
    }

    // Normal path, request sent, transition to response phase
    EnterState(slotCtx, EventType::EVENT_ENDPOINT_RECV);
}

void EpollConnectionHandler::Write(EndpointCtx* ctx)
{
    WFX_TRACE();

    // Private endpoint write. No stream / file logic since endpoints never use those paths
    auto* writeMeta = ctx->rwBuffer.GetWriteMeta();
    if(!writeMeta || writeMeta->writtenLength >= writeMeta->dataLength) {
        HandleEndpointWriteComplete(ctx);
        return;
    }

    while(writeMeta->writtenLength < writeMeta->dataLength) {
        const char* buf = ctx->rwBuffer.GetWriteData() + writeMeta->writtenLength;
        const std::size_t remaining = writeMeta->dataLength - writeMeta->writtenLength;

        const ssize_t n = WrapWrite(ctx->socket, ctx->sslConn, buf, remaining);

        if(n > 0)
            writeMeta->writtenLength += n;

        // Partial progress, wait for event loop to notify when we can send more data
        else if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            EnterState(ctx, EventType::EVENT_ENDPOINT_SEND);
            return;
        }

        // Connection closed / Fatal error
        else {
            Close(ctx);
            return;
        }
    }

    HandleEndpointWriteComplete(ctx);
}

void EpollConnectionHandler::ResolveMultiplexedStream(EndpointCtx* slotCtx, EndpointEntry& entry, std::uint64_t key)
{
    auto& desc = entry.meta.desc;

    auto it = slotCtx->pendingStreams->find(key);
    if(it == slotCtx->pendingStreams->end())
        return;

    const PendingStream stream = it->second;
    slotCtx->pendingStreams->erase(it);

    // The protocol owned this stream's output internally up to now; hand ownership to the
    // engine. Null here would mean parse() reported completion without ever finishing an
    // output object (a protocol bug), handled the same as the 'client vanished' case below.
    void* outputObj = desc.takeStreamOutput(slotCtx->slotState, key);

    // Fan out to any coalesced waiters parked under this stream's own key (per-stream, not
    // per-slot: several concurrently in-flight streams on this one slot may each coalesce
    // under a different key).
    std::vector<CoalesceWaiter> waiters;
    if(stream.coalesceKey != 0) {
        auto& pending = entry.meta.coalescePending;
        auto pit = pending.find(stream.coalesceKey);
        if(pit != pending.end()) {
            waiters = std::move(pit->second.waiters);
            pending.erase(pit);
        }
    }

    NotifyCoalesceWaiters(waiters, slotCtx->slotState, outputObj, desc);

    if(stream.parseState && desc.destroyParseState)
        desc.destroyParseState(stream.parseState);

    if(stream.clientCtx && stream.clientCtx->generationId == stream.generationId) {
        stream.clientCtx->endpointCtx = nullptr;
        stream.clientCtx->streamKey = 0;

        if(outputObj) {
            RecordEndpointCompletion(slotCtx->endpointIdx, desc, outputObj, stream.clientCtx->endpointStartUs);
            HandleClientAsyncCallback(stream.clientCtx, {outputObj, 0, {.unused = 0}, AsyncStatus::COMPLETED}, false);
        }
        else {
            AsyncResult failResult{};
            failResult.data = nullptr;
            failResult.dataLen = 0;
            failResult.endpointStatus = EndpointStatus::INTERNAL_ERROR;
            failResult.status = AsyncStatus::IO_FAILURE;
            HandleClientAsyncCallback(stream.clientCtx, failResult, false);
        }
    }

    // Client vanished before the result arrived (shouldn't normally happen: ReleaseClient erases
    // this entry synchronously on disconnect) and nothing else claimed ownership.
    else if(outputObj && desc.destroyOutput)
        desc.destroyOutput(outputObj);

    // Every in-flight stream just finished: the connection has gone idle, even though a
    // multiplexed protocol's parse() never signals a keep-alive boundary the way a
    // non-multiplexed one does. Arm the idle timeout directly rather than
    // ReturnEndpointToPool, which would free the slot's pool bitmap bit and hide it from
    // FindMultiplexableSlot; a new request reusing this slot already overwrites this with
    // requestTimeoutSeconds via RefreshExpiry in SendPayloadMultiplexed.
    if(slotCtx->pendingStreams->empty() && !slotCtx->isShuttingDown) {
        // No streams left, the slot is idle keep-alive, so drop its in-use lease (bit stays held
        // for FindMultiplexableSlot) before arming the idle timeout.
        MultiplexReleaseLeaseIfIdle(slotCtx);
        RefreshExpiry(slotCtx, entry.meta.config.idleTimeoutSeconds);
    }
}

bool EpollConnectionHandler::ConsumeParsedBytes(EndpointCtx* slotCtx, std::uint32_t consumed)
{
    if(consumed == 0)
        return true;

    auto& rwBuf = slotCtx->rwBuffer;
    auto* readMeta = rwBuf.GetReadMeta();

    // Guard: consumed must never exceed what we actually have
    if(consumed > readMeta->dataLength) {
        logger_.Error("[Epoll]: While handling endpoint receive, parse returned consumed=", consumed,
                      " but dataLength=", readMeta->dataLength, ", closing slot");
        Close(slotCtx, true);
        return false;
    }

    endpointMetrics_[slotCtx->endpointIdx].bytesIn += consumed;

    const std::uint32_t remaining = readMeta->dataLength - consumed;
    if(remaining > 0)
        std::memmove(rwBuf.GetReadData(), rwBuf.GetReadData() + consumed, remaining);

    readMeta->dataLength = remaining;
    return true;
}

void EpollConnectionHandler::TeardownSlotOnFailure(EndpointCtx* slotCtx, EndpointEntry& entry)
{
    // isShuttingDown is set before Finalize (not just inside Close, which runs after): Finalize
    // fires client callbacks that synchronously resume suspended coroutines, and an ordinary
    // app-level retry from one of those can reenter SendPayloadMultiplexed before this function
    // returns. The flag blocks FindMultiplexableSlot from handing out this exact slot while its
    // fate is still being decided.
    slotCtx->isShuttingDown = 1;
    endpointMetrics_[slotCtx->endpointIdx].otherErrors++;
    FinalizeEndpointRequest(slotCtx, entry.meta, false);
    Close(slotCtx, true);
}

void EpollConnectionHandler::FailSlotOnParseError(EndpointCtx* slotCtx, EndpointEntry& entry)
{
    // Notify the in-flight client if there is one, since it's suspended on co_await. Only the
    // single-slot path ever sets clientCtx; a multiplexed slot's streams are failed instead by
    // FinalizeEndpointRequest inside TeardownSlotOnFailure below.
    if(slotCtx->clientCtx) {
        ClientCtx* clientCtx = slotCtx->clientCtx;
        slotCtx->clientCtx = nullptr;
        clientCtx->endpointCtx = nullptr;

        AsyncResult result{};
        result.data = nullptr;
        result.dataLen = 0;
        result.status = AsyncStatus::IO_FAILURE;
        result.endpointStatus = EndpointStatus::INTERNAL_ERROR;

        HandleClientAsyncCallback(clientCtx, result, false);
    }

    TeardownSlotOnFailure(slotCtx, entry);
}

bool EpollConnectionHandler::IsIncrementallyConsumed(EndpointCtx* slotCtx) const
{
    if(slotCtx->isStreaming)
        return true;

    // Same test HandleEndpointEpollIn routes on: nothing in flight means these are pushes
    return !slotCtx->clientCtx && !slotCtx->isAborted &&
           !(slotCtx->pendingStreams && !slotCtx->pendingStreams->empty());
}

void EpollConnectionHandler::HandleEndpointPush(EndpointCtx* slotCtx, bool isEof)
{
    WFX_TRACE();

    auto& desc = endpoints_[slotCtx->endpointIdx].meta.desc;
    auto& rwBuf = slotCtx->rwBuffer;

    // No push handler means these bytes are what they always were, a backend talking out of
    // turn. isEof is the peer closing an idle keep-alive, which no handler can act on either.
    if(!desc.onPush || isEof) {
        Close(slotCtx, true);
        return;
    }

    // Drain every complete message that arrived; Receive() already read until EAGAIN
    while(rwBuf.GetReadMeta()->dataLength > 0) {
        std::uint32_t consumed = 0;

        if(!desc.onPush(slotCtx->slotState, rwBuf.GetReadData(), rwBuf.GetReadMeta()->dataLength, &consumed)) {
            Close(slotCtx, true);
            return;
        }

        // Partial message, keep the bytes buffered and wait for the next read
        if(consumed == 0)
            break;

        if(!ConsumeParsedBytes(slotCtx, consumed))
            return;
    }

    // Reading may have stopped early on a full buffer, and ET never re-fires for bytes left in the
    // socket, so re-arm now that draining freed room.
    if(!RegisterEpoll(slotCtx, EPOLL_CTL_MOD)) {
        logger_.Error("[Epoll]: 'HandleEndpointPush -> RegisterEpoll(MOD)' failed: ", strerror(errno));
        Close(slotCtx, true);
    }
}

void EpollConnectionHandler::HandleEndpointReceive(EndpointCtx* slotCtx, bool isEof)
{
    WFX_TRACE();

    auto& entry = endpoints_[slotCtx->endpointIdx];

    // hasCapacity is fixed at AllocateEndpoint time and exact here: a multiplexed slot resolves
    // through pendingStreams/completedKey and always has clientCtx == nullptr by the time it
    // reaches EVENT_ENDPOINT_RECV (FlushDeferredRequest nulls it), single-slot is the reverse.
    if(entry.meta.desc.hasCapacity)
        HandleReceiveMultiplexed(slotCtx, entry, isEof);
    else
        HandleReceiveSingleSlot(slotCtx, entry, isEof);
}

ParseResult EpollConnectionHandler::ParseSingleSlotStep(EndpointCtx* slotCtx, EndpointEntry& entry, bool isEof,
                                                        bool* outSlotGone)
{
    auto& desc = entry.meta.desc;
    auto& rwBuf = slotCtx->rwBuffer;

    std::uint32_t consumed = 0;
    std::uint64_t unusedCompletedKey = 0; // multiplexed-only out-param, never read on this path

    const ParseResult pr =
        desc.parse(slotCtx->slotState, slotCtx->parseStateObj, rwBuf.GetReadData(), rwBuf.GetReadMeta()->dataLength,
                   &consumed, slotCtx->outputObj, isEof, &unusedCompletedKey);

    *outSlotGone = !ConsumeParsedBytes(slotCtx, consumed);

    return pr;
}

void EpollConnectionHandler::HandleReceiveSingleSlot(EndpointCtx* slotCtx, EndpointEntry& entry, bool isEof)
{
    while(true) {
        bool slotGone = false;
        const ParseResult pr = ParseSingleSlotStep(slotCtx, entry, isEof, &slotGone);

        if(slotGone)
            return;

        switch(pr) {
            case ParseResult::INCOMPLETE:
                // At EOF there are no more bytes coming. A parser that still wants more means the
                // response was truncated, fail the in-flight request and any waiters, then close.
                if(isEof) {
                    TeardownSlotOnFailure(slotCtx, entry);
                    return;
                }

                // Need more bytes. eventType is already EVENT_ENDPOINT_RECV from Receive()
                return;

            case ParseResult::CHUNK_READY:
            case ParseResult::CHUNK_READY_FETCH:
                DeliverStreamChunk(slotCtx, entry, pr);
                return;

            case ParseResult::COMPLETE_KEEP_ALIVE:
            case ParseResult::COMPLETE_CLOSE:
                CompleteSingleSlotRequest(slotCtx, entry, pr);
                return;

            case ParseResult::ERROR:
            default:
                FailSlotOnParseError(slotCtx, entry);
                return;
        }
    }
}

void EpollConnectionHandler::DeliverStreamChunk(EndpointCtx* slotCtx, EndpointEntry& entry, ParseResult pr)
{
    ClientCtx* clientCtx = slotCtx->clientCtx;

    // A chunk with nobody waiting means the protocol emitted one outside a Stream() call
    if(!clientCtx) {
        logger_.Error("[Epoll]: parse returned a chunk with no client waiting for endpoint '", entry.meta.hostname,
                      "'");
        TeardownSlotOnFailure(slotCtx, entry);
        return;
    }

    // The request stays in flight, so clientCtx and outputObj are deliberately left in place:
    // the caller borrows the chunk until its next Next(), which is what keeps peak memory at
    // one chunk instead of the whole response.
    slotCtx->isStreaming = 1;
    slotCtx->needsFetch = (pr == ParseResult::CHUNK_READY_FETCH) ? 1 : 0;

    // PENDING marks this as a chunk; the final delivery uses SUCCESS so the caller can stop
    HandleClientAsyncCallback(clientCtx,
                              {slotCtx->outputObj,
                               0,
                               {.endpointStatus = EndpointStatus::PENDING},
                               AsyncStatus::COMPLETED},
                              false);
}

EndpointStatus EpollConnectionHandler::StreamNext(ClientCtx* clientCtx, const void* req, AsyncData asyncData)
{
    WFX_TRACE();

    clientCtx->asyncData = asyncData;

    const EndpointStatus status = StreamNextImpl(clientCtx, req);

    // Only PENDING resumes the caller through the completion callback
    // Anything else resumes it synchronously (await_suspend returns false), so nothing would ever
    // consume the asyncData installed above and ReleaseClient would later fire it against a
    // coroutine frame that finished long ago.
    if(status != EndpointStatus::PENDING)
        clientCtx->asyncData = {};

    return status;
}

EndpointStatus EpollConnectionHandler::StreamNextImpl(ClientCtx* clientCtx, const void* req)
{
    EndpointCtx* slotCtx = clientCtx->endpointCtx;
    if(!slotCtx || !slotCtx->isStreaming)
        return EndpointStatus::INVALID_KEY;

    auto& entry = endpoints_[slotCtx->endpointIdx];
    auto& meta = entry.meta;

    // Cursor/paging protocols send nothing until asked. serialize() gets the same req back and
    // uses parseState (portal name, paging_state, cursor, ...) to emit the continuation instead.
    if(slotCtx->needsFetch) {
        slotCtx->needsFetch = 0;

        auto& rwBuf = slotCtx->rwBuffer;
        if(!rwBuf.IsWriteInitialized() && !rwBuf.InitWriteBuffer(config_.networkConfig.maxSendBufferSize))
            return EndpointStatus::BUFFER_ERROR;

        const EndpointStatus sr = SerializeSingleSlot(slotCtx, meta, req);
        if(sr != EndpointStatus::SUCCESS)
            return sr;

        return ArmSendOrConnect(slotCtx, entry, false);
    }

    // Server-driven: the last read may already hold the next chunk. Parse it here rather than
    // waiting on epoll, which in ET mode will never re-fire for bytes already drained into
    // rwBuffer. Delivery is by return status, not the client callback: the caller's coroutine
    // is still inside await_suspend and resuming it from here would re-enter a half-suspended
    // frame.
    while(slotCtx->rwBuffer.GetReadMeta()->dataLength > 0) {
        bool slotGone = false;
        const ParseResult pr = ParseSingleSlotStep(slotCtx, entry, false, &slotGone);

        if(slotGone)
            return EndpointStatus::INTERNAL_ERROR;

        // Need more bytes than are buffered, fall through and wait on epoll
        if(pr == ParseResult::INCOMPLETE)
            break;

        switch(pr) {
            case ParseResult::CHUNK_READY:
            case ParseResult::CHUNK_READY_FETCH:
                slotCtx->needsFetch = (pr == ParseResult::CHUNK_READY_FETCH) ? 1 : 0;
                return EndpointStatus::CHUNK_AVAILABLE;

            case ParseResult::COMPLETE_KEEP_ALIVE:
            case ParseResult::COMPLETE_CLOSE:
                CompleteStreamInline(slotCtx, entry, pr);
                return EndpointStatus::SUCCESS;

            case ParseResult::ERROR:
            default:
                FailSlotOnParseError(slotCtx, entry);
                return EndpointStatus::INTERNAL_ERROR;
        }
    }

    EnterState(slotCtx, EventType::EVENT_ENDPOINT_RECV);
    RefreshExpiry(slotCtx, meta.config.requestTimeoutSeconds);

    if(!RegisterEpoll(slotCtx, EPOLL_CTL_MOD)) {
        logger_.Error("[Epoll]: 'StreamNext -> RegisterEpoll(MOD)' failed: ", strerror(errno));
        return EndpointStatus::EPOLL_ERROR;
    }

    return EndpointStatus::PENDING;
}

const void* EpollConnectionHandler::StreamChunk(ClientCtx* clientCtx)
{
    EndpointCtx* slotCtx = clientCtx->endpointCtx;

    return (slotCtx && slotCtx->isStreaming) ? slotCtx->outputObj : nullptr;
}

void EpollConnectionHandler::CompleteStreamInline(EndpointCtx* slotCtx, EndpointEntry& entry, ParseResult pr)
{
    // Same teardown CompleteSingleSlotRequest does, minus the client callback: StreamNext
    // reports completion through its return value instead. The stream owns no output to hand
    // over, every chunk was borrowed, so FinalizeEndpointRequest destroys it as usual.
    auto& rwBuf = slotCtx->rwBuffer;

    ClientCtx* clientCtx = slotCtx->clientCtx;
    if(clientCtx) {
        slotCtx->clientCtx = nullptr;
        clientCtx->endpointCtx = nullptr;
    }

    slotCtx->isStreaming = 0;
    slotCtx->needsFetch = 0;

    FinalizeEndpointRequest(slotCtx, entry.meta, pr == ParseResult::COMPLETE_KEEP_ALIVE);

    if(pr == ParseResult::COMPLETE_KEEP_ALIVE) {
        // Same trailing-byte split as CompleteSingleSlotRequest
        auto& desc = entry.meta.desc;

        if(!desc.onPush)
            rwBuf.ClearReadBuffer();

        EnterState(slotCtx, EventType::EVENT_ENDPOINT_RECV);
        ReturnEndpointToPool(slotCtx);

        if(desc.onPush && rwBuf.GetReadMeta()->dataLength > 0)
            HandleEndpointPush(slotCtx, false);
    }
    else
        Close(slotCtx);
}

void EpollConnectionHandler::HandleReceiveMultiplexed(EndpointCtx* slotCtx, EndpointEntry& entry, bool isEof)
{
    auto& desc = entry.meta.desc;
    auto& rwBuf = slotCtx->rwBuffer;

    while(true) {
        std::uint32_t consumed = 0;
        std::uint64_t completedKey = 0;
        const ParseResult pr =
            desc.parse(slotCtx->slotState, slotCtx->parseStateObj, rwBuf.GetReadData(), rwBuf.GetReadMeta()->dataLength,
                       &consumed, slotCtx->outputObj, isEof, &completedKey);

        if(!ConsumeParsedBytes(slotCtx, consumed))
            return;

        // A non-zero key means one particular stream just finished, resolve and erase only that
        // one. The connection itself may still be alive (pr == INCOMPLETE is the normal case
        // here, e.g. an HTTP/2 stream ending on a connection with others still open), so loop
        // back for more frames instead of falling into the switch below.
        if(completedKey != 0 && slotCtx->pendingStreams) {
            ResolveMultiplexedStream(slotCtx, entry, completedKey);

            if(pr == ParseResult::INCOMPLETE)
                continue;
        }

        switch(pr) {
            case ParseResult::INCOMPLETE:
                // Same truncated-response handling as the single-slot path; every stream still
                // pending on this slot is failed by FinalizeEndpointRequest during teardown.
                if(isEof) {
                    TeardownSlotOnFailure(slotCtx, entry);
                    return;
                }

                return;

            case ParseResult::COMPLETE_KEEP_ALIVE:
            case ParseResult::COMPLETE_CLOSE:
                // No single primary request lives in slotCtx->clientCtx on this path (every
                // request goes through pendingStreams, resolved above via completedKey), so
                // this return value only ever means the whole connection is done. Behave like
                // a plain teardown/idle-return and let FinalizeEndpointRequest fail whatever
                // streams were still orphaned on the slot.
                slotCtx->isShuttingDown = 1;
                FinalizeEndpointRequest(slotCtx, entry.meta, false);

                if(pr == ParseResult::COMPLETE_KEEP_ALIVE) {
                    // The slot survives and goes back to the idle pool healthy, so clear the
                    // flag again. It must not stay permanently poisoned against future
                    // multiplexing just because this one connection cycle finished.
                    slotCtx->isShuttingDown = 0;
                    EnterState(slotCtx, EventType::EVENT_ENDPOINT_RECV);
                    rwBuf.ClearReadBuffer();
                    ReturnEndpointToPool(slotCtx);
                }
                else
                    Close(slotCtx);

                return;

            case ParseResult::ERROR:
            default:
                FailSlotOnParseError(slotCtx, entry);
                return;
        }
    }
}

void EpollConnectionHandler::CompleteSingleSlotRequest(EndpointCtx* slotCtx, EndpointEntry& entry, ParseResult pr)
{
    auto& desc = entry.meta.desc;
    auto& rwBuf = slotCtx->rwBuffer;

    // A stream ending: every chunk was borrowed, so there's no owned output to hand over. Signal
    // completion with no data and let the shared teardown destroy the output object as usual.
    if(slotCtx->isStreaming) {
        ClientCtx* streamClient = slotCtx->clientCtx;

        endpointMetrics_[slotCtx->endpointIdx].completed++;
        if(streamClient)
            RecordEndpointLatency(slotCtx->endpointIdx, streamClient->endpointStartUs);

        CompleteStreamInline(slotCtx, entry, pr);

        if(streamClient)
            HandleClientAsyncCallback(streamClient,
                                      {nullptr, 0, {.endpointStatus = EndpointStatus::SUCCESS}, AsyncStatus::COMPLETED},
                                      false);

        return;
    }

    ClientCtx* clientCtx = slotCtx->clientCtx;
    void* outputObj = slotCtx->outputObj;

    std::vector<CoalesceWaiter> waiters;

    // Extract waiters and remove from coalesce map (O(1) via stored key)
    auto& pending = entry.meta.coalescePending;
    if(slotCtx->coalesceKey != 0) {
        auto it = pending.find(slotCtx->coalesceKey);
        if(it != pending.end()) {
            waiters = std::move(it->second.waiters);
            pending.erase(it);
        }

        slotCtx->coalesceKey = 0;
    }

    slotCtx->clientCtx = nullptr;
    if(clientCtx)
        clientCtx->endpointCtx = nullptr;
    slotCtx->outputObj = nullptr; // disown before any cleanup

    // clientCtx is null when the client aborted mid-request (isAborted) and this is the real
    // response arriving after the fact: 0 skips the latency sample (same signal as latency
    // being off), status-code counters below still record normally.
    RecordEndpointCompletion(slotCtx->endpointIdx, desc, outputObj, clientCtx ? clientCtx->endpointStartUs : 0);

    // Fan out clones to waiters while slotCtx->slotState is still live
    // This MUST happen before close/pool-return: COMPLETE_CLOSE destroys slotState
    NotifyCoalesceWaiters(waiters, slotCtx->slotState, outputObj, desc);

    if(pr == ParseResult::COMPLETE_KEEP_ALIVE) {
        // Only reset parse state, do NOT touch outputObj here
        // If resetParseState is absent, destroy+null so SendPayload recreates fresh next request.
        // Without this, a keep-alive slot would carry dirty parse state into the next request.
        if(slotCtx->parseStateObj) {
            if(desc.resetParseState)
                desc.resetParseState(slotCtx->parseStateObj);
            else {
                desc.destroyParseState(slotCtx->parseStateObj);
                slotCtx->parseStateObj = nullptr;
            }
        }

        // Bytes trailing a response are a smuggle attempt without onPush, so they go
        // With it they are server-initiated data, and dropping them would make delivery depend on
        // whether the peer coalesced them into the response's segment.
        if(!desc.onPush)
            rwBuf.ClearReadBuffer();

        // Back to a perfectly ordinary idle pooled connection, whether or not it got here via
        // onAbort (the CLOSE path below clears this too, via Reset() in ReleaseEndpoint).
        slotCtx->isAborted = 0;

        EnterState(slotCtx, EventType::EVENT_ENDPOINT_RECV);
        ReturnEndpointToPool(slotCtx);

        // After the pool return so the slot reads as idle, but before the client callback, which
        // could otherwise take this slot and route these bytes to parse.
        if(desc.onPush && rwBuf.GetReadMeta()->dataLength > 0)
            HandleEndpointPush(slotCtx, false);
    }
    else {
        // CLOSE path: parse state destroy only, output handled below
        if(slotCtx->parseStateObj && desc.destroyParseState) {
            desc.destroyParseState(slotCtx->parseStateObj);
            slotCtx->parseStateObj = nullptr;
        }

        Close(slotCtx);
    }

    // Ownership transfers to the primary's EndpointOutput<T> RAII wrapper. If the client aborted
    // (clientCtx null), nobody is waiting to receive outputObj: destroy it ourselves instead of leaking.
    if(clientCtx)
        HandleClientAsyncCallback(clientCtx, {outputObj, 0, {.unused = 0}, AsyncStatus::COMPLETED}, false);
    else if(outputObj && desc.destroyOutput)
        desc.destroyOutput(outputObj);
}

void EpollConnectionHandler::FireOnConnect(EndpointCtx* slotCtx, EndpointEntry& entry)
{
    auto& desc = entry.meta.desc;

    // Send / Receive go through the endpoint API directly (SlotHandle::Send/Receive
    // call SlotSend/SlotReceive themselves), nothing needed here for either.
    // Close is nullptr. Slot lifetime is managed by the engine via ConnectResult
    EndpointSlotHandle handle{};
    handle.impl = slotCtx;
    handle.negotiatedProtocol = Shared::GetEndpointAPIExt1()->negotiatedProtocol;
    handle.close = nullptr;

    AsyncData onDone{};
    onDone.userData = slotCtx;
    onDone.asyncComplete = OnSlotConnected;
    onDone.asyncDestroy = nullptr;

    slotCtx->asyncData = onDone;
    EnterState(slotCtx, EventType::EVENT_ENDPOINT_ONCONNECT);
    slotCtx->inOnConnectPhase = 1;

    // onConnect is guaranteed non-null here, FireOnConnect only runs when HandleEndpointHandshake
    // saw eventType == EVENT_ENDPOINT_ONCONNECT, which TryHandshake only sets when isSideConnection
    // || onConnect was true, and the isSideConnection branch above this call already ruled itself
    // out. The analyzer can't see that cross-branch invariant through EnterState.
    // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
    desc.onConnect(handle, slotCtx->slotState, onDone.asyncComplete, onDone.userData);
}

void EpollConnectionHandler::FireOnAbort(EndpointCtx* slotCtx, EndpointEntry& entry)
{
    auto& desc = entry.meta.desc;

    // Unlike FireOnConnect, slotCtx does NOT transition eventType or set inOnConnectPhase: it's
    // still mid-request (EVENT_ENDPOINT_RECV), still owed a real response. onAbort only ever
    // drives a separate connection (OpenSideConnection), never this primary ctx directly
    // (AbortSlotHandle doesn't even expose Send/Receive on it).
    EndpointSlotHandle handle{};
    handle.impl = slotCtx;
    handle.negotiatedProtocol = Shared::GetEndpointAPIExt1()->negotiatedProtocol;
    handle.close = nullptr;

    // No completion callback: the original slot's fate is its own receive/parse/timeout
    // cycle, not onAbort's return value. Task<void>'s final_suspend null-checks onDone
    // before calling it, so nullptr here is a plain no-op, not a missing piece.
    desc.onAbort(handle, slotCtx->slotState, nullptr, nullptr);
}

bool EpollConnectionHandler::FlushDeferredRequest(EndpointCtx* slotCtx, EndpointEntry& entry)
{
    auto& meta = entry.meta;
    auto& desc = meta.desc;

    const void* req = slotCtx->pendingConnectReq;
    slotCtx->pendingConnectReq = nullptr;

    auto& rwBuf = slotCtx->rwBuffer;
    if(!rwBuf.IsWriteInitialized() && !rwBuf.InitWriteBuffer(config_.networkConfig.maxSendBufferSize)) {
        FailDeferredRequest(slotCtx, EndpointStatus::BUFFER_ERROR);
        return false;
    }

    // Non-multiplexed: same serialize step SendPayload uses
    if(!desc.hasCapacity) {
        const EndpointStatus sr = SerializeSingleSlot(slotCtx, meta, req);
        if(sr != EndpointStatus::SUCCESS) {
            FailDeferredRequest(slotCtx, sr);
            return false;
        }

        if(slotCtx->coalesceKey != 0)
            meta.coalescePending[slotCtx->coalesceKey].inflight = slotCtx;

        return true;
    }

    // Multiplexed: same serialize step SendPayloadMultiplexed uses. clientCtx, parseStateObj
    // and coalesceKey were stashed on the slot in place of a real PendingStream entry (which
    // needs a streamKey that only exists once serialize() actually runs).
    std::uint64_t streamKey = 0;
    const EndpointStatus sr = SerializeMultiplexed(slotCtx, meta, req, &streamKey);
    if(sr != EndpointStatus::SUCCESS) {
        FailDeferredRequest(slotCtx, sr);
        return false;
    }

    ClientCtx* client = slotCtx->clientCtx;
    void* parseState = slotCtx->parseStateObj;
    const std::uint64_t coalesceKey = slotCtx->coalesceKey;

    slotCtx->clientCtx = nullptr;
    slotCtx->parseStateObj = nullptr;
    slotCtx->coalesceKey = 0;

    if(coalesceKey != 0)
        meta.coalescePending[coalesceKey].inflight = slotCtx;

    if(!slotCtx->pendingStreams)
        slotCtx->pendingStreams = Shared::New<PendingStreamMap>();

    (*slotCtx->pendingStreams)[streamKey] = PendingStream{client, parseState, coalesceKey, client->generationId};
    client->streamKey = streamKey;

    return true;
}

void EpollConnectionHandler::FailDeferredRequest(EndpointCtx* slotCtx, EndpointStatus status)
{
    // NULL clientCtx first so Close() -> ReleaseEndpoint() doesn't ALSO try to notify it,
    // with a disconnect-reason-derived status instead of this specific one.
    ClientCtx* client = slotCtx->clientCtx;
    slotCtx->clientCtx = nullptr;
    slotCtx->coalesceKey = 0; // never registered in coalescePending, just drop it

    if(client) {
        client->endpointCtx = nullptr;
        client->streamKey = 0;

        AsyncResult result{};
        result.data = nullptr;
        result.dataLen = 0;
        result.status = AsyncStatus::IO_FAILURE;
        result.endpointStatus = status;
        HandleClientAsyncCallback(client, result, false);
    }

    // FinalizeEndpointRequest (via Close -> ReleaseEndpoint) destroys parseStateObj for us,
    // whether it holds a real single-slot parse state or a stashed multiplexed reqParseState.
    Close(slotCtx, true);
}

void EpollConnectionHandler::OnSlotConnected(void* ud, AsyncResult result)
{
    auto* slotCtx = static_cast<EndpointCtx*>(ud);
    auto& entry = GlobalInstance->endpoints_[slotCtx->endpointIdx];

    slotCtx->inOnConnectPhase = 0;

    // Any failure routes through the connect-failure funnel: a client-waiting slot fails fast, an
    // explicit FATAL is discarded, and a background slot returning RETRY (or a coroutine that errored
    // out) reconnects with backoff. connectResult is only meaningful when the coroutine completed.
    if(result.status != AsyncStatus::COMPLETED || result.connectResult == ConnectResult::FATAL ||
       result.connectResult == ConnectResult::RETRY) {
        const bool fatal = (result.status == AsyncStatus::COMPLETED && result.connectResult == ConnectResult::FATAL);
        GlobalInstance->HandleConnectFailure(slotCtx, entry, fatal);
        return;
    }

    // onConnect phase is done. The read buffer may contain protocol handshake data
    // (auth packets, server greeting, etc.) consumed by the coroutine via SlotReceive.
    // That data must not bleed into the response parse loop, so clear it before the
    // slot enters normal request/response operation.
    slotCtx->rwBuffer.ClearReadBuffer();

    // A request deferred by SendPayload/SendPayloadMultiplexed only serializes now, so the
    // handshake's own writes always precede it on the wire. On failure the client is already
    // notified and the slot already torn down, so just stop here.
    if(slotCtx->pendingConnectReq && !GlobalInstance->FlushDeferredRequest(slotCtx, entry))
        return;

    // ConnectResult::READY, slot is pooled and ready. A real request is waiting either via the
    // single-slot clientCtx (non-multiplexed) or a freshly inserted pendingStreams entry
    // (multiplexed, see SendPayloadMultiplexed). Either way the write buffer already has
    // serialized bytes queued. Prewarm sets neither.
    if(slotCtx->clientCtx || (slotCtx->pendingStreams && !slotCtx->pendingStreams->empty())) {
        // Non-prewarm: SendPayload already serialized a request into the write buffer
        // Drive the write directly instead of calling RegisterEpoll(MOD) and waiting
        // for EPOLLOUT, because in ET mode the EPOLLOUT edge was consumed when EVENT_CONNECT
        // fired and a MOD on an already-writable fd does not generate a new edge.
        GlobalInstance->EnterState(slotCtx, EventType::EVENT_ENDPOINT_SEND);
        GlobalInstance->Write(slotCtx);
    }
    else {
        // Prewarm: no client waiting. Return this slot to the free list so
        // SendPayload's AllocSlot can actually lease it for a future request.
        // Without this, prewarmed slots stay permanently marked allocated and
        // are never reused, silently shrinking the effective pool by 'prewarm' count.
        GlobalInstance->EnterState(slotCtx, EventType::EVENT_ENDPOINT_RECV);
        GlobalInstance->ReturnEndpointToPool(slotCtx);

        if(!GlobalInstance->RegisterEpoll(slotCtx, EPOLL_CTL_MOD)) {
            GlobalInstance->logger_.Error("[Epoll]: 'RegisterEpoll(MOD)' failed for endpoint after onConnect prewarm: ",
                                          strerror(errno));
            GlobalInstance->Close(slotCtx, true);
        }
    }
}

std::uint32_t EpollConnectionHandler::ComputeBackoffSeconds(const EndpointConfig& config, std::uint16_t attempt)
{
    // Exponential: base * 2 ^ attempt, capped at max (computed without overflow)
    std::uint64_t exp = config.reconnectBackoffBase;
    for(std::uint16_t i = 0; i < attempt && exp < config.reconnectBackoffMax; i++)
        exp <<= 1;

    const std::uint32_t cap = static_cast<std::uint32_t>(std::min<std::uint64_t>(exp, config.reconnectBackoffMax));

    // Pick uniformly in [0, cap] so a whole pool never reconnects in lockstep and re-DDoSes a
    // recovering upstream. xorshift64 is plenty for jitter.
    reconnectRngState_ ^= reconnectRngState_ << 13;
    reconnectRngState_ ^= reconnectRngState_ >> 7;
    reconnectRngState_ ^= reconnectRngState_ << 17;

    const std::uint32_t jittered = (cap == 0) ? 0 : static_cast<std::uint32_t>(reconnectRngState_ % (cap + 1ULL));

    // Clamp to at least 1s so we never busy-retry at a 0s delay
    return jittered < 1 ? 1 : jittered;
}

void EpollConnectionHandler::ScheduleReconnect(EndpointCtx* ctx, EndpointEntry& entry)
{
    WFX_TRACE();

    auto& meta = entry.meta;
    const std::uint32_t idx = entry.pool.GetIndex(ctx);

    // Soft close: drop the socket + TLS + per-request objects, but KEEP the slot reserved in the
    // pool and KEEP slotState alive (the slot persists across retry attempts; onConnect re-runs
    // against the same slotState, and onDisconnect fires only on final ejection via ReleaseEndpoint).
    timerWheel_.Cancel(meta.timerBase + idx);
    (void)RegisterEpoll(ctx, EPOLL_CTL_DEL);

    if(ctx->sslConn) {
        sslHandler_->ForceShutdown(ctx->sslConn);
        ctx->sslConn = nullptr;
    }
    if(ctx->socket != WFX_INVALID_SOCKET) {
        close(ctx->socket);
        ctx->socket = WFX_INVALID_SOCKET;
    }

    // If a connect-phase timeout interrupted a suspended onConnect coroutine, destroy its frame now
    // (mirrors ReleaseEndpoint) so it doesn't leak. A fresh coroutine starts on the reconnect attempt.
    if(ctx->inOnConnectPhase)
        HandleEndpointAsyncCallback(ctx, {}, true);

    FinalizeEndpointRequest(ctx, meta, false);

    ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
    ctx->inOnConnectPhase = 0;
    ctx->clientCtx = nullptr; // background slot, no waiting client (already failed/never had one)
    ctx->isAwaitingReconnect = 1;

    const std::uint32_t backoff = ComputeBackoffSeconds(meta.config, ctx->reconnectAttempts);
    ctx->reconnectAttempts++;
    endpointMetrics_[ctx->endpointIdx].reconnects++;

    pendingReconnects_.push_back(
        {NowMs() + static_cast<std::uint64_t>(backoff) * 1000ULL, ctx->endpointIdx, ctx->generationId, idx});

    logger_.Debug("[Epoll]: Endpoint '", meta.hostname, "' slot ", idx, " (gen ", ctx->generationId,
                  ") connect failed, reconnect attempt ", ctx->reconnectAttempts, "/", meta.config.maxReconnectAttempts,
                  " in ", backoff, "s");
}

void EpollConnectionHandler::HandleConnectFailure(EndpointCtx* ctx, EndpointEntry& entry, bool fatal,
                                                  DisconnectReason reason, bool tls)
{
    WFX_TRACE();

    auto& meta = entry.meta;

    // Permanent teardown when: a client is actively waiting (fail it fast, it can retry at the app
    // layer rather than block on backoff), the failure is fatal, or retries are exhausted. Close ->
    // ReleaseEndpoint fires onDisconnect once, notifies any waiting client (with reason so the
    // status code is accurate), and frees the slot.
    const bool exhausted = ctx->reconnectAttempts >= meta.config.maxReconnectAttempts;
    if(ctx->clientCtx || fatal || exhausted) {
        // A background slot (no waiting client) being permanently discarded is the operationally
        // significant event worth alerting on. Client-driven failures already surface to the caller
        // via the returned status, so they don't need an engine-level error here.
        if(!ctx->clientCtx)
            logger_.Error("[Epoll]: Endpoint '", meta.hostname, "' slot ", entry.pool.GetIndex(ctx),
                          " giving up after ", ctx->reconnectAttempts, "/", meta.config.maxReconnectAttempts,
                          " reconnect attempts, slot ejected");

        if(tls)
            endpointMetrics_[ctx->endpointIdx].tlsFailures++;
        else
            endpointMetrics_[ctx->endpointIdx].connectFailures++;

        FinalizeEndpointRequest(ctx, meta, false);
        Close(ctx, true, reason);
        return;
    }

    // Background (prewarm/pool) slot, transient failure, retries remaining: heal in the background
    ScheduleReconnect(ctx, entry);
}

void EpollConnectionHandler::HandleReconnects()
{
    if(pendingReconnects_.empty())
        return;

    const std::uint64_t now = NowMs();

    for(std::size_t i = 0; i < pendingReconnects_.size();) {
        const PendingReconnect& pr = pendingReconnects_[i];
        if(pr.wakeAtMs > now) {
            i++;
            continue;
        }

        // Due: remove the entry (order doesn't matter, swap-erase)
        const PendingReconnect due = pr;
        pendingReconnects_[i] = pendingReconnects_.back();
        pendingReconnects_.pop_back();

        auto& entry = endpoints_[due.endpointIdx];
        EndpointCtx* ctx = entry.pool.GetPtr(due.slotIdx);

        // Stale guard: slot must still be the same parked one. isAwaitingReconnect is cleared by
        // Reset() on teardown (covers freed-but-not-reused), generationId catches freed-and-reused.
        if(!ctx || !ctx->isAwaitingReconnect || ctx->generationId != due.generationId)
            continue;

        ctx->isAwaitingReconnect = 0;

        // Re-attempt. CreateAndConnect advances nextAddrIdx, so this naturally rotates to the next
        // resolved IP. WrapConnect re-registers epoll and re-arms the connect timeout.
        const EndpointStatus s = WrapConnect(ctx, entry);
        if(s != EndpointStatus::PENDING)
            HandleConnectFailure(ctx, entry, false); // immediate failure: reschedule or eject
    }
}

void EpollConnectionHandler::HandlePrewarm()
{
    for(std::uint16_t i = 0; i < static_cast<std::uint16_t>(endpoints_.size()); i++) {
        auto& entry = endpoints_[i];
        auto& desc = entry.meta.desc;
        auto& cfg = entry.meta.config;

        const std::uint32_t prewarmCount = std::min(cfg.prewarm, cfg.connLimit);

        for(std::uint32_t j = 0; j < prewarmCount; j++) {
            EndpointCtx* slotCtx = GetEndpointConnection(i);
            if(!slotCtx)
                break; // pool exhausted for this endpoint

            // Slot state must exist before onConnect fires
            if(!slotCtx->slotState && desc.createSlotState)
                slotCtx->slotState = desc.createSlotState(desc.userCtx);

            const EndpointStatus result = WrapConnect(slotCtx, entry);

            // An immediate (synchronous) failure goes through the same funnel as async ones, so a
            // prewarm slot retries with backoff instead of being silently abandoned. The teardown
            // branch (retries off / exhausted) destroys slotState and frees the slot via Close
            if(result != EndpointStatus::PENDING)
                HandleConnectFailure(slotCtx, entry, false);
        }
    }
}

void EpollConnectionHandler::HandleDnsRefresh(std::uint16_t endpointIdx)
{
    auto& meta = endpoints_[endpointIdx].meta;

    // With MAX_DNS_THREADS capping concurrent resolves, the queue can only exceed
    // MAX_DNS_RESULT_QUEUE_SIZE if eventfd writes are persistently failing and
    // HandleDnsResultReady is never draining it.
    {
        const std::lock_guard<std::mutex> lock(dnsResultMutex_);
        if(dnsResultQueue_.size() >= MAX_DNS_RESULT_QUEUE_SIZE)
            logger_.Fatal("[Epoll]: DNS result queue exceeded ", MAX_DNS_RESULT_QUEUE_SIZE,
                          " entries. Event signaling has persistently failed, DNS refresh dead engine wide");
    }

    // Computed once and reused across all failure paths (semaphore busy, write fail, spawn fail)
    const std::uint64_t retrySchedule = ComputeNextDnsRefresh(MIN_REFRESH_SECONDS, 0, meta.hostname);

    // All resolver slots busy. Reschedule with jitter so waiting endpoints don't
    // all reconverge at the same instant when slots free up.
    if(!dnsThreadSemaphore_.try_acquire()) {
        meta.dnsNextRefreshSeconds = retrySchedule;
        return;
    }

    // Push schedule to ceiling (doubles as the in-flight marker, prevents Run()
    // from spawning a second overlapping resolve for this endpoint).
    meta.dnsNextRefreshSeconds = (NowMs() / 1000) + MAX_REFRESH_SECONDS;

    // meta captured by reference below (safe since endpoints_ is fully
    // populated before Run() starts and never reallocated afterward).
    try {
        std::thread([=, this, &meta]() {
            // Always release the semaphore slot on exit regardless of outcome
            struct SemGuard {
                std::counting_semaphore<MAX_DNS_THREADS>& sem;
                ~SemGuard()
                {
                    sem.release();
                }
            };
            const SemGuard guard{dnsThreadSemaphore_};

            Utils::ResolvedAddrs newAddrs;
            std::uint32_t minTtl = 0;
            const bool ok = DNSResolver::Resolve(meta.hostname.c_str(), meta.port, newAddrs, minTtl);

            // Push AND signal under a single lock. The loop's drain (HandleDnsResultReady) also
            // takes this mutex, so keeping the write() inside the same critical section guarantees
            // back() below still refers to the node we just pushed. Releasing the lock between the
            // push and the back() annotation would let a concurrent drain swap the queue empty
            // (back() on an empty vector is UB) or let another resolver append (wrong node tagged).
            // The reader read()s the eventfd before taking this lock, so it can still unblock a
            // writer that stalls inside write() while holding the mutex.
            {
                const std::lock_guard<std::mutex> lock(dnsResultMutex_);
                dnsResultQueue_.push_back({ok, endpointIdx, minTtl, std::move(newAddrs)});

                const std::uint64_t one = 1;
                const ssize_t written = RetryOnEintr([&] { return write(dnsResultEventFd_, &one, sizeof(one)); });

                // On write error, tag the just-pushed node. If future writes succeed, the errors
                // will be logged. If future writes keep failing, the queue fills rapidly and
                // triggers the MAX_DNS_RESULT_QUEUE_SIZE condition above.
                if(written < 0) {
                    dnsResultQueue_.back().wakeupError = strerror(errno);
                    meta.dnsNextRefreshSeconds = retrySchedule;
                }
            }
        }).detach();
    }
    catch(const std::system_error& e) {
        // Thread spawn failed, release the acquired slot and reschedule for retry
        dnsThreadSemaphore_.release();
        logger_.Error("[Epoll]: Failed to spawn DNS refresh thread: ", e.what());
        meta.dnsNextRefreshSeconds = retrySchedule;
    }
}

void EpollConnectionHandler::HandleDnsResultReady(int sfd)
{
    std::uint64_t val = 0;
    const ssize_t n = RetryOnEintr([&] { return read(sfd, &val, sizeof(val)); });

    if(n < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK)
            return;

        // Same reason as why HandleTimeoutTimer and HandleAsyncTimer fail, too lazy to
        // write the reason again.
        logger_.Fatal("[Epoll]: DNS result eventfd read failed: ", strerror(errno));
    }

    std::vector<DnsResult> results;
    {
        const std::lock_guard<std::mutex> lock(dnsResultMutex_);
        results.swap(dnsResultQueue_);
    }

    for(auto& r : results) {
        // Just sanity checks
        if(r.endpointIdx >= endpoints_.size())
            continue;

        auto& meta = endpoints_[r.endpointIdx].meta;

        if(!r.wakeupError.empty())
            logger_.Warn("[Epoll]: DNS refresh signal failed for '", meta.hostname, "': ", r.wakeupError);

        // Resolver failed, reschedule it again
        if(!r.success) {
            logger_.Warn("[Epoll]: DNS refresh failed for '", meta.hostname, "', keeping existing addresses");
            meta.dnsNextRefreshSeconds = ComputeNextDnsRefresh(MIN_REFRESH_SECONDS, 0, meta.hostname);
            continue;
        }

        meta.addrs = std::move(r.addrs);
        meta.dnsNextRefreshSeconds =
            ComputeNextDnsRefresh(r.minTtlSeconds, meta.config.dnsRefreshSeconds, meta.hostname);
    }
}

std::uint64_t EpollConnectionHandler::PackEpollData(ClientCtx* ctx)
{
    // For a client connection, 'EndpointIdx' will always be 'CLIENT_CONNECTION_TAG'
    const std::uint32_t idx = connections_.GetIndex(ctx);

    // Pack => [( EndpointIdx (16) | GenerationID (16) ) and PoolIdx (Low 32)]
    return (static_cast<std::uint64_t>(CLIENT_CONNECTION_TAG) << 48) |
           (static_cast<std::uint64_t>(ctx->generationId) << 32) | static_cast<std::uint64_t>(idx);
}

std::uint64_t EpollConnectionHandler::PackEpollData(EndpointCtx* ctx)
{
    auto& entry = endpoints_[ctx->endpointIdx];

    const std::uint32_t idx =
        ctx->isSideConnection ? (entry.auxPool.GetIndex(ctx) | AUX_CONNECTION_TAG_BIT) : entry.pool.GetIndex(ctx);

    // Pack => [( EndpointIdx (16) | GenerationID (16) ) and PoolIdx (Low 32, top bit = auxPool tag)]
    return (static_cast<std::uint64_t>(ctx->endpointIdx) << 48) |
           (static_cast<std::uint64_t>(ctx->generationId) << 32) | static_cast<std::uint64_t>(idx);
}

EndpointStatus EpollConnectionHandler::CreateAndConnect(EndpointCtx* ctx, EndpointMetadata& epMeta)
{
    // Should never happen post AllocateEndpoint validation, but guard anyway. An
    // empty list here would be a div/mod-by-zero on the round-robin pick below.
    if(epMeta.addrs.empty())
        return EndpointStatus::CONNECT_FAILURE;

    // Round-robin pick. nextAddrIdx keeps counting up across the uint16_t range and
    // wraps naturally. We only ever use it modulo the current list size, so the
    // stored cursor never needs to know or care about addrs.size() directly.
    const std::uint16_t idx = epMeta.nextAddrIdx % static_cast<std::uint16_t>(epMeta.addrs.size());
    epMeta.nextAddrIdx++;

    ResolvedAddr& chosen = epMeta.addrs[idx];

    ctx->socket = socket(chosen.addr.ss_family, SOCK_STREAM, 0);
    if(ctx->socket < 0)
        return EndpointStatus::SOCKET_FAILURE;

    if(!SetNonBlocking(ctx->socket))
        return EndpointStatus::SOCKET_FAILURE;

    while(true) {
        const int ret = connect(ctx->socket, reinterpret_cast<const sockaddr*>(&chosen.addr), chosen.addrLen);
        if(ret == 0)
            return EndpointStatus::PENDING;

        if(errno == EINTR)
            continue;

        if(errno == EINPROGRESS) {
            EnterState(ctx, EventType::EVENT_CONNECT);
            return EndpointStatus::PENDING;
        }

        return EndpointStatus::CONNECT_FAILURE;
    }
}

//  --- Wrapper Functions ---
void EpollConnectionHandler::WrapAccept(ClientCtx* ctx)
{
    WFX_TRACE();

    if(useHttps_) {
        ctx->sslConn = sslHandler_->Wrap(ctx->socket);
        if(!ctx->sslConn) {
            ReleaseClient(ctx);
            return;
        }

        if(!TryHandshake(ctx, EventType::EVENT_RECV, EventType::EVENT_HANDSHAKE)) {
            Close(ctx);
            return;
        }
    }
    // Plain HTTP
    else
        EnterState(ctx, EventType::EVENT_RECV);

    if(!RegisterEpoll(ctx, EPOLL_CTL_ADD)) {
        Close(ctx);
        return;
    }

    metrics_->network.accepts++;

    // Set an initial timeout for the new connection so they don't connect
    // and stay idle forever.
    RefreshExpiry(ctx, config_.networkConfig.idleTimeout);
}

EndpointStatus EpollConnectionHandler::WrapConnect(EndpointCtx* ctx, EndpointEntry& entry)
{
    WFX_TRACE();

    // Caller is responsible for error handling, this function never calls Close()
    auto& meta = entry.meta;
    auto& desc = meta.desc;

    // Whether the slot speaks TLS belongs to the connection, not to the slot. An in-band upgrade
    // (SlotUpgradeTls) marks the slot secure, and without this reset that mark outlives its
    // connection: the next connect would wrap straight away and send a ClientHello to a server
    // still waiting for the protocol's plaintext probe.
    const bool useTls = EndpointUsesTls(meta.config, meta.port);
    ctx->SetEndpointState(useTls ? EndpointState::ENDPOINT_SECURE : EndpointState::ENDPOINT_INSECURE);

    const EndpointStatus ccResult = CreateAndConnect(ctx, meta);
    if(ccResult != EndpointStatus::PENDING)
        return ccResult;

    // Immediate connect path (connect() returned 0 synchronously, no EINPROGRESS)
    // For the plain TCP case eventType is still EVENT_ACCEPT (the slot default). We must
    // set it to the correct state now so that when RegisterEpoll(MOD) fires EPOLLOUT,
    // HandleWriteReady dispatches correctly:
    //   - onConnect exists, or this is a side connection: set EVENT_ENDPOINT_ONCONNECT so
    //     HandleWriteReady calls FireOnConnect / CompleteAuxConnect
    //   - neither: set EVENT_ENDPOINT_SEND so HandleWriteReady calls Write()
    // The SSL case is excluded here because TryHandshake below sets eventType itself
    const bool onConnectPath = ctx->isSideConnection || desc.onConnect;
    const bool immediateConnect = (ctx->eventType != EventType::EVENT_CONNECT);

    if(immediateConnect) {
        ctx->SetConnectionState(ConnectionState::CONNECTION_ALIVE);

        // Immediate SSL connect (no EINPROGRESS). Attempt handshake now
        if(ctx->GetEndpointState() == EndpointState::ENDPOINT_SECURE) {
            ctx->sslConn = sslHandler_->WrapClient(ctx->socket, meta.hostname.c_str(),
                                                   std::string_view{meta.config.alpnProtocols.data,
                                                                    meta.config.alpnProtocols.length},
                                                   &meta.cachedTlsSession);
            if(!ctx->sslConn)
                return EndpointStatus::SSL_FAILURE;

            const EventType onSuccess =
                onConnectPath ? EventType::EVENT_ENDPOINT_ONCONNECT : EventType::EVENT_ENDPOINT_SEND;

            if(!TryHandshake(ctx, onSuccess, EventType::EVENT_ENDPOINT_HANDSHAKE))
                return EndpointStatus::SSL_FAILURE;
        }
        else
            EnterState(ctx, onConnectPath ? EventType::EVENT_ENDPOINT_ONCONNECT : EventType::EVENT_ENDPOINT_SEND);
    }

    // else: EVENT_CONNECT, EPOLLOUT fires naturally when OS completes the TCP handshake
    // Everything else: MOD re-evaluates fd state in ET mode so the edge fires immediately

    if(!RegisterEpoll(ctx, EPOLL_CTL_ADD)) {
        logger_.Error("[Epoll]: 'RegisterEpoll(ADD)' failed for endpoint '", meta.hostname, "': ", strerror(errno));
        return EndpointStatus::EPOLL_ERROR;
    }

    if(immediateConnect && !RegisterEpoll(ctx, EPOLL_CTL_MOD)) {
        logger_.Error("[Epoll]: 'RegisterEpoll(MOD)' failed for endpoint '", meta.hostname, "': ", strerror(errno));
        return EndpointStatus::EPOLL_ERROR;
    }

    // Four possible states after the connect/handshake attempt above:
    //   - still mid-TCP-connect or mid-TLS-handshake (async, EPOLLOUT pending)
    //     -> arm connectTimeoutSeconds, the in-progress connection must finish in time
    //   - a side connection, connected (and handshake done, if any)
    //     -> arm connectTimeoutSeconds, waiting for CompleteAuxConnect to hand it to the caller
    //   - connected (and handshake done, if any) AND a client is waiting
    //     -> arm requestTimeoutSeconds, onConnect/Write is about to run for that client
    //   - connected with no client waiting (prewarm, no onConnect hook)
    //     -> nothing to do or send, park the slot straight in the idle pool
    //
    // A waiting client shows up as clientCtx (single-slot) OR a non-empty pendingStreams
    // (multiplexed, see SendPayloadMultiplexed). Missing the second case here returns a
    // slot to the free pool while it's still mid-handshake with a real stream pending,
    // letting a concurrent SendPayload lease the same slot out from under it.
    const bool hasWaitingClient = ctx->clientCtx || (ctx->pendingStreams && !ctx->pendingStreams->empty());

    if(ctx->eventType == EventType::EVENT_CONNECT || ctx->eventType == EventType::EVENT_ENDPOINT_HANDSHAKE ||
       ctx->isSideConnection)
        RefreshExpiry(ctx, meta.config.connectTimeoutSeconds);
    else if(hasWaitingClient)
        RefreshExpiry(ctx, meta.config.requestTimeoutSeconds);
    else
        ReturnEndpointToPool(ctx);

    return EndpointStatus::PENDING;
}

void EpollConnectionHandler::CompleteAuxConnect(EndpointCtx* auxCtx)
{
    // Reused from the onConnect flow: marks "handed off once", so a later in-band SlotUpgradeTls
    // on this same connection resumes its own caller instead of re-running this handoff.
    auxCtx->inOnConnectPhase = 1;

    // Re-arm past the connect phase: this is now the safety net for the caller's whole usage
    // window (Send/Receive/Close), not just the connect itself. If the caller forgets to Close(),
    // this fires FailAuxConnect on the timer-wheel path below instead of leaking the connection.
    auto& entry = endpoints_[auxCtx->endpointIdx];
    RefreshExpiry(auxCtx, entry.meta.config.connectTimeoutSeconds);

    HandleEndpointAsyncCallback(auxCtx, {auxCtx, 0, {.unused = 0}, AsyncStatus::COMPLETED}, false);
}

void EpollConnectionHandler::FailAuxConnect(EndpointCtx* auxCtx, SlotStatus status)
{
    const AsyncResult result{nullptr, 0, {.slotStatus = status}, AsyncStatus::IO_FAILURE};

    HandleEndpointAsyncCallback(auxCtx, result, false);
    CloseSideConnection(auxCtx);
}

ssize_t EpollConnectionHandler::WrapRead(WFXSocket socket, void* sslConn, char* buf, std::size_t len)
{
    if(!sslConn) {
        const ssize_t n = ::recv(socket, buf, len, 0);

        const bool ok = (n > 0);
        metrics_->network.reads += ok;
        metrics_->network.bytesRead += ok ? static_cast<std::uint64_t>(n) : 0;

        return n;
    }

    const SSLResult result = sslHandler_->Read(sslConn, buf, static_cast<int>(len));

    switch(result.error) {
        case SSLReturn::SUCCESS:
            metrics_->network.reads++;
            metrics_->network.bytesRead += static_cast<std::uint64_t>(result.res);
            return result.res;
        case SSLReturn::WANT_READ:
        case SSLReturn::WANT_WRITE:
            errno = EAGAIN;
            return -1;
        case SSLReturn::CLOSED:
            return 0;
        case SSLReturn::SYSCALL:
            return -1;
        case SSLReturn::FATAL:
        default:
            errno = EIO;
            return -1;
    }
}

ssize_t EpollConnectionHandler::WrapWrite(WFXSocket socket, void* sslConn, const char* buf, std::size_t len)
{
    if(!sslConn) {
        const ssize_t n = ::send(socket, buf, len, MSG_NOSIGNAL);

        const bool ok = (n > 0);
        metrics_->network.writes += ok;
        metrics_->network.bytesWritten += ok ? static_cast<std::uint64_t>(n) : 0;

        return n;
    }

    const SSLResult result = sslHandler_->Write(sslConn, buf, static_cast<int>(len));

    switch(result.error) {
        case SSLReturn::SUCCESS:
            metrics_->network.writes++;
            metrics_->network.bytesWritten += static_cast<std::uint64_t>(result.res);
            return result.res;
        case SSLReturn::WANT_READ:
        case SSLReturn::WANT_WRITE:
            errno = EAGAIN;
            return -1;
        case SSLReturn::CLOSED:
            return 0;
        case SSLReturn::SYSCALL:
            return -1;
        case SSLReturn::FATAL:
        default:
            errno = EIO;
            return -1;
    }
}

ssize_t EpollConnectionHandler::WrapFile(ClientCtx* ctx, int fd, off_t* offset, std::size_t count)
{
    if(!ctx->sslConn) {
        const ssize_t n = ::sendfile(ctx->socket, fd, offset, count);

        const bool ok = (n > 0);
        metrics_->network.fileCalls += ok;
        metrics_->network.fileBytesWritten += ok ? static_cast<std::uint64_t>(n) : 0;

        return n;
    }

    const SSLResult result = sslHandler_->WriteFile(ctx->sslConn, fd, offset ? *offset : 0, count);

    switch(result.error) {
        // Switch to streaming mode with Write instead
        // Stream will uses a non chunked mode of transferring files (cuz we already sent the header)
        // And we have access to FileInfo struct anyways (its guaranteed initialized so yeah)
        case SSLReturn::NO_IMPL: {
            metrics_->network.fileFallbacks++;

            ctx->isFileOperation = 0;
            ctx->isStreamOperation = 1;
            ctx->streamChunked = 0;
            ctx->streamGenerator = {&ctx->fileInfo,
                                    [](void* c, StreamBuffer buffer) -> StreamResult {
                                        auto* fi = static_cast<FileInfo*>(c);
                                        const ssize_t r = pread(fi->fd, buffer.buffer, buffer.size, fi->offset);

                                        // Error or EOF
                                        if(r <= 0) {
                                            return {0, r == 0 ? StreamAction::STOP_AND_ALIVE_CONN
                                                              : StreamAction::STOP_AND_CLOSE_CONN};
                                        }

                                        // Success
                                        fi->offset += r;
                                        return {static_cast<std::size_t>(r), StreamAction::CONTINUE};
                                    },
                                    nullptr};

            // Signal to caller that streaming mode is engaged
            return SWITCH_FILE_TO_STREAM;
        }
        case SSLReturn::SUCCESS:
            metrics_->network.fileCalls++;
            metrics_->network.fileBytesWritten += static_cast<std::uint64_t>(result.res);
            if(offset)
                *offset += result.res;
            return result.res;
        case SSLReturn::WANT_READ:
        case SSLReturn::WANT_WRITE:
            errno = EAGAIN;
            return -1;
        case SSLReturn::CLOSED:
            return 0;
        case SSLReturn::SYSCALL:
            return -1;
        case SSLReturn::FATAL:
        default:
            errno = EIO;
            return -1;
    }
}

} // namespace WFX::OSSpecific