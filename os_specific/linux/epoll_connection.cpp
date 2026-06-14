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
#include <sys/timerfd.h>
#include <fcntl.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h>

namespace WFX::OSSpecific {

// Used by 'OnSlotConnected' to call back into the engine without a capture
EpollConnectionHandler* EpollConnectionHandler::instance_ = nullptr;

// vvv Constructor & Destructor vvv
EpollConnectionHandler::EpollConnectionHandler(bool useHttps) : useHttps_(useHttps)
{
    instance_ = this;

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
                             // Endpoint: always close on timeout (idle or in-flight)
                             // Also, connId is an absolute timer wheel index, recover pool index by subtracting-
                             // -timerBase
                             auto& entry = endpoints_[extra];
                             std::uint32_t slotIdx = connId - entry.meta.timerBase;
                             EndpointCtx* ctx = entry.pool.GetPtr(slotIdx);
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
}

void EpollConnectionHandler::SetEngineCallback(ReceiveCallback onData)
{
    onReceive_ = std::move(onData);
}

std::uint16_t EpollConnectionHandler::AllocateEndpoint(const char* host, EndpointDesc desc, EndpointConfig config)
{
    if(endpoints_.size() >= MAX_DISTINCT_ENDPOINTS)
        logger_.Fatal("[Epoll]: Too many distinct domain endpoints registered");

    std::string_view hostView{host};

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

    // Used in RefreshExpiry(EndpointCtx*) overload, do check it out for info
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
    meta.dnsNextRefreshMs =
        config.dnsRefreshSeconds > 0 ? NowMs() + static_cast<std::uint64_t>(config.dnsRefreshSeconds) * 1000 : 0;

    // TODO: AUTO logic can be expanded later (port-based heuristics)
    bool useTLS = (config.tlsConfig == EndpointTLSConfig::FORCE_REQUIRE || config.tlsConfig == EndpointTLSConfig::AUTO);

    if(!ResolveHost(meta.hostname.c_str(), portStr.c_str(), &meta.addr, &meta.addrLen))
        logger_.Fatal("[Epoll]: Failed to resolve endpoint: ", host);

    for(std::uint32_t i = 0; i < pool.GetSlots(); i++) {
        EndpointCtx* ctx = pool.GetPtr(i);
        ctx->endpointIdx = endpointIdx;
        ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
        ctx->SetEndpointState(useTLS ? EndpointState::ENDPOINT_SECURE : EndpointState::ENDPOINT_INSECURE);
    }

    // Prewarm fires on first Run() iteration via HandlePrewarm()

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
        logger_.Error("[Epoll]: 'Stream()' called but received empty generator");
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

    // Sanity check
    if(!ctx)
        return;

    // Force close bypasses any in-progress shutdown or state checks
    if(!forceClose && ctx->isShuttingDown)
        return;

    ctx->isShuttingDown = 1;

    if(ctx->sslConn) {
        // Skip clean shutdown, nuke it immediately
        if(forceClose) {
            sslHandler_->ForceShutdown(ctx->sslConn);
            ctx->sslConn = nullptr;
        }
        else {
            auto res = sslHandler_->Shutdown(ctx->sslConn);

            // Shutdown finished or failed immediately. Proceed to synchronous cleanup
            if(res == SSLReturn::SUCCESS || res == SSLReturn::FATAL)
                ctx->sslConn = nullptr;

            // Wait for the event loop to complete the shutdown
            else {
                // Endpoint and client shutdowns are distinct so the event loop-
                // -can route them without an 'IsEndpoint()' check
                ctx->eventType = EventType::EVENT_SHUTDOWN;
                return;
            }
        }
    }

    // Synchronous cleanup for both non-SSL and SSL paths
    (void)RegisterEpoll(ctx, EPOLL_CTL_DEL);
    ReleaseClient(ctx);
}

void EpollConnectionHandler::Close(EndpointCtx* ctx, bool forceClose, DisconnectReason disconnectReason)
{
    WFX_TRACE();

    // Sanity check
    if(!ctx)
        return;

    // Force close bypasses any in-progress shutdown or state checks
    if(!forceClose && ctx->isShuttingDown)
        return;

    ctx->isShuttingDown = true;

    if(ctx->sslConn) {
        // Skip clean shutdown, nuke it immediately
        if(forceClose) {
            sslHandler_->ForceShutdown(ctx->sslConn);
            ctx->sslConn = nullptr;
        }
        else {
            auto res = sslHandler_->Shutdown(ctx->sslConn);

            // Shutdown finished or failed immediately. Proceed to synchronous cleanup
            if(res == SSLReturn::SUCCESS || res == SSLReturn::FATAL)
                ctx->sslConn = nullptr;

            // Wait for the event loop to complete the shutdown
            else {
                // Endpoint and client shutdowns are distinct so the event loop-
                // -can route them without an 'IsEndpoint()' check
                ctx->eventType = EventType::EVENT_ENDPOINT_SHUTDOWN;
                return;
            }
        }
    }

    // Synchronous cleanup for both non-SSL and SSL paths
    (void)RegisterEpoll(ctx, EPOLL_CTL_DEL);
    ReleaseEndpoint(ctx, disconnectReason);
}

// vvv Endpoint Operations vvv
EndpointStatus EpollConnectionHandler::SendPayload(ClientCtx* clientCtx, std::uint16_t endpointIdx, const void* req,
                                                   AsyncData asyncData)
{
    if(endpointIdx >= endpoints_.size())
        return EndpointStatus::INVALID_KEY;

    auto& entry = endpoints_[endpointIdx];
    auto& meta = entry.meta;
    auto& desc = meta.desc;

    // If an identical in-flight request exists, park the client as a waiter
    if(desc.coalesceKey) {
        std::uint64_t key = desc.coalesceKey(req);
        if(key != 0) {
            auto it = meta.coalescePending.find(key);
            if(it != meta.coalescePending.end()) {
                // TODO: push clientCtx onto a per-slot waiter list so it gets-
                // -notified when the in-flight request completes (coalesce follow-up)
                clientCtx->endpointCtx = it->second;
                return EndpointStatus::PENDING;
            }
        }
    }

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

    auto& rwBuf = slotCtx->rwBuffer;
    if(!rwBuf.IsWriteInitialized() && !rwBuf.InitWriteBuffer(config_.networkConfig.maxSendBufferSize)) {
        FinalizeEndpointRequest(slotCtx, desc, false);
        ReleaseEndpoint(slotCtx);
        return EndpointStatus::BUFFER_ERROR;
    }

    // Reset write buffer before every request so reused keep-alive slots start clean
    // On a fresh slot this is a no-op (dataLength and writtenLength are already 0)
    rwBuf.ClearWriteBuffer();

    // Serialize the request directly into the write buffer. We pass GetWriteData()-
    // -(start of data region) since ClearWriteBuffer just set dataLength to 0, making-
    // -the full buffer available from the beginning
    // After a successful serialize we set dataLength = written explicitly so Write()-
    // -knows how many bytes to send. AdvanceWriteLength is NOT used here because it-
    // -advances writtenLength (bytes sent out) capped at dataLength, and with dataLength-
    // -still 0 that would leave both fields at 0 and Write() would skip the send entirely
    while(true) {
        auto* writeMeta = rwBuf.GetWriteMeta();
        std::uint32_t written = 0;

        SerializeResult sr =
            desc.serialize(slotCtx->slotState, req, rwBuf.GetWriteData(), writeMeta->bufferSize, &written);

        if(sr == SerializeResult::OK) {
            writeMeta->dataLength = written;
            writeMeta->writtenLength = 0;
            break;
        }

        if(sr == SerializeResult::BUFFER_TOO_SMALL) {
            // GenericGrowBuffer bails early when dataLength < bufferSize because it thinks-
            // -there is still room. Since we cleared dataLength to 0, we must temporarily-
            // -set it to bufferSize to convince GrowWriteBuffer to actually grow, then reset-
            // -it to 0 afterwards so the next serialize attempt sees the full new buffer
            writeMeta->dataLength = writeMeta->bufferSize;
            bool grew =
                rwBuf.GrowWriteBuffer(config_.networkConfig.sendBufferIncSize, config_.networkConfig.maxSendBufferSize);
            writeMeta->dataLength = 0;

            if(!grew) {
                FinalizeEndpointRequest(slotCtx, desc, false);
                ReleaseEndpoint(slotCtx);
                return EndpointStatus::INSUFFICIENT_BUFFER;
            }

            continue;
        }

        // SerializeResult::ERROR
        FinalizeEndpointRequest(slotCtx, desc, false);
        ReleaseEndpoint(slotCtx);
        return EndpointStatus::SERIALIZE_ERROR;
    }

    // Register in coalesce map after successful serialize
    if(desc.coalesceKey) {
        std::uint64_t key = desc.coalesceKey(req);
        if(key != 0)
            meta.coalescePending[key] = slotCtx;
    }

    slotCtx->clientCtx = clientCtx;
    clientCtx->endpointCtx = slotCtx;
    clientCtx->asyncData = asyncData;

    EndpointStatus result;

    // Closed slot, start from connecting to endpoint
    if(slotCtx->GetConnectionState() == ConnectionState::CONNECTION_CLOSE)
        result = WrapConnect(slotCtx, entry);

    // Re-use existing connection
    else {
        slotCtx->eventType = EventType::EVENT_ENDPOINT_SEND;

        // Refresh timeout so the idle timer does not fire mid-request
        RefreshExpiry(slotCtx, meta.config.idleTimeoutSeconds);

        result = RegisterEpoll(slotCtx, EPOLL_CTL_MOD) ? EndpointStatus::PENDING : EndpointStatus::EPOLL_ERROR;
        if(result == EndpointStatus::EPOLL_ERROR)
            logger_.Error("[Epoll]: 'SendPayload -> RegisterEpoll(MOD)' failed for endpoint ", endpointIdx, ": ",
                          strerror(errno));
    }

    if(result != EndpointStatus::PENDING) {
        slotCtx->clientCtx = nullptr;
        clientCtx->endpointCtx = nullptr;

        FinalizeEndpointRequest(slotCtx, desc, false);
        Close(slotCtx, true);
    }

    return result;
}

void EpollConnectionHandler::SlotSend(EndpointCtx* slotCtx, const void* data, std::uint32_t size, AsyncData asyncData)
{
    auto& rwBuf = slotCtx->rwBuffer;

    auto fireFailure = [&]() {
        AsyncResult fail{nullptr, 0, MiddlewareAction::CONTINUE, AsyncStatus::IO_FAILURE};
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

    RegisterEpoll(slotCtx, EPOLL_CTL_MOD);
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
    RegisterEpoll(slotCtx, EPOLL_CTL_MOD);
}

// vvv Main Functions vvv
void EpollConnectionHandler::Run()
{
    WFX_TRACE();

    // Just a simple sanity check before we do anything
    if(!onReceive_)
        logger_.Fatal(
            "[Epoll]: Member 'onReceive_' was not initialized. Call 'SetEngineCallback' before calling 'Run'");

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

                    // Check limiter and try to grab a slot if its valid
                    ClientCtx* ctx = nullptr;
                    if(!ipLimiter_.AllowConnection(tmpIp) || !(ctx = GetClientConnection())) {
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
        std::uint64_t nowMs = NowMs();
        for(std::uint16_t i = 0; i < static_cast<std::uint16_t>(endpoints_.size()); i++) {
            auto& m = endpoints_[i].meta;
            if(m.dnsNextRefreshMs > 0 && nowMs >= m.dnsNextRefreshMs)
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
    metrics_->network.activeConns++;

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
    metrics_->network.activeConns++;

    // If it wraps to 0, bump it to 1 cuz 0 is reserved for identifying fds such as Listen/Timer
    if(ctx->generationId == 0)
        ctx->generationId = 1;

    return ctx;
}

void EpollConnectionHandler::ReleaseClient(ClientCtx* ctx)
{
    WFX_TRACE();

    if(!ctx)
        return;

    metrics_->network.activeConns--;

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
    // -completed. In this case, force close the endpoint connection as well
    if(ctx->endpointCtx) {
        ctx->endpointCtx->clientCtx = nullptr;
        Close(ctx->endpointCtx, true);
        ctx->endpointCtx = nullptr;
    }

    ipLimiter_.ReleaseConnection(ctx->connInfo);

    if(ctx->socket > 0)
        close(ctx->socket);

    ctx->Reset();
    connections_.FreeSlot(idx);
}

void EpollConnectionHandler::ReleaseEndpoint(EndpointCtx* ctx, DisconnectReason disconnectReason)
{
    WFX_TRACE();

    if(!ctx)
        return;

    metrics_->network.activeConns--;

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
    FinalizeEndpointRequest(ctx, desc, false);

    // Per-slot state is destroyed last
    if(ctx->slotState && desc.destroySlotState) {
        desc.destroySlotState(ctx->slotState);
        ctx->slotState = nullptr;
    }

    // Remove from coalesce map, O(n) scan on a tiny map, faster than a hash lookup here
    auto& pending = meta.coalescePending;
    for(auto it = pending.begin(); it != pending.end(); ++it) {
        if(it->second == ctx) {
            pending.erase(it);
            break;
        }
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

        HandleClientAsyncCallback(client, {nullptr, 0, MiddlewareAction::CONTINUE, AsyncStatus::IO_FAILURE}, false);
    }

    if(ctx->socket > 0)
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

    // CRITICAL: cancel timer so it cannot fire on an idle slot and double-decrement-
    // -activeConns, or worse fire on a newly-leased slot at the same pool index
    std::uint32_t timerIdx = entry.meta.timerBase + idx;
    timerWheel_.Cancel(timerIdx);

    entry.pool.FreeSlot(idx);
    metrics_->network.activeConns--;
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

bool EpollConnectionHandler::EnsureReadReady(ClientCtx* ctx)
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

bool EpollConnectionHandler::EnsureReadReady(EndpointCtx* ctx)
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

bool EpollConnectionHandler::Receive(ClientCtx* ctx)
{
    WFX_TRACE();

    if(!EnsureReadReady(ctx))
        return false;

    auto& rwBuffer = ctx->rwBuffer;
    bool gotData = false;

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
            Close(ctx);
            return false;
        }

        else {
            // Done reading for now, wait for more data in future
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                // Endpoint and client use different receive states so HandleWriteReady-
                // -and HandleEpollIn can route without an IsEndpoint() check
                ctx->eventType = EventType::EVENT_RECV;
                break;
            }

            // Fatal error
            Close(ctx);
            return false;
        }
    }

    return gotData;
}

bool EpollConnectionHandler::Receive(EndpointCtx* ctx)
{
    WFX_TRACE();

    if(!EnsureReadReady(ctx))
        return false;

    auto& rwBuffer = ctx->rwBuffer;
    bool gotData = false;

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
            Close(ctx);
            return false;
        }

        else {
            // Done reading for now, wait for more data in future
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                // Endpoint and client use different receive states so HandleWriteReady-
                // -and HandleEpollIn can route without an IsEndpoint() check
                ctx->eventType = EventType::EVENT_ENDPOINT_RECV;
                break;
            }

            // Fatal error
            Close(ctx);
            return false;
        }
    }

    return gotData;
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

    // Stuff for ease of understanding
    constexpr std::size_t chunkHeaderReserve = 10;
    auto& rwBuffer = ctx->rwBuffer;

    // Before we call stream generator, we need to reset buffer because we assume-
    // -the last time it was called, the content of it has been written of to socket
    auto writeMeta = rwBuffer.GetWriteMeta();
    if(!writeMeta) {
        Close(ctx);
        return;
    }

    writeMeta->dataLength = 0;
    writeMeta->writtenLength = 0;

    // Call stream generator function, passing in the write buffer of rwBuffer
    auto writeRegion = rwBuffer.GetWritableWriteRegion();
    if(!writeRegion.ptr || writeRegion.len == 0) {
        Close(ctx);
        return;
    }

    // The format for chunk is (if framing is true):
    // <Chunk Size in Hex> \r\n [3 - 10 bytes]
    // <Chunk> \r\n             [2 bytes]
    char* chunkPtr = !ctx->streamChunked ? writeRegion.ptr : writeRegion.ptr + chunkHeaderReserve;
    std::size_t chunkCap = !ctx->streamChunked ? writeRegion.len : writeRegion.len - chunkHeaderReserve - 2;

    auto streamResult = ctx->streamGenerator.Next(ctx->streamGenerator.ctx, {chunkPtr, chunkCap});

    // Refresh timeout everytime a chunk is sent
    RefreshExpiry(ctx, config_.networkConfig.idleTimeout);

    switch(streamResult.action) {
        case StreamAction::CONTINUE: {
            // The actual rwbuffer allows chunks only upto uint32 max only, if its 0 or > uint32 max-
            // -its an invalid / corrupted output, 'Close' connection
            if(streamResult.writtenBytes == 0 || streamResult.writtenBytes > UINT32_MAX) {
                Close(ctx);
                return;
            }

            // No need to add all the stuff, just send it as is
            if(!ctx->streamChunked) {
                writeMeta->dataLength = streamResult.writtenBytes;
                Write(ctx, {});
                return;
            }

            // Write chunk header to an intermediate buffer first
            char chunkHeader[chunkHeaderReserve + 1] = {0};
            int headerLen = snprintf(chunkHeader, chunkHeaderReserve, "%zX\r\n", streamResult.writtenBytes);
            if(headerLen <= 0 || headerLen >= static_cast<int>(chunkHeaderReserve)) {
                Close(ctx);
                return;
            }

            // Manually set the amount of bytes that were reserved + written to the buffer
            // Reason being, we artifically advance write pointer below, for that the data length-
            // -needs to show the total size of buffer from start to end even if we skipped bytes
            writeMeta->dataLength = chunkHeaderReserve + streamResult.writtenBytes + 2;

            // So we don't want to leave space between header and chunk start
            // So write header in reverse order from chunk start going back
            // Also move the internal write pointer of write buffer to point to the header start
            // So 'Write' will pickup from where write pointer left off
            std::memcpy(chunkPtr - headerLen, chunkHeader, headerLen);
            rwBuffer.AdvanceWriteLength(chunkHeaderReserve - headerLen);

            // Append CRLF after data
            char* trailer = chunkPtr + streamResult.writtenBytes;
            *trailer++ = '\r';
            *trailer++ = '\n';

            Write(ctx, {});
            return;
        }
        // Just resume the connection, we are done streaming
        case StreamAction::STOP_AND_ALIVE_CONN:
            ctx->SetConnectionState(ConnectionState::CONNECTION_ALIVE);
            break;

        // Do not resume connection, close it
        case StreamAction::STOP_AND_CLOSE_CONN:
        default:
            ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
            break;
    }

    // Call destructor if one is present
    if(ctx->streamGenerator.Destroy)
        ctx->streamGenerator.Destroy(ctx->streamGenerator.ctx);

    // Storing value before resetting it below
    bool wasChunked = static_cast<bool>(ctx->streamChunked);

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
    // TODO: properly handle read return value
    std::uint64_t expirations = 0;
    (void)read(sfd, &expirations, sizeof(expirations));

    timerWheel_.Tick(NowMs() / 1000);
}

