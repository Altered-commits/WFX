// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_LINUX_USE_IO_URING

#include "epoll_connection.hpp"

#include "http/common/http_error_msgs.hpp"
#include "http/ssl/http_ssl_factory.hpp"
#include "shared/apis/http_api.hpp"
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
    // Retries a syscall-style operation while it fails with EINTR. 'fn' should-
    // -perform exactly one attempt of the underlying syscall and return its raw-
    // -result (whatever a successful call returns, typically >= 0). On any error-
    // -other than EINTR, returns immediately with that result so the caller can-
    // -inspect errno itself
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

template <typename Ctx> bool EpollConnectionHandler::RegisterEpoll(Ctx* ctx, int op)
{
    // Poll once, then we just won't touch 'epoll_ctl' again till we close connection
    // We will use 'ctx->eventType' to control the flow of data pretty much, preventing-
    // -any sort of race condition and such
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

    constexpr EventType recvState =
        std::is_same_v<Ctx, ClientCtx> ? EventType::EVENT_RECV : EventType::EVENT_ENDPOINT_RECV;

    // Drain loop (ET mode: must read until EAGAIN)
    while(true) {
        ValidRegion region = rwBuffer.GetWritableReadRegion();
        if(!region.ptr || region.len == 0) {
            if(!rwBuffer.GrowReadBuffer(config_.networkConfig.readBufferIncSize,
                                        config_.networkConfig.maxReadBufferSize)) {
                logger_.Warn("[Epoll]: Read buffer full, closing connection");
                Close(ctx);
                return false;
            }

            region = rwBuffer.GetWritableReadRegion();
        }

        ssize_t n = WrapRead(ctx->socket, ctx->sslConn, region.ptr, region.len);

        // Fully handle SSL + TCP edge-triggered
        if(n > 0) {
            rwBuffer.AdvanceReadLength(n);
            gotData = true;
        }

        // Connection closed by peer
        else if(n == 0) {
            // If the caller can finalize on EOF (the request RECV path), don't tear the slot-
            // -down here. Report EOF and let it run one last isEof parse so a close-delimited-
            // -body (no Content-Length, no chunked) can be delivered before teardown
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
                // Endpoint and client use different receive states so HandleWriteReady-
                // -and HandleEpollIn can route without an IsEndpoint() check
                ctx->eventType = recvState;
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

    // Wait for the event loop to complete the shutdown. Endpoint and client shutdowns-
    // -are distinct so the event loop can route them without an 'IsEndpoint()' check
    if constexpr(std::is_same_v<Ctx, ClientCtx>)
        ctx->eventType = EventType::EVENT_SHUTDOWN;
    else
        ctx->eventType = EventType::EVENT_ENDPOINT_SHUTDOWN;

    return false;
}
// ^^^ Shared ClientCtx/EndpointCtx templates ^^^

// Used by 'OnSlotConnected' to call back into the engine without a capture
EpollConnectionHandler* EpollConnectionHandler::instance_ = nullptr;

// vvv Constructor & Destructor vvv
EpollConnectionHandler::EpollConnectionHandler(bool useHttps) : useHttps_(useHttps)
{
    instance_ = this;

    // Decorrelate backoff jitter across worker processes. Without per-process entropy every worker-
    // -would share the same xorshift sequence and reconnect in lockstep, re-creating the thundering-
    // -herd the jitter exists to prevent. Mix pid + a clock + this; OR-in 1 so the state is never 0-
    // -(0 is the xorshift fixed point)
    std::uint64_t seed = static_cast<std::uint64_t>(::getpid());
    seed ^= static_cast<std::uint64_t>(SteadyClock::now().time_since_epoch().count()) * 0x9E3779B97F4A7C15ULL;
    seed ^= reinterpret_cast<std::uintptr_t>(this);
    reconnectRngState_ = seed | 1ULL;

    if(useHttps)
        sslHandler_ = CreateSSLHandler();
}

EpollConnectionHandler::~EpollConnectionHandler()
{
    // Free all endpoint slot allocations before fds close and before BufferPool-
    // -potentially destructs ahead of us. void* fields have no destructors so we-
    // -must call the user-supplied hooks explicitly
    for(auto& entry : endpoints_) {
        auto& desc = entry.meta.desc;
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

    // Free all client slot read/write buffers
    for(std::uint32_t i = 0; i < connections_.GetSlots(); i++)
        connections_.GetPtr(i)->rwBuffer.ResetBuffer();

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

    // If we got an AF_INET6 socket, we must explicitly disable IPV6_V6ONLY to allow-
    // -it to accept connections from both IPv4 and IPv6 clients.
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

    if(listen(listenFd_, osConfig.backlog) < 0)
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
    timerWheel_.Init(connections_.GetSlots(), 4096, 1, TimeUnit::SECONDS,
                     [this](std::uint32_t connId, std::uint32_t extra) {
                         // 'extra' contains a 16-bit value:
                         //   >= CLIENT_CONNECTION_TAG -> client connection
                         //   <  CLIENT_CONNECTION_TAG -> endpoint connection (index)
                         if(extra >= CLIENT_CONNECTION_TAG) {
                             ClientCtx* ctx = connections_.GetPtr(connId);

                             // So the logic behind the if condition is, in normal sync path, if a connection is marked-
                             // -'close', it will trigger cleanup after it sent data so no need to clash with it
                             // But on the other hand, in the async / endpoint path, if a connections is marked 'close'
                             // and- -the callback, for some odd reason, just hung up and isn't responding, we shouldn't
                             // care- -about connection atp. WE CLOSE IT OURSELVES
                             if(ctx->GetConnectionState() != ConnectionState::CONNECTION_CLOSE ||
                                ctx->IsAsyncOperation())
                                 Close(ctx, true);
                         }
                         else {
                             // connId is an absolute timer wheel index, recover pool index by-
                             // -subtracting timerBase
                             auto& entry = endpoints_[extra];
                             std::uint32_t slotIdx = connId - entry.meta.timerBase;
                             EndpointCtx* ctx = entry.pool.GetPtr(slotIdx);

                             // A timeout during the connect phase (TCP connect / TLS handshake /-
                             // -onConnect) is a transient connect failure: route it through the funnel-
                             // -so a background slot reconnects with backoff and a client-waiting slot-
                             // -fails fast. A timeout during request/idle just closes as before
                             EventType et = ctx->eventType;
                             bool connectPhase = et == EventType::EVENT_CONNECT ||
                                                 et == EventType::EVENT_ENDPOINT_HANDSHAKE ||
                                                 et == EventType::EVENT_ENDPOINT_ONCONNECT;

                             if(connectPhase)
                                 HandleConnectFailure(ctx, entry, false, DisconnectReason::HANDSHAKE_TIMEOUT);
                             else
                                 Close(ctx, true, DisconnectReason::TIMEOUT);
                         }
                     });

    // Re-expand for any endpoints registered before Initialize was called
    for(auto& entry : endpoints_)
        timerWheel_.Expand(entry.pool.GetSlots());

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

void EpollConnectionHandler::SetEngineCallback(ReceiveCallback onData)
{
    onReceive_ = std::move(onData);
}

std::uint16_t EpollConnectionHandler::AllocateEndpoint(const char* host, EndpointDesc desc, EndpointConfig config)
{
    WFX_TRACE();

    if(endpoints_.size() >= MAX_DISTINCT_ENDPOINTS)
        logger_.Fatal("[Epoll]: Too many distinct domain endpoints registered");

    ValidateEndpoint(host, desc, config);

    // Scheme prefixes are not allowed. Use FORCE_REQUIRE or FORCE_INSECURE for-
    // -non-standard ports, or rely on port heuristics with AUTO
    std::string_view hostView{host};

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

    std::uint16_t endpointIdx = static_cast<std::uint16_t>(endpoints_.size());
    std::uint32_t timerBase = connections_.GetSlots();

    // 'timerBase' used in RefreshExpiry(EndpointCtx*) overload, do check it out for info
    if(!endpoints_.empty()) {
        auto& last = endpoints_.back();
        timerBase = last.meta.timerBase + last.pool.GetSlots();
    }

    auto& entry = endpoints_.emplace_back(config.connLimit);
    auto& meta = entry.meta;
    auto& pool = entry.pool;

    meta.timerBase = timerBase;
    meta.desc = desc;
    meta.config = config;
    meta.hostname = std::move(hostname);
    meta.port = port;

    bool useTLS = false;
    switch(config.tlsConfig) {
        case EndpointTLSConfig::FORCE_REQUIRE:
            useTLS = true;
            break;
        case EndpointTLSConfig::FORCE_INSECURE:
            useTLS = false;
            break;
        case EndpointTLSConfig::AUTO:
        default:
            useTLS = ResolveTLSFromAuto(port);
            break;
    }

    // Resolve AFTER hostname/port are set on meta
    std::uint32_t minTtl = 0;
    if(!DNSResolver::Resolve(meta.hostname.c_str(), meta.port, meta.addrs, minTtl))
        logger_.Fatal("[Epoll]: Failed to resolve endpoint: ", host);

    meta.dnsNextRefreshSeconds = ComputeNextDnsRefresh(minTtl, config.dnsRefreshSeconds, meta.hostname);

    for(std::uint32_t i = 0; i < pool.GetSlots(); i++) {
        EndpointCtx* ctx = pool.GetPtr(i);
        ctx->endpointIdx = endpointIdx;
        ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
        ctx->SetEndpointState(useTLS ? EndpointState::ENDPOINT_SECURE : EndpointState::ENDPOINT_INSECURE);
    }

    logger_.Info("[Epoll]: Endpoint allocated -- host='", meta.hostname, "' port=", meta.port,
                 " endpointIdx=", endpointIdx, " tls=", (useTLS ? "yes" : "no"), " connLimit=", config.connLimit,
                 " prewarm=", config.prewarm, " addrs=", meta.addrs.size(),
                 " nextDnsRefreshInSeconds=", (meta.dnsNextRefreshSeconds - NowMs() / 1000));

    return endpointIdx;
}

// vvv Core I/O Operations vvv
void EpollConnectionHandler::ResumeReceive(ClientCtx* ctx)
{
    if(!EnsureReadReady(ctx))
        return;

    // We are ready to receive data now, set 'eventType' to EVENT_RECV
    ctx->eventType = EventType::EVENT_RECV;
}

void EpollConnectionHandler::Write(ClientCtx* ctx, std::string_view msg)
{
    WFX_TRACE();

    // Case 1: Direct send (used only for static error codes)
    // NOTE: CHANGE OF PLANS, msg is fire and forget, i don't care if they get delivered-
    // -or not, if u want good error messages u will go the hard route anyways (res.Status().SendText()...)
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

        while(writeMeta->writtenLength < writeMeta->dataLength) {
            const char* buf = ctx->rwBuffer.GetWriteData() + writeMeta->writtenLength;
            std::size_t remaining = writeMeta->dataLength - writeMeta->writtenLength;

            ssize_t n = WrapWrite(ctx->socket, ctx->sslConn, buf, remaining);

            if(n > 0)
                writeMeta->writtenLength += n;

            // Partial progress, wait for event loop to notify when we can send more data
            else if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                ctx->eventType = EventType::EVENT_SEND;
                return;
            }

            // Connection closed / Fatal error
            else
                goto __CloseConnection;
        }
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

    if(ctx->GetConnectionState() == ConnectionState::CONNECTION_CLOSE) {
    __CloseConnection:
        Close(ctx);
    }
    else {
        ctx->Clear();
        ResumeReceive(ctx);
    }
}

void EpollConnectionHandler::WriteFile(ClientCtx* ctx, std::string path)
{
    // Before we proceed, ensure stuffs ready for file operation
    if(!EnsureFileReady(ctx, std::move(path))) {
        ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
        Write(ctx, HttpError::internalError);
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
    if(!generator.ctx || !generator.Next) {
        logger_.Warn("[Epoll]: 'Stream()' called but received empty generator");
        Close(ctx);
        return;
    }

    // Store the generator function in context for future use
    ctx->streamGenerator = generator;

    // For streaming operations, we first want to finish writing out headers-
    // -and mark it as stream operation, so when 'Write' completes, it should-
    // -start the streaming process
    ctx->isStreamOperation = 1;
    ctx->streamChunked = streamChunked;
    Write(ctx, {});
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
EndpointStatus EpollConnectionHandler::SerializeSingleSlot(EndpointCtx* slotCtx, EndpointMetadata& meta,
                                                           const void* req)
{
    // Serializes req into the FULL write buffer (non-multiplexed slots only ever hold one-
    // -request at a time, so clearing first is safe), growing it as needed. Caller sets up-
    // -the clientCtx link and dispatches to WrapConnect / RegisterEpoll on success
    auto& desc = meta.desc;
    auto& rwBuf = slotCtx->rwBuffer;

    rwBuf.ClearWriteBuffer();

    while(true) {
        auto* writeMeta = rwBuf.GetWriteMeta();
        std::uint32_t written = 0;
        std::uint64_t unusedStreamKey = 0; // desc.hasCapacity is null on this path, always ignored

        SerializeResult sr = desc.serialize(slotCtx->slotState, req, rwBuf.GetWriteData(), writeMeta->bufferSize,
                                            &written, &unusedStreamKey);

        if(sr == SerializeResult::OK) {
            writeMeta->dataLength = written;
            writeMeta->writtenLength = 0;
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
    // Serializes req into the TAIL of the write buffer (never clears it: other streams on this-
    // -multiplexed slot may still have bytes queued between writtenLength and dataLength that-
    // -must survive). Only advances dataLength once a valid streamKey comes back, so a rejected-
    // -serialize never corrupts bytes another in-flight stream is relying on. Caller still owns-
    // -registering the result in pendingStreams / coalescePending
    auto& desc = meta.desc;
    auto& rwBuf = slotCtx->rwBuffer;

    *streamKey = 0;

    while(true) {
        auto region = rwBuf.GetWritableWriteRegion();
        std::uint32_t written = 0;

        SerializeResult sr = desc.serialize(slotCtx->slotState, req, region.ptr, static_cast<std::uint32_t>(region.len),
                                            &written, streamKey);

        if(sr == SerializeResult::OK) {
            if(*streamKey == 0) {
                // Protocol contract violation: hasCapacity is set, so serialize() must always-
                // -assign a real key. Bail before touching dataLength so the bytes just written-
                // -are simply overwritten by the next attempt instead of corrupting the shared-
                // -connection's framing
                logger_.Error("[Epoll]: multiplexed 'serialize' returned OK with streamKey=0 for endpoint ",
                              meta.hostname);
                return EndpointStatus::SERIALIZE_ERROR;
            }

            rwBuf.GetWriteMeta()->dataLength += written;
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
                                                   AsyncData asyncData)
{
    if(endpointIdx >= endpoints_.size())
        return EndpointStatus::INVALID_KEY;

    auto& entry = endpoints_[endpointIdx];
    auto& meta = entry.meta;
    auto& desc = meta.desc;

    std::uint64_t pendingCoalesceKey = 0;

    // If an identical in-flight request exists, park the client as a waiter
    // Key is computed once here and reused below when registering the primary
    // Do NOT set clientCtx->endpointCtx for waiters (ReleaseClient would otherwise-
    // -kill the in-flight slot when a waiter disconnects)
    if(desc.coalesceKey) {
        pendingCoalesceKey = desc.coalesceKey(req);
        if(pendingCoalesceKey != 0) {
            auto it = meta.coalescePending.find(pendingCoalesceKey);
            if(it != meta.coalescePending.end()) {
                it->second.waiters.push_back({clientCtx, clientCtx->generationId});
                clientCtx->asyncData = asyncData;
                return EndpointStatus::PENDING;
            }
        }
    }

    if(desc.hasCapacity)
        return SendPayloadMultiplexed(clientCtx, endpointIdx, req, asyncData, entry, pendingCoalesceKey);

    EndpointCtx* slotCtx = GetEndpointConnection(endpointIdx);
    if(!slotCtx)
        return EndpointStatus::POOL_EXHAUSTED;

    // Per-slot state survives across requests. Only create if not already present
    if(!slotCtx->slotState && desc.createSlotState)
        slotCtx->slotState = desc.createSlotState(desc.userCtx);

    // Per-request objects are fresh every time
    if(!slotCtx->parseStateObj && desc.createParseState)
        slotCtx->parseStateObj = desc.createParseState(slotCtx->slotState);

    if(!slotCtx->outputObj && desc.createOutput)
        slotCtx->outputObj = desc.createOutput(slotCtx->slotState);

    // A fresh connect with an onConnect hook must run the handshake before anything else-
    // -touches the write buffer. Serializing the request now and handing it to WrapConnect-
    // -would let FireOnConnect's own Send() append the handshake bytes AFTER the request-
    // -bytes already queued here, so the request would reach the wire before the handshake
    // Defer: stash req and let FlushDeferredRequest serialize it once onConnect is READY
    bool freshConnect = slotCtx->GetConnectionState() == ConnectionState::CONNECTION_CLOSE;

    if(desc.onConnect && freshConnect) {
        slotCtx->pendingConnectReq = req;
        slotCtx->coalesceKey = pendingCoalesceKey; // registered for real once FlushDeferredRequest serializes it
    }
    else {
        auto& rwBuf = slotCtx->rwBuffer;
        if(!rwBuf.IsWriteInitialized() && !rwBuf.InitWriteBuffer(config_.networkConfig.maxSendBufferSize)) {
            FinalizeEndpointRequest(slotCtx, meta, false);
            ReleaseEndpoint(slotCtx);
            return EndpointStatus::BUFFER_ERROR;
        }

        EndpointStatus sr = SerializeSingleSlot(slotCtx, meta, req);
        if(sr != EndpointStatus::SUCCESS) {
            FinalizeEndpointRequest(slotCtx, meta, false);
            ReleaseEndpoint(slotCtx);
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

    EndpointStatus result;

    // Closed slot, start from connecting to endpoint
    if(freshConnect)
        result = WrapConnect(slotCtx, entry);

    // Re-use existing connection
    else {
        slotCtx->eventType = EventType::EVENT_ENDPOINT_SEND;

        // Switch from idle timeout to request timeout for the duration of this request
        RefreshExpiry(slotCtx, meta.config.requestTimeoutSeconds);

        result = RegisterEpoll(slotCtx, EPOLL_CTL_MOD) ? EndpointStatus::PENDING : EndpointStatus::EPOLL_ERROR;
        if(result == EndpointStatus::EPOLL_ERROR)
            logger_.Error("[Epoll]: 'SendPayload -> RegisterEpoll(MOD)' failed for endpoint ", meta.hostname, ": ",
                          strerror(errno));
    }

    // N U L L; so Close() -> ReleaseEndpoint() doesn't ALSO try to break the bad news to this client
    if(result != EndpointStatus::PENDING) {
        slotCtx->clientCtx = nullptr;
        clientCtx->endpointCtx = nullptr;
        FinalizeEndpointRequest(slotCtx, meta, false);
        Close(slotCtx, true);
    }

    return result;
}

EndpointStatus EpollConnectionHandler::SendPayloadMultiplexed(ClientCtx* clientCtx, std::uint16_t endpointIdx,
                                                              const void* req, AsyncData asyncData,
                                                              EndpointEntry& entry, std::uint64_t pendingCoalesceKey)
{
    auto& meta = entry.meta;
    auto& desc = meta.desc;

    // Prefer an already-open slot with spare capacity over opening a new connection: keeping-
    // -the connection count low is sort of the entire point of multiplexing
    EndpointCtx* slotCtx = FindMultiplexableSlot(endpointIdx, meta);
    bool freshSlot = false;

    if(!slotCtx) {
        slotCtx = GetEndpointConnection(endpointIdx);
        if(!slotCtx)
            return EndpointStatus::POOL_EXHAUSTED;

        freshSlot = true;
    }

    // Per-slot (connection-level) state survives across requests, same as the single-slot path
    if(!slotCtx->slotState && desc.createSlotState)
        slotCtx->slotState = desc.createSlotState(desc.userCtx);

    // Per-request parse scratch belongs to THIS request alone, not the slot: a busy slot may-
    // -have several of these alive at once, tracked in slotCtx->pendingStreams. Output is NOT-
    // -created here: the protocol owns per-stream output internally (keyed by the streamKey it-
    // -assigns below) and only hands it to the engine via takeStreamOutput once finished
    void* reqParseState = desc.createParseState ? desc.createParseState(slotCtx->slotState) : nullptr;
    std::uint64_t streamKey = 0; // assigned by serialize(); stays 0 when the request is deferred below

    auto cleanupReqParseState = [&]() {
        if(reqParseState && desc.destroyParseState)
            desc.destroyParseState(reqParseState);
    };

    // Only reachable when freshSlot is true: FindMultiplexableSlot only ever returns-
    // -already-connected slots. A fresh connect with an onConnect hook must run the-
    // -handshake before the request touches the write buffer (see SendPayload for why),
    // -so defer serialize() to FlushDeferredRequest once onConnect is READY. slotState-
    // -fields that would normally only matter on the single-slot path (clientCtx,
    // -coalesceKey, parseStateObj) are unused here otherwise, so they double as storage-
    // -for this stream's pending bits until FlushDeferredRequest moves them into a real-
    // -PendingStream entry
    bool freshConnect = slotCtx->GetConnectionState() == ConnectionState::CONNECTION_CLOSE;

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

        EndpointStatus sr = SerializeMultiplexed(slotCtx, meta, req, &streamKey);
        if(sr != EndpointStatus::SUCCESS) {
            cleanupReqParseState();
            if(freshSlot)
                ReleaseEndpoint(slotCtx);

            return sr;
        }

        // Register in coalesce map after successful serialize (master). Per-stream, not per-slot:-
        // -PendingStream::coalesceKey, not EndpointCtx::coalesceKey, since several concurrently-
        // -in-flight streams on this one slot may each be coalescing under a different key
        if(pendingCoalesceKey != 0) {
            auto& ce = meta.coalescePending[pendingCoalesceKey];
            ce.inflight = slotCtx;
        }

        // TODO: This and the parser state, we gotta start using our pool to allocate
        if(!slotCtx->pendingStreams)
            slotCtx->pendingStreams = new PendingStreamMap();

        (*slotCtx->pendingStreams)[streamKey] =
            PendingStream{clientCtx, reqParseState, pendingCoalesceKey, clientCtx->generationId};

        clientCtx->streamKey = streamKey;
    }

    clientCtx->endpointCtx = slotCtx;
    clientCtx->asyncData = asyncData;

    EndpointStatus result;

    // Closed slot, start from connecting to endpoint. Only reachable when freshSlot is true:-
    // -FindMultiplexableSlot only ever returns already-connected slots
    if(freshConnect)
        result = WrapConnect(slotCtx, entry);

    // Re-use existing connection
    else {
        slotCtx->eventType = EventType::EVENT_ENDPOINT_SEND;
        RefreshExpiry(slotCtx, meta.config.requestTimeoutSeconds);

        result = RegisterEpoll(slotCtx, EPOLL_CTL_MOD) ? EndpointStatus::PENDING : EndpointStatus::EPOLL_ERROR;
        if(result == EndpointStatus::EPOLL_ERROR)
            logger_.Error("[Epoll]: 'SendPayloadMultiplexed -> RegisterEpoll(MOD)' failed for endpoint ", endpointIdx,
                          ": ", strerror(errno));
    }

    if(result != EndpointStatus::PENDING) {
        clientCtx->endpointCtx = nullptr;
        clientCtx->streamKey = 0;

        if(pendingCoalesceKey != 0)
            meta.coalescePending.erase(pendingCoalesceKey);

        // Deferred case: WrapConnect failed before serialize() ever ran, so the protocol never-
        // -saw this request at all. Nothing was registered in pendingStreams, just drop what-
        // -was stashed on the slot and tear it down (freshSlot is always true here)
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

            // Tell the protocol the engine is abandoning this stream so it can drop whatever-
            // -internal tracking it started at serialize() time. Matters most for the shared-slot-
            // -case below, where the slot (and the protocol's connection-level state) lives on
            void* abandoned = desc.takeStreamOutput(slotCtx->slotState, streamKey);
            if(abandoned && desc.destroyOutput)
                desc.destroyOutput(abandoned);

            // A brand new slot that never even finished connecting has nothing else relying on-
            // -it, tear it down entirely. A shared slot that merely failed to re-arm epoll for-
            // -THIS request stays alive for every other stream still in flight on it
            if(freshSlot)
                Close(slotCtx, true);
        }
    }

    return result;
}

void EpollConnectionHandler::SlotSend(EndpointCtx* slotCtx, const void* data, std::uint32_t size, AsyncData asyncData)
{
    auto& rwBuf = slotCtx->rwBuffer;

    auto fireFailure = [&]() {
        AsyncResult fail{nullptr, 0, {.unused = 0}, AsyncStatus::IO_FAILURE};
        if(asyncData.AsyncComplete)
            asyncData.AsyncComplete(asyncData.userData, fail);
    };

    if(!rwBuf.IsWriteInitialized() && !rwBuf.InitWriteBuffer(config_.networkConfig.maxSendBufferSize)) {
        fireFailure();
        return;
    }

    if(!rwBuf.AppendWriteData(static_cast<const char*>(data), size, config_.networkConfig.sendBufferIncSize,
                              config_.networkConfig.maxSendBufferSize)) {
        fireFailure();
        return;
    }

    // 'asyncData' holds the SlotSend completion. 'HandleEndpointWriteComplete' fires it
    slotCtx->asyncData = asyncData;
    slotCtx->eventType = EventType::EVENT_ENDPOINT_SEND;

    // Fail the operation immediately so that the user's onConnect coroutine-
    // -gets a definite answer rather than a slow hang
    if(!RegisterEpoll(slotCtx, EPOLL_CTL_MOD)) {
        logger_.Error("[Epoll]: 'SlotSend -> RegisterEpoll(MOD)' failed: ", strerror(errno));
        fireFailure();
    }
}

void EpollConnectionHandler::SlotReceive(EndpointCtx* slotCtx, AsyncData asyncData)
{
    if(!EnsureReadReady(slotCtx))
        return;

    // 'asyncData' holds the SlotReceive completion. 'HandleEpollIn' fires it when data arrives
    slotCtx->asyncData = asyncData;
    slotCtx->eventType = EventType::EVENT_ENDPOINT_ONCONNECT;

    // Re-arm epoll so EPOLLIN fires even if backend data arrived while we were in the-
    // -SlotSend write phase. During that phase eventType was EVENT_ENDPOINT_SEND, so any-
    // -EPOLLIN edge that fired was consumed and ignored by HandleEpollIn's default case
    // Without this MOD call, EPOLLIN would never re-fire in ET mode for that data
    if(!RegisterEpoll(slotCtx, EPOLL_CTL_MOD)) {
        logger_.Error("[Epoll]: 'SlotReceive -> RegisterEpoll(MOD)' failed: ", strerror(errno));

        AsyncResult fail{nullptr, 0, {.unused = 0}, AsyncStatus::IO_FAILURE};
        if(asyncData.AsyncComplete)
            asyncData.AsyncComplete(asyncData.userData, fail);
    }
}

StringView EpollConnectionHandler::NegotiatedProtocol(EndpointCtx* slotCtx)
{
    if(!slotCtx->sslConn)
        return {};

    std::string_view proto = sslHandler_->NegotiatedProtocol(slotCtx->sslConn);
    return StringView{proto.data(), proto.size()};
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
        int nfds = epoll_wait(epollFd_, events_.get(), maxEvents_, -1);
        if(nfds < 0) {
            // Interrupted by signal
            if(errno == EINTR)
                continue;
            break;
        }

        for(std::uint32_t i = 0; i < static_cast<std::uint32_t>(nfds); i++) {
            std::uint32_t ev = events_[i].events;
            std::uint64_t meta = events_[i].data.u64;
            std::uint16_t gen = (meta >> 32) & 0xFFFF; // First half's lower 16 bits

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

                    int clientFd = accept4(listenFd_, (sockaddr*)&addr, &len, SOCK_NONBLOCK);
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

                    // Check limiter first, then try to grab a slot
                    if(!ipLimiter_.AllowConnection(tmpIp)) {
                        close(clientFd);
                        continue;
                    }

                    ClientCtx* ctx = GetClientConnection();
                    if(!ctx) {
                        ipLimiter_.ReleaseConnection(tmpIp);
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
            // Also if you are confused with the below hardcoded numbers, check 'PackEpollData'-
            // -function, you can see how data is packed in event.u64 member field
            std::uint16_t endpointIdx = meta >> 48;
            std::uint32_t poolIdx = meta & 0xFFFFFFFF;

            if(endpointIdx == CLIENT_CONNECTION_TAG) {
                ClientCtx* ctx = connections_.GetPtr(poolIdx);

                // If the slot's current generation doesn't match the event's generation, it means-
                // -this event is for a dead connection
                if(ctx->generationId != gen)
                    continue;

                HandleClientEvent(ctx, ev, gen);
            }
            else {
                EndpointCtx* ctx = endpoints_[endpointIdx].pool.GetPtr(poolIdx);

                // If the slot's current generation doesn't match the event's generation, it means-
                // -this event is for a dead connection
                if(ctx->generationId != gen)
                    continue;

                HandleEndpointEvent(ctx, ev, gen);
            }
        }

        // DNS refresh check, cheap time comparison, runs once per epoll wakeup
        std::uint64_t nowSeconds = NowMs() / 1000;
        for(std::uint16_t i = 0; i < static_cast<std::uint16_t>(endpoints_.size()); i++) {
            auto& m = endpoints_[i].meta;
            if(nowSeconds >= m.dnsNextRefreshSeconds)
                HandleDnsRefresh(i);
        }
    }
}

void EpollConnectionHandler::RefreshExpiry(ClientCtx* ctx, std::uint16_t timeoutSeconds)
{
    std::uint32_t idx = connections_.GetIndex(ctx);
    timerWheel_.Schedule(idx, CLIENT_CONNECTION_TAG, timeoutSeconds);
}

void EpollConnectionHandler::RefreshExpiry(EndpointCtx* ctx, std::uint16_t timeoutSeconds)
{
    // 'timerBase' offsets endpoint indices past all client slots and all preceding endpoint pools-
    // -so client slot N and endpoint slot N never collide in the timer wheel's meta_ array
    auto& entry = endpoints_[ctx->endpointIdx];
    std::uint32_t idx = entry.meta.timerBase + entry.pool.GetIndex(ctx);
    timerWheel_.Schedule(idx, ctx->endpointIdx, timeoutSeconds);
}

bool EpollConnectionHandler::RefreshAsyncTimer(ClientCtx* ctx, std::uint32_t delayMs, AsyncData asyncData)
{
    std::uint32_t idx = connections_.GetIndex(ctx);
    std::uint64_t expire = NowMs() + delayMs;

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

    // If it wraps to 0, bump it to 1 cuz 0 is reserved for identifying fds such as Listen/Timer
    if(ctx->generationId == 0)
        ctx->generationId = 1;

    return ctx;
}

EndpointCtx* EpollConnectionHandler::FindMultiplexableSlot(std::uint16_t endpointIdx, EndpointMetadata& meta)
{
    WFX_TRACE();

    auto& pool = endpoints_[endpointIdx].pool;
    std::uint32_t slots = pool.GetSlots();
    if(slots == 0)
        return nullptr;

    auto tryIdx = [&](std::uint32_t idx) -> EndpointCtx* {
        if(!pool.IsAllocated(idx))
            return nullptr;

        EndpointCtx* ctx = pool.GetPtr(idx);

        // Only a slot fully connected and cycling between requests (not still connecting,-
        // -handshaking, in onConnect, awaiting reconnect backoff, or already being torn down)-
        // -can safely take another request. isShuttingDown / connection-state checks are-
        // -required here specifically because this function (unlike AllocSlot) selects among-
        // -already-leased slots by inspecting live state rather than a free/leased bitmap bit --
        // -a slot mid-teardown in Close()/ReleaseEndpoint() still reads eventType RECV/SEND and-
        // -is still bitmap-allocated for that entire window, so without this check a reentrant-
        // -SendPayload (e.g. from a coroutine resumed by this very teardown's waiter callbacks)-
        // -could attach a brand new client to a slot that's about to be Reset() and recycled for-
        // -an unrelated connection
        bool ready =
            !ctx->isShuttingDown && ctx->GetConnectionState() == ConnectionState::CONNECTION_ALIVE &&
            (ctx->eventType == EventType::EVENT_ENDPOINT_RECV || ctx->eventType == EventType::EVENT_ENDPOINT_SEND);

        return (ready && meta.desc.hasCapacity(ctx->slotState)) ? ctx : nullptr;
    };

    // Fast path: the slot we last multiplexed onto very likely still has room, avoids-
    // -a full scan on the common steady-state case
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

    std::uint32_t idx = connections_.GetIndex(ctx);

    // Cancelling timer in 'Close' kinda sucks cuz during async shutdown-
    // -the client might bail, never finish it, and we just be stuck in-
    // -closing state forever aaand timeout won't do anything cuz... we cancelled it
    // So we close it here instead
    timerWheel_.Cancel(idx);

    if(ctx->isAsyncTimerOperation) {
        if(timerHeap_.Remove(idx))
            UpdateAsyncTimer();
    }

    // Destroy orphaned coroutine frame if connection is dying-
    // -while an async operation is in-flight
    HandleClientAsyncCallback(ctx, {}, true);

    // From clients POV, if the endpoint hasn't been set to nullptr after-
    // -endpoint operations complete, it means client closed before endpoint even-
    // -completed
    if(ctx->endpointCtx) {
        EndpointCtx* epCtx = ctx->endpointCtx;

        // Multiplexed request: drop only this stream, the shared slot and every-
        // -other in-flight stream on it live on untouched
        if(ctx->streamKey != 0 && epCtx->pendingStreams) {
            auto& meta = endpoints_[epCtx->endpointIdx].meta;
            auto& desc = meta.desc;
            auto it = epCtx->pendingStreams->find(ctx->streamKey);
            if(it != epCtx->pendingStreams->end()) {
                // Copy the entry out and erase it BEFORE any callback below. desc.takeStreamOutput-
                // -and the coalesce-waiter callbacks can synchronously reenter SendPayloadMultiplexed-
                // -(e.g. an app-level retry), which may insert into this same pendingStreams map and-
                // -trigger a rehash (using `it` afterward would then be undefined behavior)
                PendingStream stream = it->second;
                epCtx->pendingStreams->erase(it);

                // Same coalesce-waiter failure this key's master death already gets on the-
                // -non-multiplexed path below (Close -> ReleaseEndpoint), just scoped to this-
                // -one stream instead of the whole slot
                if(stream.coalesceKey != 0) {
                    auto cit = meta.coalescePending.find(stream.coalesceKey);
                    if(cit != meta.coalescePending.end()) {
                        std::vector<CoalesceWaiter> waiters = std::move(cit->second.waiters);
                        meta.coalescePending.erase(cit);

                        for(auto& w : waiters) {
                            if(!w.clientCtx || w.clientCtx->generationId != w.generationId)
                                continue;

                            w.clientCtx->endpointCtx = nullptr;

                            AsyncResult failResult{};
                            failResult.data = nullptr;
                            failResult.dataLen = 0;
                            failResult.status = AsyncStatus::IO_FAILURE;
                            failResult.endpointStatus = EndpointStatus::INTERNAL_ERROR;
                            HandleClientAsyncCallback(w.clientCtx, failResult, false);
                        }
                    }
                }

                if(stream.parseState && desc.destroyParseState)
                    desc.destroyParseState(stream.parseState);

                // Abandon the stream in the protocol too so it stops tracking a key nothing-
                // -will ever ask for again. May return an already-finished-but-not-yet-
                // -delivered output (a benign race), which we simply free here
                void* abandoned = desc.takeStreamOutput(epCtx->slotState, ctx->streamKey);
                if(abandoned && desc.destroyOutput)
                    desc.destroyOutput(abandoned);
            }
        }
        // Non-multiplexed: force close the whole endpoint connection as before
        else {
            epCtx->clientCtx = nullptr;
            Close(epCtx, true);
        }

        ctx->endpointCtx = nullptr;
    }

    ipLimiter_.ReleaseConnection(ctx->connInfo);

    if(ctx->socket >= 0)
        close(ctx->socket);

    // Bump before Reset so any saved generationId in a CoalesceWaiter no longer-
    // -matches this slot, preventing a spurious delivery to a freed-but-not-reused slot
    ctx->generationId++;
    if(ctx->generationId == 0)
        ctx->generationId = 1;

    ctx->Reset();
    connections_.FreeSlot(idx);
}

void EpollConnectionHandler::ReleaseEndpoint(EndpointCtx* ctx, DisconnectReason disconnectReason)
{
    WFX_TRACE();

    if(!ctx)
        return;

    // If this slot was idle-pooled (returned via ReturnEndpointToPool and never-
    // -re-leased), activeEndpointConns was already decremented at that point. Decrementing-
    // -again here would undercount. Only decrement for slots that were actively leased
    if(!ctx->isPooledIdle)
        metrics_->network.activeEndpointConns--;

    ctx->isPooledIdle = 0;

    auto& entry = endpoints_[ctx->endpointIdx];
    auto& meta = entry.meta;
    auto& desc = meta.desc;

    std::uint32_t idx = entry.pool.GetIndex(ctx);

    // SAME LOGIC PRETTY MUCH
    std::uint32_t timerIdx = meta.timerBase + idx;
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
            for(auto& w : it->second.waiters) {
                if(!w.clientCtx || w.clientCtx->generationId != w.generationId)
                    continue;

                w.clientCtx->endpointCtx = nullptr;

                AsyncResult failResult{};
                failResult.data = nullptr;
                failResult.dataLen = 0;
                failResult.status = AsyncStatus::IO_FAILURE;
                failResult.endpointStatus = DisconnectReasonToStatus(disconnectReason);

                HandleClientAsyncCallback(w.clientCtx, failResult, false);
            }

            pending.erase(it);
        }
        // coalesceKey = 0 handled by Reset() at end of ReleaseEndpoint
    }

    // If slot died during onConnect phase, destroy the suspended coroutine frame-
    // -to avoid leaking it. clientCtx notification is handled separately below
    if(ctx->inOnConnectPhase)
        HandleEndpointAsyncCallback(ctx, {}, true);

    // Notify suspended client if the slot died mid-request
    // If client context exists, it means that the endpoint operation hasn't finished-
    // -but it somehow closed, in this case, just notify the client (as client is suspended-
    // -due to co_await)
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
    std::uint32_t idx = entry.pool.GetIndex(slotCtx);

    // CRITICAL: cancel any leftover timer schedule first (e.g. a stale request-timeout-
    // -from a previous lease cycle) before arming the fresh idle timeout below
    std::uint32_t timerIdx = entry.meta.timerBase + idx;
    timerWheel_.Cancel(timerIdx);

    entry.pool.FreeSlot(idx);
    metrics_->network.activeEndpointConns--;

    // Slot reached a healthy idle state (fresh connect, successful reconnect, or a completed-
    // -keep-alive request), so clear any accumulated backoff attempts
    slotCtx->reconnectAttempts = 0;

    // Slot is now idle-pooled, open socket, no in-flight request. Arm idle-
    // -timeout so the connection doesn't sit open forever without traffic
    // isPooledIdle marks that activeEndpointConns was already decremented above
    // ReleaseEndpoint checks this to avoid double-decrementing if the idle-
    // -timer actually fires before this slot is re-leased
    slotCtx->isPooledIdle = 1;
    RefreshExpiry(slotCtx, entry.meta.config.idleTimeoutSeconds);
}

//  --- MISC Handlers ---
std::uint64_t EpollConnectionHandler::NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - startTime_).count();
}

bool EpollConnectionHandler::SetNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags < 0)
        return false;

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool EpollConnectionHandler::ResolveTLSFromAuto(std::uint16_t port)
{
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

    // Port binary search
    std::size_t lo = 0, hi = std::size(TLS_PORTS);
    while(lo < hi) {
        std::size_t mid = lo + (hi - lo) / 2;
        if(TLS_PORTS[mid] == port)
            return true;
        if(TLS_PORTS[mid] < port)
            lo = mid + 1;
        else
            hi = mid;
    }

    return false;
}

bool EpollConnectionHandler::EnsureFileReady(ClientCtx* ctx, std::string path)
{
    auto [fd, size] = fileCache_.GetFileDesc(std::move(path));
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
            out.ip.v6 = reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr;
            out.type = AF_INET6;
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
    int fd = fileInfo.fd;

    while(fileInfo.offset < fileInfo.fileSize) {
        ssize_t n = WrapFile(ctx, fd, &fileInfo.offset, fileInfo.fileSize - fileInfo.offset);

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
                ctx->eventType = EventType::EVENT_SEND_FILE;

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
    if(!ctx->streamGenerator.ctx || !ctx->streamGenerator.Next) {
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

    // Loop: one iteration per chunk. Drives send() inline to skip the-
    // -epoll_ctl(MOD) + epoll_wait round trip when the socket stays writable
    // On EAGAIN: yield via EVENT_SEND; EPOLLET re-fires EPOLLOUT when the kernel-
    // -drains the send buffer; Write() finishes the partial chunk then re-enters here
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
        std::size_t chunkCap = writeRegion.len - (chunked ? HDR_RESERVE + 2 : 0);

        auto result = ctx->streamGenerator.Next(ctx->streamGenerator.ctx, {chunkPtr, chunkCap});
        RefreshExpiry(ctx, config_.networkConfig.idleTimeout);

        if(result.action != StreamAction::CONTINUE) {
            ctx->SetConnectionState(result.action == StreamAction::STOP_AND_ALIVE_CONN
                                        ? ConnectionState::CONNECTION_ALIVE
                                        : ConnectionState::CONNECTION_CLOSE);
            break;
        }

        if(result.writtenBytes == 0 || result.writtenBytes > UINT32_MAX) {
            Close(ctx);
            return;
        }

        if(!chunked)
            writeMeta->dataLength = result.writtenBytes;
        else {
            char hdr[HDR_RESERVE + 1];
            int hdrLen = snprintf(hdr, HDR_RESERVE, "%zX\r\n", result.writtenBytes);
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

        const char* base = rwBuffer.GetWriteData();
        while(writeMeta->writtenLength < writeMeta->dataLength) {
            ssize_t n = WrapWrite(ctx->socket, ctx->sslConn, base + writeMeta->writtenLength,
                                  writeMeta->dataLength - writeMeta->writtenLength);
            if(n > 0)
                writeMeta->writtenLength += static_cast<std::uint32_t>(n);
            else if(errno == EAGAIN || errno == EWOULDBLOCK) {
                ctx->eventType = EventType::EVENT_SEND;
                return;
            }
            else {
                Close(ctx);
                return;
            }
        }
        // Chunk fully sent, next iteration
    }

    if(ctx->streamGenerator.Destroy)
        ctx->streamGenerator.Destroy(ctx->streamGenerator.ctx);

    bool wasChunked = chunked; // ctx->streamChunked cleared below

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

void EpollConnectionHandler::AsyncCallbackImpl(void* ctxPtr, AsyncData& async, AsyncResult res, bool destroy)
{
    WFX_TRACE();

    auto& asyncRef = async;

    // Sanity checks, 'userData' in some edge cases maybe null, these shouldn't be
    if(!asyncRef.AsyncComplete && !asyncRef.AsyncDestroy)
        return;

    auto complete = asyncRef.AsyncComplete;
    auto kill = asyncRef.AsyncDestroy;
    auto ud = asyncRef.userData;

    asyncRef.AsyncComplete = nullptr;
    asyncRef.AsyncDestroy = nullptr;
    asyncRef.userData = nullptr;

    // Like all coroutines, this one would also require us to set type-erased 'ctx' at Http API
    Shared::GetHttpAPIExt1()->SetGlobalPtrData(ctxPtr);

    if(destroy) {
        if(kill)
            kill(ud);
    }
    else {
        if(complete)
            complete(ud, res);
    }

    // And at the end, erase it
    Shared::GetHttpAPIExt1()->SetGlobalPtrData(nullptr);
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
    ssize_t n = RetryOnEintr([&] { return read(sfd, &expirations, sizeof(expirations)); });

    if(n < 0) {
        // EAGAIN here would mean epoll reported readiness but the count was already-
        // -drained by the time we got here. Shouldn't normally happen since nothing-
        // -else reads this fd, but harmless if it does: skip this call, the level-
        // -triggered registration means epoll will report it again if truly still ready
        if(errno == EAGAIN || errno == EWOULDBLOCK)
            return;

        // Any other error means the read itself is broken. timeoutTimerFd_ is-
        // -registered level-triggered (no EPOLLET), so its readiness is cleared only-
        // -by a successful read here. If read() can no longer succeed at all, this-
        // -fd's state is now unknown/unrecoverable from inside this function. Worse,-
        // -if the fd somehow stays marked ready without being drained, epoll_wait-
        // -would return immediately on every iteration from here on, busy-spinning-
        // -the entire event loop at 100% CPU. Fail loudly rather than risk that
        logger_.Fatal("[Epoll]: Timeout timer fd read failed: ", strerror(errno));
    }

    timerWheel_.Tick(NowMs() / 1000);

    // Fire any backoff reconnects that have come due
    HandleReconnects();
}

void EpollConnectionHandler::HandleAsyncTimer(int sfd)
{
    std::uint64_t expirations = 0;
    ssize_t n = RetryOnEintr([&] { return read(sfd, &expirations, sizeof(expirations)); });

    if(n < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK)
            return;

        // Same reasoning as HandleTimeoutTimer: asyncTimerFd_ is level-triggered,-
        // -a broken read here risks either silently dead async timer delivery or-
        // -an undrained-readiness busy-spin on the event loop
        logger_.Fatal("[Epoll]: Async timer fd read failed: ", strerror(errno));
    }

    std::uint64_t newTick = NowMs();
    std::uint64_t connId = 0;

    while(timerHeap_.PopExpired(newTick, connId)) {
        ClientCtx* ctx = connections_.GetPtr(connId);
        ctx->isAsyncTimerOperation = 0;

        HandleClientAsyncCallback(ctx, {nullptr, 0, {.unused = 0}, AsyncStatus::COMPLETED}, false);
    }

    // Because the async timer is one shot, update it just in case there exists more async-
    // -registered timers
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

void EpollConnectionHandler::HandleEndpointHandshake(EndpointCtx* ctx, std::uint32_t ev)
{
    WFX_TRACE();

    // SSL handshake for outbound endpoint connections
    // On success: EVENT_ENDPOINT_ONCONNECT if onConnect hook exists, else EVENT_ENDPOINT_SEND
    auto& meta = endpoints_[ctx->endpointIdx].meta;
    EventType onSuccess = meta.desc.onConnect ? EventType::EVENT_ENDPOINT_ONCONNECT : EventType::EVENT_ENDPOINT_SEND;

    if(!TryHandshake(ctx, onSuccess, EventType::EVENT_ENDPOINT_HANDSHAKE)) {
        logger_.Error("[Epoll]: TLS handshake failed for endpoint '", meta.hostname, "'");
        HandleConnectFailure(ctx, endpoints_[ctx->endpointIdx], false);
        return;
    }

    // Wait for handshake to finish
    if(ctx->eventType == EventType::EVENT_ENDPOINT_HANDSHAKE)
        return;

    // Its alive, ITS ALIVE. ITS ALIVEEEUEEUUEEEE. IN THE NAME OF GOD, NOW I KNOW WHAT IT FEELS LIKE TO BE GOD
    // - Frankenstein
    ctx->SetConnectionState(ConnectionState::CONNECTION_ALIVE);

    auto& entry = endpoints_[ctx->endpointIdx];

    // TLS handshake done, connectTimeoutSeconds phase is over. What happens next-
    // -depends on whether onConnect exists and whether a client is waiting:
    //   - onConnect hook exists -> FireOnConnect drives the handshake protocol,
    //     requestTimeoutSeconds covers onConnect + the eventual send/receive
    //   - no onConnect, but a client is waiting -> Write the already-serialized
    //     request, requestTimeoutSeconds covers the round trip
    //   - no onConnect, no client (prewarm) -> nothing to send, the refresh
    //     below is immediately superseded by ReturnEndpointToPool's idle timeout
    RefreshExpiry(ctx, entry.meta.config.requestTimeoutSeconds);

    if(ctx->eventType == EventType::EVENT_ENDPOINT_ONCONNECT)
        FireOnConnect(ctx, entry);
    else if(ctx->clientCtx)
        Write(ctx);
    else {
        ReturnEndpointToPool(ctx);

        // Slot is idle-pooled at this point. If MOD fails the fd is left in an-
        // -inconsistent epoll registration state with no way to detect future-
        // -readability/writability. Force close it rather than leak a half-dead slot
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
        HandleClientWriteReady(ctx, ev);
}

void EpollConnectionHandler::HandleEndpointEvent(EndpointCtx* ctx, std::uint32_t ev, std::uint16_t gen)
{
    WFX_TRACE();

    // SSL handshake dispatch
    if(ctx->eventType == EventType::EVENT_ENDPOINT_HANDSHAKE) {
        HandleEndpointHandshake(ctx, ev);
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
        Close(ctx);
        return;
    }

    if(ev & EPOLLIN)
        HandleEndpointEpollIn(ctx);

    // Re-check: HandleEndpointEpollIn may have closed this slot
    if((ev & EPOLLOUT) && ctx->generationId == gen)
        HandleEndpointWriteReady(ctx, ev);
}

// Single EPOLLIN dispatch point for clients, routes by eventType
void EpollConnectionHandler::HandleClientEpollIn(ClientCtx* ctx)
{
    WFX_TRACE();

    switch(ctx->eventType) {
        case EventType::EVENT_RECV:
            // Rate-limit inbound client requests
            if(!ipLimiter_.AllowRequest(ctx->connInfo)) {
                ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
                Write(ctx, HttpError::tooManyRequests);
                return;
            }

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
            // Data arrived from backend, run the parse loop
            bool eof = false;
            bool gotData = Receive(ctx, &eof);

            // Nothing to act on: EAGAIN with no new bytes, or Receive already closed on a fatal-
            // -error (ctx may be released, must not touch it)
            if(!eof && !gotData)
                return;

            // The parse callback only ever runs for a slot with a request in flight: clientCtx-
            // -for the single-slot path, pendingStreams for multiplexed (which never sets-
            // -clientCtx at all). A slot with neither is idle-pooled or prewarmed: any EOF-
            // -(peer closing an idle keep-alive) or unsolicited bytes (a misbehaving backend)-
            // -have nothing to parse or deliver, and the slot has no outputObj, so just release-
            // -it. Otherwise parse, passing eof through so a close-delimited body gets finalized-
            // -on the last call
            if(!ctx->clientCtx && !(ctx->pendingStreams && !ctx->pendingStreams->empty()))
                Close(ctx, true);
            else
                HandleEndpointReceive(ctx, eof);

            return;
        }

        case EventType::EVENT_ENDPOINT_ONCONNECT:
            // clang-format off

            // onConnect coroutine called SlotReceive and is now suspended waiting for data
            // Wake it by firing its asyncData completion with the buffer contents
            if(Receive(ctx))
                HandleEndpointAsyncCallback(ctx, {ctx->rwBuffer.GetReadData(), ctx->rwBuffer.GetReadMeta()->dataLength,
                                             {.unused = 0}, AsyncStatus::COMPLETED}, false);
            return;

            // clang-format on

        default:
            // Any other state, connection is doing something else, ignore stray EPOLLIN
            return;
    }
}

void EpollConnectionHandler::HandleClientWriteReady(ClientCtx* ctx, std::uint32_t /*ev*/)
{
    WFX_TRACE();

    switch(ctx->eventType) {
        // Client response write
        case EventType::EVENT_SEND:
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

void EpollConnectionHandler::HandleEndpointWriteReady(EndpointCtx* ctx, std::uint32_t ev)
{
    WFX_TRACE();

    switch(ctx->eventType) {
        // Endpoint request write or SlotSend from inside onConnect
        case EventType::EVENT_ENDPOINT_SEND:
            Write(ctx);
            break;

        // Immediate connect completed and onConnect hook exists, fire it now
        // This case is reached when connect() returned 0 synchronously (loopback,-
        // -same-host) and WrapConnect set EVENT_ENDPOINT_ONCONNECT before RegisterEpoll(ADD + MOD)
        // The MOD causes EPOLLOUT to fire immediately since the socket is already writable
        //
        // RegisterEpoll always arms EPOLLIN|EPOLLOUT together, so once onConnect is running-
        // -and suspended on a Receive (same eventType, opposite meaning: "waiting for a reply"-
        // -instead of "just connected"), a still-writable socket can redeliver EPOLLOUT here
        // inOnConnectPhase tells the two apart: only fire onConnect while nothing is running yet
        case EventType::EVENT_ENDPOINT_ONCONNECT: {
            if(!ctx->inOnConnectPhase)
                FireOnConnect(ctx, endpoints_[ctx->endpointIdx]);
        } break;

        // TCP connect completed, proceed to SSL handshake or directly to write/onConnect
        case EventType::EVENT_CONNECT: {
            int err = 0;
            socklen_t len = sizeof(err);

            auto& ep = endpoints_[ctx->endpointIdx];
            auto& meta = ep.meta;

            if(getsockopt(ctx->socket, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
                logger_.Error("[Epoll]: Connect failed for endpoint '", meta.hostname, "': ", strerror(err));
                HandleConnectFailure(ctx, ep, false);
                break;
            }

            ctx->SetConnectionState(ConnectionState::CONNECTION_ALIVE);

            // TLS required, try SSL handshake
            if(ctx->GetEndpointState() == EndpointState::ENDPOINT_SECURE) {
                ctx->sslConn = sslHandler_->WrapClient(ctx->socket, meta.hostname.c_str(),
                                                       std::string_view{meta.config.alpnProtocols.data,
                                                                        meta.config.alpnProtocols.length});

                if(!ctx->sslConn) {
                    HandleConnectFailure(ctx, ep, false);
                    break;
                }

                HandleEndpointHandshake(ctx, ev);
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

            // No onConnect hook. Only Write if a client request is actually-
            // -waiting in the buffer. A prewarm slot with no client has nothing-
            // -to send and should go straight to the idle pool instead
            if(ctx->clientCtx)
                Write(ctx);
            else {
                ctx->eventType = EventType::EVENT_ENDPOINT_RECV;
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
        itimerspec disarm{};
        timerfd_settime(asyncTimerFd_, 0, &disarm, nullptr);
        return;
    }

    std::uint64_t now = NowMs();
    std::uint64_t expire = min->delay;
    std::uint64_t remain = (expire <= now) ? 1 : (expire - now);

    itimerspec ts{};
    ts.it_value.tv_sec = remain / 1000;
    ts.it_value.tv_nsec = (remain % 1000) * 1'000'000;
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

    // Timeout timer ticks at most once every INVOKE_TIMEOUT_COOLDOWN seconds, so any-
    // -timeout value below that fires no earlier than the next tick. The configured-
    // -value would silently lie about how soon it actually triggers
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
    // The address can never change, so refreshing is always a no-op regardless of-
    // -any user-configured interval. Schedule the furthest-out check the wheel allows
    if(minTtlSeconds == UINT32_MAX)
        return (NowMs() / 1000) + MAX_REFRESH_SECONDS;

    std::uint32_t interval;

    // User explicitly set a refresh cadence (this is a CEILING, not an override-
    // -of TTL entirely). If the record's actual TTL is shorter, still honor it,-
    // -since refreshing later than the DNS-promised validity risks stale addresses
    if(userOverrideSeconds > 0)
        interval = std::min(userOverrideSeconds, minTtlSeconds > 0 ? minTtlSeconds : userOverrideSeconds);

    // 0 = fully TTL-driven, refresh exactly when the DNS record says to
    else
        interval = minTtlSeconds > 0 ? minTtlSeconds : MAX_REFRESH_SECONDS;

    interval = std::clamp(interval, MIN_REFRESH_SECONDS, MAX_REFRESH_SECONDS);

    // Jitter range is 10% of interval, minimum 5 seconds regardless of interval size
    // Ensures meaningful spread even at MIN_REFRESH_SECONDS where 10% would be < 1s
    std::uint32_t jitterRange = std::max(MIN_REFRESH_SECONDS, interval / 10);
    std::uint32_t hash32 = static_cast<std::uint32_t>(std::hash<std::string>{}(hostname));
    std::uint32_t jitter = static_cast<std::uint32_t>((static_cast<std::uint64_t>(hash32) * jitterRange) >> 32);

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

    // Multiplexed slot: fail and tear down every stream still pending. Only reached from the-
    // -slot-teardown paths (parse error, connection drop); HandleEndpointReceive already erases-
    // -a stream's own map entry the moment that individual stream completes.
    //
    // Detach the map from ctx BEFORE touching any entry or firing any callback below. The-
    // -callbacks resume suspended client coroutines synchronously, and a completely ordinary-
    // -app-level retry pattern (co_await the same endpoint again on failure) can reenter-
    // -SendPayloadMultiplexed from inside one of those callbacks, before this function returns-
    // -and before the caller has necessarily marked ctx unavailable. With the map already-
    // -detached, SendPayloadMultiplexed sees ctx->pendingStreams == nullptr and lazily allocates-
    // -a fresh map instead of touching the live one this loop is iterating, otherwise a-
    // -reentrant insert into *ctx->pendingStreams while this range-for is executing is undefined-
    // -behavior (unordered_map insertion may rehash, invalidating the loop's iterator)
    if(ctx->pendingStreams) {
        PendingStreamMap orphaned = std::move(*ctx->pendingStreams);
        delete ctx->pendingStreams;
        ctx->pendingStreams = nullptr;

        for(auto& [key, stream] : orphaned) {
            // Same per-stream coalesce cleanup ReleaseEndpoint does for the single-slot-
            // -coalesceKey below (fail every waiter, then erase), just scoped to this one-
            // -stream instead of the whole slot. Move the waiters out and erase the map entry-
            // -BEFORE firing any callback, same reasoning as detaching pendingStreams above:-
            // -a reentrant SendPayload with this exact coalesce key would otherwise push_back-
            // -into the very vector this loop is iterating (the entry isn't erased until after-
            // -the loop would have finished, so a reentrant call still finds and reuses it)
            if(stream.coalesceKey != 0) {
                auto it = meta.coalescePending.find(stream.coalesceKey);
                if(it != meta.coalescePending.end()) {
                    std::vector<CoalesceWaiter> waiters = std::move(it->second.waiters);
                    meta.coalescePending.erase(it);

                    for(auto& w : waiters) {
                        if(!w.clientCtx || w.clientCtx->generationId != w.generationId)
                            continue;

                        w.clientCtx->endpointCtx = nullptr;

                        AsyncResult failResult{};
                        failResult.data = nullptr;
                        failResult.dataLen = 0;
                        failResult.status = AsyncStatus::IO_FAILURE;
                        failResult.endpointStatus = EndpointStatus::INTERNAL_ERROR;
                        HandleClientAsyncCallback(w.clientCtx, failResult, false);
                    }
                }
            }

            if(stream.parseState && desc.destroyParseState)
                desc.destroyParseState(stream.parseState);

            // Abandon the stream in the protocol too. Whatever it hands back (finished or-
            // -not) is discarded: every path below is a failure delivery, never a success
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
    if(slotCtx->asyncData.AsyncComplete) {
        HandleEndpointAsyncCallback(slotCtx, {nullptr, 0, {.unused = 0}, AsyncStatus::COMPLETED}, false);
        return;
    }

    // Normal path, request sent, transition to response phase
    slotCtx->eventType = EventType::EVENT_ENDPOINT_RECV;
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
        std::size_t remaining = writeMeta->dataLength - writeMeta->writtenLength;

        ssize_t n = WrapWrite(ctx->socket, ctx->sslConn, buf, remaining);

        if(n > 0)
            writeMeta->writtenLength += n;

        // Partial progress, wait for event loop to notify when we can send more data
        else if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            ctx->eventType = EventType::EVENT_ENDPOINT_SEND;
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

    PendingStream stream = it->second;
    slotCtx->pendingStreams->erase(it);

    // The protocol owned this stream's output internally up to now; hand ownership to the-
    // -engine. Null here would mean parse() reported completion without ever finishing an-
    // -output object (a protocol bug), handled the same as the 'client vanished' case below
    void* outputObj = desc.takeStreamOutput(slotCtx->slotState, key);

    // Fan out to any coalesced waiters parked under this stream's own key (per-stream, not-
    // -per-slot: several concurrently in-flight streams on this one slot may each coalesce-
    // -under a different key)
    std::vector<CoalesceWaiter> waiters;
    if(stream.coalesceKey != 0) {
        auto& pending = entry.meta.coalescePending;
        auto pit = pending.find(stream.coalesceKey);
        if(pit != pending.end()) {
            waiters = std::move(pit->second.waiters);
            pending.erase(pit);
        }
    }

    for(auto& w : waiters) {
        // Client disconnected before result arrived
        if(!w.clientCtx || w.clientCtx->generationId != w.generationId)
            continue;

        w.clientCtx->endpointCtx = nullptr;
        // Null outputObj (protocol bug: completed without ever finishing an output) can't be-
        // -cloned; fall straight to the failure branch instead of calling cloneOutput on it
        void* cloned = outputObj ? desc.cloneOutput(slotCtx->slotState, outputObj) : nullptr;

        // Ownership transfers to the waiter's EndpointOutput<T> RAII wrapper
        if(cloned)
            HandleClientAsyncCallback(w.clientCtx, {cloned, 0, {.unused = 0}, AsyncStatus::COMPLETED}, false);
        // Kind of worst case, normally it wouldn't happen
        else {
            AsyncResult failResult{};
            failResult.data = nullptr;
            failResult.dataLen = 0;
            failResult.endpointStatus = EndpointStatus::INTERNAL_ERROR;
            failResult.status = AsyncStatus::IO_FAILURE;
            HandleClientAsyncCallback(w.clientCtx, failResult, false);
        }
    }

    if(stream.parseState && desc.destroyParseState)
        desc.destroyParseState(stream.parseState);

    if(stream.clientCtx && stream.clientCtx->generationId == stream.generationId) {
        stream.clientCtx->endpointCtx = nullptr;
        stream.clientCtx->streamKey = 0;

        if(outputObj) {
            // Ownership transfers to the primary's EndpointOutput<T> RAII wrapper
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
    // Client vanished before the result arrived (shouldn't normally happen: ReleaseClient erases-
    // -this entry synchronously on disconnect) and nothing else claimed ownership
    else if(outputObj && desc.destroyOutput)
        desc.destroyOutput(outputObj);

    // Every in-flight stream just finished: the connection has gone idle, even though a-
    // -multiplexed protocol's parse() never signals a keep-alive boundary the way a-
    // -non-multiplexed one does. Arm the idle timeout directly rather than-
    // -ReturnEndpointToPool, which would free the slot's pool bitmap bit and hide it from-
    // -FindMultiplexableSlot; a new request reusing this slot already overwrites this with-
    // -requestTimeoutSeconds via RefreshExpiry in SendPayloadMultiplexed
    if(slotCtx->pendingStreams->empty() && !slotCtx->isShuttingDown)
        RefreshExpiry(slotCtx, entry.meta.config.idleTimeoutSeconds);
}

void EpollConnectionHandler::HandleEndpointReceive(EndpointCtx* slotCtx, bool isEof)
{
    WFX_TRACE();

    auto& entry = endpoints_[slotCtx->endpointIdx];
    auto& desc = entry.meta.desc;
    auto& rwBuf = slotCtx->rwBuffer;

    while(true) {
        std::uint32_t consumed = 0;
        std::uint64_t completedKey = 0;
        ParseResult pr =
            desc.parse(slotCtx->slotState, slotCtx->parseStateObj, rwBuf.GetReadData(), rwBuf.GetReadMeta()->dataLength,
                       &consumed, slotCtx->outputObj, isEof, &completedKey);

        if(consumed > 0) {
            auto* readMeta = rwBuf.GetReadMeta();

            // Guard: consumed must never exceed what we actually have
            if(consumed > readMeta->dataLength) {
                logger_.Error("[Epoll]: While handling endpoint receive, parse returned consumed=", consumed,
                              " but dataLength=", readMeta->dataLength, ", closing slot");
                Close(slotCtx, true);
                return;
            }

            std::uint32_t remaining = readMeta->dataLength - consumed;
            if(remaining > 0)
                std::memmove(rwBuf.GetReadData(), rwBuf.GetReadData() + consumed, remaining);

            readMeta->dataLength = remaining;
        }

        // Multiplexed slot: a non-zero key means one particular stream just finished, resolve-
        // -and erase only that one. The connection itself may still be alive (pr == INCOMPLETE-
        // -is the normal case here, e.g. an HTTP/2 stream ending on a connection with others-
        // -still open), so loop back for more frames instead of falling into the switch below
        if(completedKey != 0 && slotCtx->pendingStreams) {
            ResolveMultiplexedStream(slotCtx, entry, completedKey);

            if(pr == ParseResult::INCOMPLETE)
                continue;
        }

        switch(pr) {
            case ParseResult::INCOMPLETE:
                // At EOF there are no more bytes coming. A parser that still wants more means the-
                // -response was truncated, fail the in-flight request and any waiters, then close
                if(isEof) {
                    // Set before Finalize (not just inside Close, which runs after): blocks a-
                    // -reentrant SendPayloadMultiplexed (synchronously triggered by a client-
                    // -callback Finalize fires below) from having FindMultiplexableSlot hand it-
                    // -this exact slot while its fate is still being decided
                    slotCtx->isShuttingDown = 1;
                    FinalizeEndpointRequest(slotCtx, entry.meta, false);
                    Close(slotCtx, true);
                    return;
                }

                // Need more bytes. eventType is already EVENT_ENDPOINT_RECV from Receive()
                return;

            case ParseResult::COMPLETE_KEEP_ALIVE:
            case ParseResult::COMPLETE_CLOSE: {
                // Multiplexed slot: no single primary request lives in slotCtx->clientCtx (every-
                // -request goes through pendingStreams, resolved above via completedKey). This-
                // -return value just means the whole connection is done; behave like a plain-
                // -teardown/idle-return and let FinalizeEndpointRequest fail any orphaned streams
                if(slotCtx->pendingStreams && !slotCtx->clientCtx) {
                    // Same reentrancy guard as the isEof branch above. For the KEEP_ALIVE case-
                    // -the slot survives and goes back to the idle pool healthy, so clear the-
                    // -flag again afterward -- it must not stay permanently poisoned against-
                    // -future multiplexing just because this one connection cycle finished
                    slotCtx->isShuttingDown = 1;
                    FinalizeEndpointRequest(slotCtx, entry.meta, false);

                    if(pr == ParseResult::COMPLETE_KEEP_ALIVE) {
                        slotCtx->isShuttingDown = 0;
                        rwBuf.ClearReadBuffer();
                        slotCtx->eventType = EventType::EVENT_ENDPOINT_RECV;
                        ReturnEndpointToPool(slotCtx);
                    }
                    else
                        Close(slotCtx);

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
                clientCtx->endpointCtx = nullptr;
                slotCtx->outputObj = nullptr; // disown before any cleanup

                // Fan out clones to waiters while slotCtx->slotState is still live
                // This MUST happen before close/pool-return: COMPLETE_CLOSE destroys slotState
                for(auto& w : waiters) {
                    // Client disconnected before result arrived
                    if(!w.clientCtx || w.clientCtx->generationId != w.generationId)
                        continue;

                    w.clientCtx->endpointCtx = nullptr;
                    void* cloned = desc.cloneOutput(slotCtx->slotState, outputObj);

                    // Ownership transfers to the waiter's EndpointOutput<T> RAII wrapper
                    if(cloned)
                        HandleClientAsyncCallback(w.clientCtx, {cloned, 0, {.unused = 0}, AsyncStatus::COMPLETED},
                                                  false);
                    // Kind of worst case, normally it wouldn't happen
                    else {
                        AsyncResult failResult{};
                        failResult.data = nullptr;
                        failResult.dataLen = 0;
                        failResult.endpointStatus = EndpointStatus::INTERNAL_ERROR;
                        failResult.status = AsyncStatus::IO_FAILURE;
                        HandleClientAsyncCallback(w.clientCtx, failResult, false);
                    }
                }

                if(pr == ParseResult::COMPLETE_KEEP_ALIVE) {
                    // Only reset parse state, do NOT touch outputObj here
                    // If resetParseState is absent, destroy+null so SendPayload recreates fresh next request-
                    // -without this, a keep-alive slot would carry dirty parse state into the next request
                    if(slotCtx->parseStateObj) {
                        if(desc.resetParseState)
                            desc.resetParseState(slotCtx->parseStateObj);
                        else {
                            desc.destroyParseState(slotCtx->parseStateObj);
                            slotCtx->parseStateObj = nullptr;
                        }
                    }

                    rwBuf.ClearReadBuffer();
                    slotCtx->eventType = EventType::EVENT_ENDPOINT_RECV;
                    ReturnEndpointToPool(slotCtx);
                }
                else {
                    // CLOSE path: parse state destroy only, output handled below
                    if(slotCtx->parseStateObj && desc.destroyParseState) {
                        desc.destroyParseState(slotCtx->parseStateObj);
                        slotCtx->parseStateObj = nullptr;
                    }

                    Close(slotCtx);
                }

                // Ownership transfers to the primary's EndpointOutput<T> RAII wrapper
                HandleClientAsyncCallback(clientCtx, {outputObj, 0, {.unused = 0}, AsyncStatus::COMPLETED}, false);
                return;
            }

            case ParseResult::ERROR:
            default: {
                // Notify client if exists (as its suspended)
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

                // Same reentrancy guard as the isEof branch above
                slotCtx->isShuttingDown = 1;
                FinalizeEndpointRequest(slotCtx, entry.meta, false);
                Close(slotCtx, true);

                return;
            }
        }
    }
}

void EpollConnectionHandler::FireOnConnect(EndpointCtx* slotCtx, EndpointEntry& entry)
{
    auto& desc = entry.meta.desc;

    // Send / Receive go through the endpoint API directly (SlotHandle::Send/Receive-
    // -call SlotSend/SlotReceive themselves), nothing needed here for either
    // Close is nullptr. Slot lifetime is managed by the engine via ConnectResult
    EndpointSlotHandle handle{};
    handle.impl = slotCtx;
    handle.NegotiatedProtocol = Shared::GetEndpointAPIExt1()->NegotiatedProtocol;
    handle.Close = nullptr;

    AsyncData onDone{};
    onDone.userData = slotCtx;
    onDone.AsyncComplete = OnSlotConnected;
    onDone.AsyncDestroy = nullptr;

    slotCtx->asyncData = onDone;
    slotCtx->eventType = EventType::EVENT_ENDPOINT_ONCONNECT;
    slotCtx->inOnConnectPhase = 1;

    desc.onConnect(handle, slotCtx->slotState, onDone.AsyncComplete, onDone.userData);
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
        EndpointStatus sr = SerializeSingleSlot(slotCtx, meta, req);
        if(sr != EndpointStatus::SUCCESS) {
            FailDeferredRequest(slotCtx, sr);
            return false;
        }

        if(slotCtx->coalesceKey != 0)
            meta.coalescePending[slotCtx->coalesceKey].inflight = slotCtx;

        return true;
    }

    // Multiplexed: same serialize step SendPayloadMultiplexed uses. clientCtx, parseStateObj-
    // -and coalesceKey were stashed on the slot in place of a real PendingStream entry (which-
    // -needs a streamKey that only exists once serialize() actually runs)
    std::uint64_t streamKey = 0;
    EndpointStatus sr = SerializeMultiplexed(slotCtx, meta, req, &streamKey);
    if(sr != EndpointStatus::SUCCESS) {
        FailDeferredRequest(slotCtx, sr);
        return false;
    }

    ClientCtx* client = slotCtx->clientCtx;
    void* parseState = slotCtx->parseStateObj;
    std::uint64_t coalesceKey = slotCtx->coalesceKey;

    slotCtx->clientCtx = nullptr;
    slotCtx->parseStateObj = nullptr;
    slotCtx->coalesceKey = 0;

    if(coalesceKey != 0)
        meta.coalescePending[coalesceKey].inflight = slotCtx;

    if(!slotCtx->pendingStreams)
        slotCtx->pendingStreams = new PendingStreamMap();

    (*slotCtx->pendingStreams)[streamKey] = PendingStream{client, parseState, coalesceKey, client->generationId};
    client->streamKey = streamKey;

    return true;
}

void EpollConnectionHandler::FailDeferredRequest(EndpointCtx* slotCtx, EndpointStatus status)
{
    // NULL clientCtx first so Close() -> ReleaseEndpoint() doesn't ALSO try to notify it,-
    // -with a disconnect-reason-derived status instead of this specific one
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

    // FinalizeEndpointRequest (via Close -> ReleaseEndpoint) destroys parseStateObj for us,-
    // -whether it holds a real single-slot parse state or a stashed multiplexed reqParseState
    Close(slotCtx, true);
}

void EpollConnectionHandler::OnSlotConnected(void* ud, AsyncResult result)
{
    auto* slotCtx = static_cast<EndpointCtx*>(ud);
    auto& entry = instance_->endpoints_[slotCtx->endpointIdx];

    slotCtx->inOnConnectPhase = 0;

    // Any failure routes through the connect-failure funnel: a client-waiting slot fails fast, an-
    // -explicit FATAL is discarded, and a background slot returning RETRY (or a coroutine that errored-
    // -out) reconnects with backoff. connectResult is only meaningful when the coroutine completed
    if(result.status != AsyncStatus::COMPLETED || result.connectResult == ConnectResult::FATAL ||
       result.connectResult == ConnectResult::RETRY) {
        bool fatal = (result.status == AsyncStatus::COMPLETED && result.connectResult == ConnectResult::FATAL);
        instance_->HandleConnectFailure(slotCtx, entry, fatal);
        return;
    }

    // onConnect phase is done. The read buffer may contain protocol handshake data-
    // -(auth packets, server greeting, etc.) consumed by the coroutine via SlotReceive
    // That data must not bleed into the response parse loop, so clear it before the-
    // -slot enters normal request/response operation
    slotCtx->rwBuffer.ClearReadBuffer();

    // A request deferred by SendPayload/SendPayloadMultiplexed only serializes now, so the-
    // -handshake's own writes always precede it on the wire. On failure the client is already-
    // -notified and the slot already torn down, so just stop here
    if(slotCtx->pendingConnectReq && !instance_->FlushDeferredRequest(slotCtx, entry))
        return;

    // ConnectResult::READY, slot is pooled and ready. A real request is waiting either via the-
    // -single-slot clientCtx (non-multiplexed) or a freshly inserted pendingStreams entry-
    // -(multiplexed, see SendPayloadMultiplexed). Either way the write buffer already has-
    // -serialized bytes queued. Prewarm sets neither
    if(slotCtx->clientCtx || (slotCtx->pendingStreams && !slotCtx->pendingStreams->empty())) {
        // Non-prewarm: SendPayload already serialized a request into the write buffer
        // Drive the write directly instead of calling RegisterEpoll(MOD) and waiting-
        // -for EPOLLOUT, because in ET mode the EPOLLOUT edge was consumed when EVENT_CONNECT-
        // -fired and a MOD on an already-writable fd does not generate a new edge
        slotCtx->eventType = EventType::EVENT_ENDPOINT_SEND;
        instance_->Write(slotCtx);
    }
    else {
        // Prewarm: no client waiting. Return this slot to the free list so-
        // -SendPayload's AllocSlot can actually lease it for a future request
        // Without this, prewarmed slots stay permanently marked allocated and-
        // -are never reused, silently shrinking the effective pool by 'prewarm' count
        slotCtx->eventType = EventType::EVENT_ENDPOINT_RECV;
        instance_->ReturnEndpointToPool(slotCtx);

        if(!instance_->RegisterEpoll(slotCtx, EPOLL_CTL_MOD)) {
            instance_->logger_.Error("[Epoll]: 'RegisterEpoll(MOD)' failed for endpoint after onConnect prewarm: ",
                                     strerror(errno));
            instance_->Close(slotCtx, true);
        }
    }
}

std::uint32_t EpollConnectionHandler::ComputeBackoffSeconds(const EndpointConfig& config, std::uint16_t attempt)
{
    // Exponential: base * 2 ^ attempt, capped at max (computed without overflow)
    std::uint64_t exp = config.reconnectBackoffBase;
    for(std::uint16_t i = 0; i < attempt && exp < config.reconnectBackoffMax; i++)
        exp <<= 1;

    std::uint32_t cap = static_cast<std::uint32_t>(std::min<std::uint64_t>(exp, config.reconnectBackoffMax));

    // Pick uniformly in [0, cap] so a whole pool never reconnects in lockstep and re-DDoSes a-
    // -recovering upstream. xorshift64 is plenty for jitter
    reconnectRngState_ ^= reconnectRngState_ << 13;
    reconnectRngState_ ^= reconnectRngState_ >> 7;
    reconnectRngState_ ^= reconnectRngState_ << 17;

    std::uint32_t jittered = (cap == 0) ? 0 : static_cast<std::uint32_t>(reconnectRngState_ % (cap + 1ULL));

    // Clamp to at least 1s so we never busy-retry at a 0s delay
    return jittered < 1 ? 1 : jittered;
}

void EpollConnectionHandler::ScheduleReconnect(EndpointCtx* ctx, EndpointEntry& entry)
{
    WFX_TRACE();

    auto& meta = entry.meta;
    std::uint32_t idx = entry.pool.GetIndex(ctx);

    // Soft close: drop the socket + TLS + per-request objects, but KEEP the slot reserved in the-
    // -pool and KEEP slotState alive (the slot persists across retry attempts; onConnect re-runs-
    // -against the same slotState, and onDisconnect fires only on final ejection via ReleaseEndpoint)
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

    // If a connect-phase timeout interrupted a suspended onConnect coroutine, destroy its frame now-
    // -(mirrors ReleaseEndpoint) so it doesn't leak. A fresh coroutine starts on the reconnect attempt
    if(ctx->inOnConnectPhase)
        HandleEndpointAsyncCallback(ctx, {}, true);

    FinalizeEndpointRequest(ctx, meta, false);

    ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
    ctx->inOnConnectPhase = 0;
    ctx->clientCtx = nullptr; // background slot, no waiting client (already failed/never had one)
    ctx->isAwaitingReconnect = 1;

    std::uint32_t backoff = ComputeBackoffSeconds(meta.config, ctx->reconnectAttempts);
    ctx->reconnectAttempts++;

    pendingReconnects_.push_back(
        {NowMs() + static_cast<std::uint64_t>(backoff) * 1000ULL, ctx->endpointIdx, ctx->generationId, idx});

    logger_.Debug("[Epoll]: Endpoint '", meta.hostname, "' slot ", idx, " (gen ", ctx->generationId,
                  ") connect failed, reconnect attempt ", ctx->reconnectAttempts, "/", meta.config.maxReconnectAttempts,
                  " in ", backoff, "s");
}

void EpollConnectionHandler::HandleConnectFailure(EndpointCtx* ctx, EndpointEntry& entry, bool fatal,
                                                  DisconnectReason reason)
{
    WFX_TRACE();

    auto& meta = entry.meta;

    // Permanent teardown when: a client is actively waiting (fail it fast, it can retry at the app-
    // -layer rather than block on backoff), the failure is fatal, or retries are exhausted. Close ->-
    // -ReleaseEndpoint fires onDisconnect once, notifies any waiting client (with reason so the-
    // -status code is accurate), and frees the slot
    bool exhausted = ctx->reconnectAttempts >= meta.config.maxReconnectAttempts;
    if(ctx->clientCtx || fatal || exhausted) {
        // A background slot (no waiting client) being permanently discarded is the operationally-
        // -significant event worth alerting on. Client-driven failures already surface to the caller-
        // -via the returned status, so they don't need an engine-level error here
        if(!ctx->clientCtx)
            logger_.Error("[Epoll]: Endpoint '", meta.hostname, "' slot ", entry.pool.GetIndex(ctx),
                          " giving up after ", ctx->reconnectAttempts, "/", meta.config.maxReconnectAttempts,
                          " reconnect attempts, slot ejected");

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

    std::uint64_t now = NowMs();

    for(std::size_t i = 0; i < pendingReconnects_.size();) {
        PendingReconnect& pr = pendingReconnects_[i];
        if(pr.wakeAtMs > now) {
            i++;
            continue;
        }

        // Due: remove the entry (order doesn't matter, swap-erase)
        PendingReconnect due = pr;
        pendingReconnects_[i] = pendingReconnects_.back();
        pendingReconnects_.pop_back();

        auto& entry = endpoints_[due.endpointIdx];
        EndpointCtx* ctx = entry.pool.GetPtr(due.slotIdx);

        // Stale guard: slot must still be the same parked one. isAwaitingReconnect is cleared by-
        // -Reset() on teardown (covers freed-but-not-reused), generationId catches freed-and-reused
        if(!ctx || !ctx->isAwaitingReconnect || ctx->generationId != due.generationId)
            continue;

        ctx->isAwaitingReconnect = 0;

        // Re-attempt. CreateAndConnect advances nextAddrIdx, so this naturally rotates to the next-
        // -resolved IP. WrapConnect re-registers epoll and re-arms the connect timeout
        EndpointStatus s = WrapConnect(ctx, entry);
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

        std::uint32_t prewarmCount = std::min(cfg.prewarm, cfg.connLimit);

        for(std::uint32_t j = 0; j < prewarmCount; j++) {
            EndpointCtx* slotCtx = GetEndpointConnection(i);
            if(!slotCtx)
                break; // pool exhausted for this endpoint

            // Slot state must exist before onConnect fires
            if(!slotCtx->slotState && desc.createSlotState)
                slotCtx->slotState = desc.createSlotState(desc.userCtx);

            EndpointStatus result = WrapConnect(slotCtx, entry);

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

    // With MAX_DNS_THREADS capping concurrent resolves, the queue can only exceed-
    // -MAX_DNS_RESULT_QUEUE_SIZE if eventfd writes are persistently failing and-
    // -HandleDnsResultReady is never draining it
    {
        std::lock_guard<std::mutex> lock(dnsResultMutex_);
        if(dnsResultQueue_.size() >= MAX_DNS_RESULT_QUEUE_SIZE)
            logger_.Fatal("[Epoll]: DNS result queue exceeded ", MAX_DNS_RESULT_QUEUE_SIZE,
                          " entries. Event signaling has persistently failed, DNS refresh dead engine wide");
    }

    // Computed once and reused across all failure paths (semaphore busy, write fail, spawn fail)
    std::uint64_t retrySchedule = ComputeNextDnsRefresh(MIN_REFRESH_SECONDS, 0, meta.hostname);

    // All resolver slots busy. Reschedule with jitter so waiting endpoints don't-
    // -all reconverge at the same instant when slots free up
    if(!dnsThreadSemaphore_.try_acquire()) {
        meta.dnsNextRefreshSeconds = retrySchedule;
        return;
    }

    // Push schedule to ceiling (doubles as the in-flight marker, prevents Run()-
    // -from spawning a second overlapping resolve for this endpoint)
    meta.dnsNextRefreshSeconds = (NowMs() / 1000) + MAX_REFRESH_SECONDS;

    // meta captured by reference below (safe since endpoints_ is fully-
    // -populated before Run() starts and never reallocated afterward)
    try {
        std::thread([=, this, &meta]() {
            // Always release the semaphore slot on exit regardless of outcome
            struct SemGuard {
                std::counting_semaphore<MAX_DNS_THREADS>& sem;
                ~SemGuard()
                {
                    sem.release();
                }
            } guard{dnsThreadSemaphore_};

            Utils::ResolvedAddrs newAddrs;
            std::uint32_t minTtl = 0;
            bool ok = DNSResolver::Resolve(meta.hostname.c_str(), meta.port, newAddrs, minTtl);

            // Push AND signal under a single lock. The loop's drain (HandleDnsResultReady) also-
            // -takes this mutex, so keeping the write() inside the same critical section guarantees-
            // -back() below still refers to the node we just pushed. Releasing the lock between the-
            // -push and the back() annotation would let a concurrent drain swap the queue empty-
            // -(back() on an empty vector is UB) or let another resolver append (wrong node tagged)
            // The reader read()s the eventfd before taking this lock, so it can still unblock a-
            // -writer that stalls inside write() while holding the mutex
            {
                std::lock_guard<std::mutex> lock(dnsResultMutex_);
                dnsResultQueue_.push_back({ok, endpointIdx, minTtl, std::move(newAddrs)});

                std::uint64_t one = 1;
                ssize_t written = RetryOnEintr([&] { return write(dnsResultEventFd_, &one, sizeof(one)); });

                // On write error, tag the just-pushed node. If future writes succeed, the errors-
                // -will be logged. If future writes keep failing, the queue fills rapidly and-
                // -triggers the MAX_DNS_RESULT_QUEUE_SIZE condition above
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
    ssize_t n = RetryOnEintr([&] { return read(sfd, &val, sizeof(val)); });

    if(n < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK)
            return;

        // Same reason as why HandleTimeoutTimer and HandleAsyncTimer fail, too lazy to-
        // -write the reason again
        logger_.Fatal("[Epoll]: DNS result eventfd read failed: ", strerror(errno));
    }

    std::vector<DnsResult> results;
    {
        std::lock_guard<std::mutex> lock(dnsResultMutex_);
        results.swap(dnsResultQueue_);
    }

    std::uint64_t nowSeconds = NowMs() / 1000;

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
    std::uint32_t idx = connections_.GetIndex(ctx);

    // Pack => [( EndpointIdx (16) | GenerationID (16) ) and PoolIdx (Low 32)]
    return (static_cast<std::uint64_t>(CLIENT_CONNECTION_TAG) << 48) |
           (static_cast<std::uint64_t>(ctx->generationId) << 32) | static_cast<std::uint64_t>(idx);
}

std::uint64_t EpollConnectionHandler::PackEpollData(EndpointCtx* ctx)
{
    std::uint32_t idx = endpoints_[ctx->endpointIdx].pool.GetIndex(ctx);

    // Pack => [( EndpointIdx (16) | GenerationID (16) ) and PoolIdx (Low 32)]
    return (static_cast<std::uint64_t>(ctx->endpointIdx) << 48) |
           (static_cast<std::uint64_t>(ctx->generationId) << 32) | static_cast<std::uint64_t>(idx);
}

EndpointStatus EpollConnectionHandler::CreateAndConnect(EndpointCtx* ctx, EndpointMetadata& epMeta)
{
    // Should never happen post AllocateEndpoint validation, but guard anyway. An-
    // -empty list here would be a div/mod-by-zero on the round-robin pick below
    if(epMeta.addrs.empty())
        return EndpointStatus::CONNECT_FAILURE;

    // Round-robin pick. nextAddrIdx keeps counting up across the uint16_t range and-
    // -wraps naturally. We only ever use it modulo the current list size, so the-
    // -stored cursor never needs to know or care about addrs.size() directly
    std::uint16_t idx = epMeta.nextAddrIdx % static_cast<std::uint16_t>(epMeta.addrs.size());
    epMeta.nextAddrIdx++;

    ResolvedAddr& chosen = epMeta.addrs[idx];

    ctx->socket = socket(chosen.addr.ss_family, SOCK_STREAM, 0);
    if(ctx->socket < 0)
        return EndpointStatus::SOCKET_FAILURE;

    if(!SetNonBlocking(ctx->socket))
        return EndpointStatus::SOCKET_FAILURE;

    while(true) {
        int ret = connect(ctx->socket, reinterpret_cast<const sockaddr*>(&chosen.addr), chosen.addrLen);
        if(ret == 0)
            return EndpointStatus::PENDING;

        if(errno == EINTR)
            continue;

        if(errno == EINPROGRESS) {
            ctx->eventType = EventType::EVENT_CONNECT;
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
        ctx->eventType = EventType::EVENT_RECV;

    if(!RegisterEpoll(ctx, EPOLL_CTL_ADD)) {
        Close(ctx);
        return;
    }

    metrics_->network.accepts++;

    // Set an initial timeout for the new connection so they don't connect-
    // -and stay idle forever
    RefreshExpiry(ctx, config_.networkConfig.idleTimeout);
}

EndpointStatus EpollConnectionHandler::WrapConnect(EndpointCtx* ctx, EndpointEntry& entry)
{
    WFX_TRACE();

    // Caller is responsible for error handling, this function never calls Close()
    auto& meta = entry.meta;
    auto& desc = meta.desc;

    EndpointStatus ccResult = CreateAndConnect(ctx, meta);
    if(ccResult != EndpointStatus::PENDING)
        return ccResult;

    // Immediate connect path (connect() returned 0 synchronously, no EINPROGRESS)
    // For the plain TCP case eventType is still EVENT_ACCEPT (the slot default). We must-
    // -set it to the correct state now so that when RegisterEpoll(MOD) fires EPOLLOUT,-
    // -HandleWriteReady dispatches correctly
    //   - onConnect exists: set EVENT_ENDPOINT_ONCONNECT so HandleWriteReady calls FireOnConnect
    //   - no onConnect: set EVENT_ENDPOINT_SEND so HandleWriteReady calls Write()
    // The SSL case is excluded here because TryHandshake below sets eventType itself
    const bool immediateConnect = (ctx->eventType != EventType::EVENT_CONNECT);

    if(immediateConnect) {
        ctx->SetConnectionState(ConnectionState::CONNECTION_ALIVE);

        // Immediate SSL connect (no EINPROGRESS). Attempt handshake now
        if(ctx->GetEndpointState() == EndpointState::ENDPOINT_SECURE) {
            ctx->sslConn = sslHandler_->WrapClient(ctx->socket, meta.hostname.c_str(),
                                                   std::string_view{meta.config.alpnProtocols.data,
                                                                    meta.config.alpnProtocols.length});
            if(!ctx->sslConn)
                return EndpointStatus::SSL_FAILURE;

            EventType onSuccess = desc.onConnect ? EventType::EVENT_ENDPOINT_ONCONNECT : EventType::EVENT_ENDPOINT_RECV;
            if(!TryHandshake(ctx, onSuccess, EventType::EVENT_ENDPOINT_HANDSHAKE))
                return EndpointStatus::SSL_FAILURE;
        }
        else
            ctx->eventType = desc.onConnect ? EventType::EVENT_ENDPOINT_ONCONNECT : EventType::EVENT_ENDPOINT_SEND;
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

    // Three possible states after the connect/handshake attempt above:
    //   - still mid-TCP-connect or mid-TLS-handshake (async, EPOLLOUT pending)
    //     -> arm connectTimeoutSeconds, the in-progress connection must finish in time
    //   - connected (and handshake done, if any) AND a client is waiting
    //     -> arm requestTimeoutSeconds, onConnect/Write is about to run for that client
    //   - connected with no client waiting (prewarm, no onConnect hook)
    //     -> nothing to do or send, park the slot straight in the idle pool
    //
    // A waiting client shows up as clientCtx (single-slot) OR a non-empty pendingStreams-
    // -(multiplexed, see SendPayloadMultiplexed). Missing the second case here returns a-
    // -slot to the free pool while it's still mid-handshake with a real stream pending,
    // letting a concurrent SendPayload lease the same slot out from under it
    bool hasWaitingClient = ctx->clientCtx || (ctx->pendingStreams && !ctx->pendingStreams->empty());

    if(ctx->eventType == EventType::EVENT_CONNECT || ctx->eventType == EventType::EVENT_ENDPOINT_HANDSHAKE)
        RefreshExpiry(ctx, meta.config.connectTimeoutSeconds);
    else if(hasWaitingClient)
        RefreshExpiry(ctx, meta.config.requestTimeoutSeconds);
    else
        ReturnEndpointToPool(ctx);

    return EndpointStatus::PENDING;
}

ssize_t EpollConnectionHandler::WrapRead(WFXSocket socket, void* sslConn, char* buf, std::size_t len)
{
    if(!sslConn) {
        ssize_t n = ::recv(socket, buf, len, 0);

        const bool ok = (n > 0);
        metrics_->network.reads += ok;
        metrics_->network.bytesRead += ok ? static_cast<std::uint64_t>(n) : 0;

        return n;
    }

    SSLResult result = sslHandler_->Read(sslConn, buf, static_cast<int>(len));

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
        ssize_t n = ::send(socket, buf, len, MSG_NOSIGNAL);

        const bool ok = (n > 0);
        metrics_->network.writes += ok;
        metrics_->network.bytesWritten += ok ? static_cast<std::uint64_t>(n) : 0;

        return n;
    }

    SSLResult result = sslHandler_->Write(sslConn, buf, static_cast<int>(len));

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
        ssize_t n = ::sendfile(ctx->socket, fd, offset, count);

        const bool ok = (n > 0);
        metrics_->network.fileCalls += ok;
        metrics_->network.fileBytesWritten += ok ? static_cast<std::uint64_t>(n) : 0;

        return n;
    }

    SSLResult result = sslHandler_->WriteFile(ctx->sslConn, fd, offset ? *offset : 0, count);

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
                                        ssize_t r = pread(fi->fd, buffer.buffer, buffer.size, fi->offset);

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

#endif // !WFX_LINUX_USE_IO_URING