void EpollConnectionHandler::HandleAsyncTimer(int sfd)
{
    std::uint64_t expirations = 0;
    (void)read(sfd, &expirations, sizeof(expirations));

    std::uint64_t newTick = NowMs();
    std::uint64_t connId = 0;

    while(timerHeap_.PopExpired(newTick, connId)) {
        ClientCtx* ctx = connections_.GetPtr(connId);
        ctx->isAsyncTimerOperation = 0;

        HandleClientAsyncCallback(ctx, {nullptr, 0, MiddlewareAction::CONTINUE, AsyncStatus::COMPLETED}, false);
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
        Close(ctx);
        return;
    }

    // Wait for handshake to finish
    if(ctx->eventType == EventType::EVENT_ENDPOINT_HANDSHAKE)
        return;

    // Its alive, ITS ALIVE. ITS ALIVEEEUEEUUEEEE. IN THE NAME OF GOD, NOW I KNOW WHAT IT FEELS LIKE TO BE GOD
    // - Frankenstein
    ctx->SetConnectionState(ConnectionState::CONNECTION_ALIVE);

    if(ctx->eventType == EventType::EVENT_ENDPOINT_ONCONNECT)
        FireOnConnect(ctx, endpoints_[ctx->endpointIdx]);
    else
        Write(ctx);
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
        case EventType::EVENT_ENDPOINT_RECV:
            // Data arrived from backend, run the parse loop
            if(Receive(ctx))
                HandleEndpointReceive(ctx);

            return;

        case EventType::EVENT_ENDPOINT_ONCONNECT:
            // onConnect coroutine called SlotReceive and is now suspended waiting for data
            // Wake it by firing its asyncData completion with the buffer contents
            if(Receive(ctx))
                HandleEndpointAsyncCallback(ctx,
                                            {ctx->rwBuffer.GetReadData(), ctx->rwBuffer.GetReadMeta()->dataLength,
                                             MiddlewareAction::CONTINUE, AsyncStatus::COMPLETED},
                                            false);
            return;

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
        case EventType::EVENT_ENDPOINT_ONCONNECT:
            FireOnConnect(ctx, endpoints_[ctx->endpointIdx]);
            break;

        // TCP connect completed, proceed to SSL handshake or directly to write/onConnect
        case EventType::EVENT_CONNECT: {
            int err = 0;
            socklen_t len = sizeof(err);

            auto& meta = endpoints_[ctx->endpointIdx].meta;

            if(getsockopt(ctx->socket, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
                logger_.Error("[Epoll]: Connect failed for endpoint '", meta.hostname, "': ", strerror(err));
                Close(ctx);
                break;
            }

            ctx->SetConnectionState(ConnectionState::CONNECTION_ALIVE);

            // TLS required, try SSL handshake
            if(ctx->GetEndpointState() == EndpointState::ENDPOINT_SECURE) {
                ctx->sslConn = sslHandler_->WrapClient(ctx->socket, meta.hostname.c_str());

                if(!ctx->sslConn) {
                    Close(ctx);
                    break;
                }

                HandleEndpointHandshake(ctx, ev);
                break;
            }

            // Plain TCP, check for onConnect hook before writing
            if(meta.desc.onConnect) {
                FireOnConnect(ctx, endpoints_[ctx->endpointIdx]);
                break;
            }

            Write(ctx);
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

    while(timerfd_settime(asyncTimerFd_, 0, &ts, nullptr) < 0) {
        if(errno == EINTR)
            continue;

        logger_.Error("[Epoll]: Failed to set async timer: ", strerror(errno));
        break;
    }
}

void EpollConnectionHandler::FinalizeEndpointRequest(EndpointCtx* ctx, EndpointDesc& desc, bool success)
{
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
}

void EpollConnectionHandler::HandleEndpointWriteComplete(EndpointCtx* slotCtx)
{
    WFX_TRACE();

    // If asyncData is set, this write was initiated by SlotSend from inside onConnect
    // Fire the SlotSend completion and stay in the onConnect phase
    if(slotCtx->asyncData.AsyncComplete) {
        HandleEndpointAsyncCallback(slotCtx, {nullptr, 0, MiddlewareAction::CONTINUE, AsyncStatus::COMPLETED}, false);
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

void EpollConnectionHandler::HandleEndpointReceive(EndpointCtx* slotCtx)
{
    WFX_TRACE();

    auto& entry = endpoints_[slotCtx->endpointIdx];
    auto& desc = entry.meta.desc;
    auto& rwBuf = slotCtx->rwBuffer;

    while(true) {
        std::uint32_t consumed = 0;
        ParseResult pr = desc.parse(slotCtx->slotState, slotCtx->parseStateObj, rwBuf.GetReadData(),
                                    rwBuf.GetReadMeta()->dataLength, &consumed, slotCtx->outputObj);

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

        switch(pr) {
            case ParseResult::INCOMPLETE:
                // Need more bytes. eventType is already EVENT_ENDPOINT_RECV from Receive()
                return;

            case ParseResult::COMPLETE_KEEP_ALIVE:
            case ParseResult::COMPLETE_CLOSE: {
                ClientCtx* clientCtx = slotCtx->clientCtx;
                void* outputObj = slotCtx->outputObj;

                // Remove from coalesce map
                auto& pending = entry.meta.coalescePending;
                for(auto it = pending.begin(); it != pending.end(); ++it) {
                    if(it->second == slotCtx) {
                        pending.erase(it);
                        break;
                    }
                }

                slotCtx->clientCtx = nullptr;
                clientCtx->endpointCtx = nullptr;
                slotCtx->outputObj = nullptr; // disown before any cleanup

                if(pr == ParseResult::COMPLETE_KEEP_ALIVE) {
                    // Only reset parse state, do NOT touch outputObj here
                    if(slotCtx->parseStateObj && desc.resetParseState)
                        desc.resetParseState(slotCtx->parseStateObj);

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

                // Fire callback. Coroutine gets valid outputObj pointer
                HandleClientAsyncCallback(clientCtx, {outputObj, 0, MiddlewareAction::CONTINUE, AsyncStatus::COMPLETED},
                                          false);

                // NOW destroy output. Coroutine has already run and is done with outputObj
                if(outputObj && desc.destroyOutput)
                    desc.destroyOutput(outputObj);

                return;
            }

            case ParseResult::ERROR:
            default: {
                logger_.Error("[Epoll]: Parse error for endpoint '", entry.meta.hostname, "' consumed=", consumed,
                              " dataLength=", rwBuf.GetReadMeta()->dataLength);

                if(slotCtx->clientCtx) {
                    ClientCtx* clientCtx = slotCtx->clientCtx;
                    slotCtx->clientCtx = nullptr;
                    clientCtx->endpointCtx = nullptr;

                    HandleClientAsyncCallback(clientCtx,
                                              {nullptr, 0, MiddlewareAction::CONTINUE, AsyncStatus::IO_FAILURE}, false);
                }

                FinalizeEndpointRequest(slotCtx, desc, false);
                Close(slotCtx, true);

                return;
            }
        }
    }
}

void EpollConnectionHandler::FireOnConnect(EndpointCtx* slotCtx, EndpointEntry& entry)
{
    auto& desc = entry.meta.desc;

    // Send / Receive go through the endpoint API so the engine intercepts them
    // Close is nullptr. Slot lifetime is managed by the engine via ConnectResult
    EndpointSlotHandle handle{};
    handle.impl = slotCtx;
    handle.Send = [](void* impl, const void* data, std::uint32_t size, AsyncData asyncData) -> SlotSendResult {
        Shared::GetEndpointAPIExt1()->SlotSend(impl, data, size, asyncData);
        return SlotSendResult::PENDING;
    };
    handle.Receive = Shared::GetEndpointAPIExt1()->SlotReceive;
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

void EpollConnectionHandler::OnSlotConnected(void* ud, AsyncResult result)
{
    auto* slotCtx = static_cast<EndpointCtx*>(ud);
    auto& entry = instance_->endpoints_[slotCtx->endpointIdx];
    auto& desc = entry.meta.desc;

    slotCtx->inOnConnectPhase = 0;

    // Any failure. Call onDisconnect, clean up, close slot
    if(result.status != AsyncStatus::COMPLETED || result.connectResult == ConnectResult::FATAL ||
       result.connectResult == ConnectResult::RETRY) {
        if(desc.onDisconnect && slotCtx->slotState)
            desc.onDisconnect(slotCtx->slotState, DisconnectReason::ERROR);

        instance_->FinalizeEndpointRequest(slotCtx, desc, false);
        instance_->Close(slotCtx, true);

        // TODO: RETRY. Schedule reconnect with exponential backoff
        return;
    }

    // onConnect phase is done. The read buffer may contain protocol handshake data-
    // -(auth packets, server greeting, etc.) consumed by the coroutine via SlotReceive
    // That data must not bleed into the response parse loop, so clear it before the-
    // -slot enters normal request/response operation
    slotCtx->rwBuffer.ClearReadBuffer();

    // ConnectResult::READY, slot is pooled and ready
    if(slotCtx->clientCtx) {
        // Non-prewarm: SendPayload already serialized a request into the write buffer
        // Drive the write directly instead of calling RegisterEpoll(MOD) and waiting-
        // -for EPOLLOUT, because in ET mode the EPOLLOUT edge was consumed when EVENT_CONNECT-
        // -fired and a MOD on an already-writable fd does not generate a new edge
        slotCtx->eventType = EventType::EVENT_ENDPOINT_SEND;
        instance_->Write(slotCtx);
    }
    else {
        // Prewarm: no client waiting, no data in the write buffer. Re-arm so future-
        // -SendPayload can lease this slot and the next MOD triggers EPOLLOUT correctly
        slotCtx->eventType = EventType::EVENT_ENDPOINT_RECV;
        instance_->RegisterEpoll(slotCtx, EPOLL_CTL_MOD);
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
            if(result != EndpointStatus::PENDING) {
                if(desc.destroySlotState && slotCtx->slotState) {
                    desc.destroySlotState(slotCtx->slotState);
                    slotCtx->slotState = nullptr;
                }

                ReleaseEndpoint(slotCtx);
            }
        }
    }
}

void EpollConnectionHandler::HandleDnsRefresh(std::uint16_t endpointIdx)
{
    auto& meta = endpoints_[endpointIdx].meta;

    // Reschedule to prevent tight loop while the real implementation is pending
    std::uint64_t interval =
        meta.config.dnsRefreshSeconds > 0 ? static_cast<std::uint64_t>(meta.config.dnsRefreshSeconds) * 1000 : 30000;

    meta.dnsNextRefreshMs = NowMs() + interval;

    // TODO: spawn std::thread, call getaddrinfo blocking, post result via eventfd
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

bool EpollConnectionHandler::RegisterEpoll(ClientCtx* ctx, int op)
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
    while(true) {
        if(epoll_ctl(epollFd_, op, ctx->socket, evPtr) == 0)
            return true;

        if(errno == EINTR)
            continue;

        return false;
    }
}

bool EpollConnectionHandler::RegisterEpoll(EndpointCtx* ctx, int op)
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
    while(true) {
        if(epoll_ctl(epollFd_, op, ctx->socket, evPtr) == 0)
            return true;

        if(errno == EINTR)
            continue;

        return false;
    }
}

bool EpollConnectionHandler::CreateAndConnect(EndpointCtx* ctx, EndpointMetadata& epMeta)
{
    ctx->socket = socket(epMeta.addr.ss_family, SOCK_STREAM, 0);
    if(ctx->socket < 0)
        return false;

    if(!SetNonBlocking(ctx->socket))
        return false;

    while(true) {
        int ret = connect(ctx->socket, reinterpret_cast<const sockaddr*>(&epMeta.addr), epMeta.addrLen);
        if(ret == 0)
            return true;

        if(errno == EINTR)
            continue;

        if(errno == EINPROGRESS) {
            ctx->eventType = EventType::EVENT_CONNECT;
            return true;
        }

        return false;
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

    if(!CreateAndConnect(ctx, meta))
        return EndpointStatus::CONNECT_FAILURE;

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
            ctx->sslConn = sslHandler_->WrapClient(ctx->socket, meta.hostname.c_str());
            if(!ctx->sslConn) {
                logger_.Error("[Epoll]: 'WrapClient' failed for endpoint '", meta.hostname, "'");
                return EndpointStatus::SSL_FAILURE;
            }

            if(!TryHandshake(ctx, EventType::EVENT_ENDPOINT_SEND, EventType::EVENT_ENDPOINT_HANDSHAKE))
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

    RefreshExpiry(ctx, meta.config.idleTimeoutSeconds);
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