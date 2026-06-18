#include "kqueue_connection.hpp"

#include "http/common/http_error_msgs.hpp"
#include "http/ssl/http_ssl_factory.hpp"
#include "shared/apis/http_api.hpp"
#include "utils/diagnostics/crash_tracer.hpp"

#include <sys/event.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h> // macOS sendfile is declared here
#include <fcntl.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h>
#include <algorithm>
#include <vector>

namespace WFX::OSSpecific {

// vvv Constructor & Destructor vvv
KqueueConnectionHandler::KqueueConnectionHandler(bool useHttps) : useHttps_(useHttps)
{
    if(useHttps)
        sslHandler_ = CreateSSLHandler();
}

KqueueConnectionHandler::~KqueueConnectionHandler()
{
    if(listenFd_ > 0) {
        close(listenFd_);
        listenFd_ = -1;
    }
    if(kqFd_ > 0) {
        close(kqFd_);
        kqFd_ = -1;
    }

    logger_.Info("[Kqueue]: Cleaned up resources successfully");
}

// vvv Initializing Functions vvv
void KqueueConnectionHandler::Initialize(const std::string& host, std::uint16_t port)
{
    WFX_TRACE();

    metrics_ = MetricTracer::Current();
    if(!metrics_)
        logger_.Fatal("[Kqueue]: MetricTracer not initialized in worker");

    auto& osConfig = config_.osSpecificConfig;

    events_ = std::make_unique<struct kevent[]>(maxEvents_);

    sockaddr_storage addr{};
    socklen_t addrLen = 0;

    char portStr[6];
    auto [ptr, err] = std::to_chars(portStr, portStr + sizeof(portStr), port);
    if(err != std::errc{})
        logger_.Fatal("[Kqueue]: Failed to convert port to string");

    *ptr = '\0';

    if(!ResolveHost(host.c_str(), portStr, &addr, &addrLen))
        logger_.Fatal("[Kqueue]: Failed to resolve host '", host, '\'');

    listenFd_ = socket(addr.ss_family, SOCK_STREAM, 0);
    if(listenFd_ < 0)
        logger_.Fatal("[Kqueue]: Failed to create listening socket: ", strerror(errno));

    if(addr.ss_family == AF_INET6) {
        int no = 0;
        if(setsockopt(listenFd_, IPPROTO_IPV6, IPV6_V6ONLY, (void*)&no, sizeof(no)) < 0)
            logger_.Fatal("[Kqueue]: Failed to disable IPV6_V6ONLY: ", strerror(errno));
    }

    int opt = 1;
    if(setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        logger_.Fatal("[Kqueue]: Failed to set SO_REUSEADDR: ", strerror(errno));

    if(setsockopt(listenFd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0)
        logger_.Fatal("[Kqueue]: Failed to set SO_REUSEPORT: ", strerror(errno));

    if(!SetNonBlocking(listenFd_))
        logger_.Fatal("[Kqueue]: Failed to make listening socket non-blocking: ", strerror(errno));

    if(bind(listenFd_, (sockaddr*)&addr, addrLen) < 0)
        logger_.Fatal("[Kqueue]: Failed to bind socket: ", strerror(errno));

    if(listen(listenFd_, osConfig.backlog) < 0)
        logger_.Fatal("[Kqueue]: Failed to listen: ", strerror(errno));

    kqFd_ = kqueue();
    if(kqFd_ < 0)
        logger_.Fatal("[Kqueue]: Failed to create kqueue: ", strerror(errno));

    struct kevent listenEv;
    EV_SET(&listenEv, listenFd_, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if(kevent(kqFd_, &listenEv, 1, nullptr, 0, nullptr) < 0)
        logger_.Fatal("[Kqueue]: Failed to register listening socket: ", strerror(errno));

    timerWheel_.Init(connections_.GetSlots(), 4096, 1, TimeUnit::SECONDS,
                     [this](std::uint32_t connId, std::uint32_t extra) {
                         ConnectionContext* ctx = nullptr;

                         // extra is either the client tag or the endpoint index.
                         if(extra >= CLIENT_CONNECTION_TAG)
                             ctx = connections_.GetPtr(connId);
                         else
                             ctx = endpoints_[extra].second.GetPtr(connId);

                         if(ctx->GetConnectionState() != ConnectionState::CONNECTION_CLOSE ||
                            (ctx->IsEndpoint() || ctx->IsAsyncOperation()))
                             Close(ctx, true);
                     });

    struct kevent timeoutKev;
    EV_SET(&timeoutKev, TIMEOUT_TIMER_IDENT, EVFILT_TIMER, EV_ADD | EV_ENABLE, NOTE_SECONDS,
           (intptr_t)INVOKE_TIMEOUT_COOLDOWN, nullptr);
    if(kevent(kqFd_, &timeoutKev, 1, nullptr, 0, nullptr) < 0)
        logger_.Fatal("[Kqueue]: Failed to register timeout timer: ", strerror(errno));
}

void KqueueConnectionHandler::SetEngineCallback(ReceiveCallback onData)
{
    onReceive_ = std::move(onData);
}

std::uint16_t KqueueConnectionHandler::AllocateEndpoint(std::string_view host, std::string_view port,
                                                        std::uint32_t cLimit, std::uint32_t ifLimit, bool useTLS)
{
    (void)ifLimit;

    if(endpoints_.size() > MAX_DISTINCT_ENDPOINTS)
        logger_.Fatal("[Kqueue]: Too many distinct domain endpoints registered");

    auto& endpointSlot =
        endpoints_.emplace_back(std::piecewise_construct, std::forward_as_tuple(), std::forward_as_tuple(cLimit));
    activeEndpoints_.emplace_back();

    auto& endpointInfo = endpointSlot.first;
    auto& endpointPool = endpointSlot.second;

    std::uint16_t endpointIdx = endpoints_.size() - 1;

    for(std::uint32_t j = 0; j < endpointPool.GetSlots(); j++) {
        auto* ctx = endpointPool.GetPtr(j);
        ctx->endpointIdx = endpointIdx;
        ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
        ctx->SetEndpointState(useTLS ? EndpointState::ENDPOINT_SECURE : EndpointState::ENDPOINT_INSECURE);
    }

    std::string tempHost = std::string(host);
    std::string tempPort = std::string(port);

    if(!ResolveHost(tempHost.c_str(), tempPort.c_str(), &endpointInfo.addr, &endpointInfo.addrLen))
        logger_.Fatal("[Kqueue]: Failed to resolve endpoint URL: ", host, ':', port);

    endpointInfo.host = std::move(tempHost);

    return endpointIdx;
}

// vvv I/O Operations vvv
void KqueueConnectionHandler::ResumeReceive(ConnectionContext* ctx)
{
    if(!EnsureReadReady(ctx))
        return;

    ctx->eventType = EventType::EVENT_RECV;
}

void KqueueConnectionHandler::Write(ConnectionContext* ctx, std::string_view msg)
{
    WFX_TRACE();

    if(!msg.empty()) {
        std::size_t sent = 0;
        while(sent < msg.size()) {
            ssize_t n = WrapWrite(ctx, msg.data() + sent, msg.size() - sent);
            if(n > 0)
                sent += static_cast<std::size_t>(n);
            else if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                auto& netCfg = config_.networkConfig;
                if(!ctx->rwBuffer.IsWriteInitialized() && !ctx->rwBuffer.InitWriteBuffer(netCfg.maxSendBufferSize)) {
                    goto __CloseConnection;
                }

                // Socket buffer is full. Store only the unsent tail and wait for EVFILT_WRITE.
                if(!ctx->rwBuffer.AppendWriteData(msg.data() + sent, static_cast<std::uint32_t>(msg.size() - sent),
                                                  netCfg.sendBufferIncSize, netCfg.maxSendBufferSize)) {
                    goto __CloseConnection;
                }
                ctx->eventType = EventType::EVENT_SEND;
                if(!RegisterKqueue(ctx, KQ_MOD))
                    goto __CloseConnection;
                return;
            }
            else
                goto __CloseConnection;
        }
        goto __CleanupOrRearm;
    }
    else {
        auto* writeMeta = ctx->rwBuffer.GetWriteMeta();
        if(!writeMeta || writeMeta->writtenLength >= writeMeta->dataLength)
            goto __CleanupOrRearm;

        // Empty msg means resume a previous buffered write.
        while(writeMeta->writtenLength < writeMeta->dataLength) {
            const char* buf = ctx->rwBuffer.GetWriteData() + writeMeta->writtenLength;
            std::size_t remaining = writeMeta->dataLength - writeMeta->writtenLength;

            ssize_t n = WrapWrite(ctx, buf, remaining);

            if(n > 0)
                writeMeta->writtenLength += n;
            else if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                ctx->eventType = EventType::EVENT_SEND;
                if(!RegisterKqueue(ctx, KQ_MOD))
                    goto __CloseConnection;
                return;
            }
            else
                goto __CloseConnection;
        }
    }

__CleanupOrRearm:
    if(ctx->isStreamOperation) {
        ResumeStream(ctx);
        return;
    }

    if(ctx->isFileOperation) {
        SendFile(ctx);
        return;
    }

    if(ctx->GetConnectionState() == ConnectionState::CONNECTION_CLOSE) {
    __CloseConnection:
        Close(ctx);
    }
    else
        FinishWriteCycle(ctx);
}

void KqueueConnectionHandler::FinishWriteCycle(ConnectionContext* ctx)
{
    // Drop write interest after flushing, otherwise kqueue keeps waking us up
    // just because most sockets are writable most of the time.
    (void)RegisterKqueue(ctx, KQ_DROP_WRITE);
    ctx->ClearContext();
    ResumeReceive(ctx);
    Receive(ctx);
}

void KqueueConnectionHandler::WriteFile(ConnectionContext* ctx, std::string path)
{
    if(!EnsureFileReady(ctx, std::move(path))) {
        ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
        Write(ctx, HttpError::internalError);
        return;
    }

    ctx->isFileOperation = 1;
    Write(ctx, {});
}

EndpointStatus KqueueConnectionHandler::WriteEndpoint(ConnectionContext* ctx, std::uint32_t endpointIndex,
                                                      const std::byte* ptr, std::uint32_t size)
{
    if(endpointIndex > endpoints_.size() - 1)
        return EndpointStatus::INVALID_KEY;

    auto* allocatedCtx = GetConnection(endpointIndex);
    if(!allocatedCtx)
        return EndpointStatus::POOL_EXHAUSTED;

    auto& endpointRWBuffer = allocatedCtx->rwBuffer;
    if(!endpointRWBuffer.IsWriteInitialized() &&
       !endpointRWBuffer.InitWriteBuffer(config_.networkConfig.maxSendBufferSize)) {
        ReleaseConnection(allocatedCtx, true);
        return EndpointStatus::BUFFER_ERROR;
    }

    if(!endpointRWBuffer.AppendWriteData(reinterpret_cast<const char*>(ptr), size,
                                         config_.networkConfig.sendBufferIncSize,
                                         config_.networkConfig.maxSendBufferSize)) {
        ReleaseConnection(allocatedCtx, true);
        return EndpointStatus::INSUFFICIENT_BUFFER;
    }

    allocatedCtx->clientContext = ctx;
    ctx->endpointContext = allocatedCtx;

    EndpointStatus result;

    if(allocatedCtx->GetConnectionState() == ConnectionState::CONNECTION_CLOSE)
        result = WrapConnect(allocatedCtx, endpoints_[endpointIndex]);
    else {
        allocatedCtx->eventType = EventType::EVENT_SEND;
        result = RegisterKqueue(allocatedCtx, KQ_MOD) ? EndpointStatus::PENDING : EndpointStatus::INTERNAL_ERROR;
    }

    if(result != EndpointStatus::PENDING) {
        allocatedCtx->clientContext = nullptr;
        ctx->endpointContext = nullptr;
        Close(allocatedCtx, true);
    }

    return result;
}

void KqueueConnectionHandler::Stream(ConnectionContext* ctx, StreamGenerator generator, bool streamChunked)
{
    if(!generator.ctx || !generator.Next) {
        logger_.Error("[Kqueue]: 'Stream()' called but received empty generator");
        Close(ctx);
        return;
    }

    ctx->streamGenerator = generator;
    ctx->isStreamOperation = 1;
    ctx->streamChunked = streamChunked;
    Write(ctx, {});
}

void KqueueConnectionHandler::Close(ConnectionContext* ctx, bool forceClose)
{
    WFX_TRACE();

    if(!ctx)
        return;

    if(!forceClose && ctx->isShuttingDown)
        return;

    ctx->isShuttingDown = 1;

    if(ctx->sslConn) {
        if(forceClose) {
            sslHandler_->ForceShutdown(ctx->sslConn);
            ctx->sslConn = nullptr;
        }
        else {
            auto res = sslHandler_->Shutdown(ctx->sslConn);

            if(res == SSLReturn::SUCCESS || res == SSLReturn::FATAL)
                ctx->sslConn = nullptr;
            else {
                ctx->eventType = EventType::EVENT_SHUTDOWN;
                return;
            }
        }
    }

    (void)RegisterKqueue(ctx, KQ_DEL);
    ReleaseConnection(ctx);
}

// vvv Main Event Loop vvv
void KqueueConnectionHandler::Run()
{
    WFX_TRACE();

    if(!onReceive_)
        logger_.Fatal(
            "[Kqueue]: Member 'onReceive_' was not initialized. Call 'SetEngineCallback' before calling 'Run'");

    while(running_) {
        int nfds = kevent(kqFd_, nullptr, 0, events_.get(), maxEvents_, nullptr);
        if(nfds < 0) {
            if(errno == EINTR)
                continue;
            break;
        }

        bool listenReady = false;

        for(int i = 0; i < nfds; i++) {
            auto& ev = events_[i];

            // kqueue timer events don't use udata for identification
            if(ev.filter == EVFILT_TIMER) {
                if(ev.ident == TIMEOUT_TIMER_IDENT)
                    HandleTimeoutTimer();
                else if(ev.ident == ASYNC_TIMER_IDENT)
                    HandleAsyncTimer();
                continue;
            }

            std::uint64_t udata = (std::uint64_t)(uintptr_t)ev.udata;
            std::uint16_t gen = (udata >> 32) & 0xFFFF;

            // Listener/timer events don't point to a ConnectionContext.
            if(gen == 0) {
                if((uintptr_t)ev.ident == (uintptr_t)listenFd_ && ev.filter == EVFILT_READ)
                    listenReady = true;
                continue;
            }

            // udata packs enough info to find the context without fd->ctx maps.
            std::uint16_t endpointIdx = udata >> 48;
            std::uint32_t poolIdx = udata & 0xFFFFFFFF;

            ConnectionContext* ctx = nullptr;

            if(endpointIdx == CLIENT_CONNECTION_TAG)
                ctx = connections_.GetPtr(poolIdx);
            else
                ctx = endpoints_[endpointIdx].second.GetPtr(poolIdx);

            if(ctx->generationId != gen)
                continue;

            // System-level error on this fd
            if(ev.flags & EV_ERROR) {
                Close(ctx);
                continue;
            }

            // SSL handshake in progress
            if(ctx->eventType == EventType::EVENT_HANDSHAKE) {
                HandleHandshake(ctx, ev.filter);
                continue;
            }

            // SSL shutdown in progress
            if(ctx->eventType == EventType::EVENT_SHUTDOWN) {
                auto res = sslHandler_->Shutdown(ctx->sslConn);

                switch(res) {
                    case SSLReturn::WANT_READ:
                    case SSLReturn::WANT_WRITE:
                        break;
                    default:
                        ctx->sslConn = nullptr;
                        (void)RegisterKqueue(ctx, KQ_DEL);
                        ReleaseConnection(ctx);
                        break;
                }
                continue;
            }

            // Readable event
            if(ev.filter == EVFILT_READ) {
                if(ev.flags & (EV_EOF | EV_ERROR)) {
                    Receive(ctx);
                    continue;
                }
                if(ctx->eventType == EventType::EVENT_RECV) {
                    if(!ipLimiter_.AllowRequest(ctx->connInfo)) {
                        ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
                        Write(ctx, HttpError::tooManyRequests);
                    }
                    else
                        Receive(ctx);
                }
            }

            // Writable event
            if(ev.filter == EVFILT_WRITE) {
                // EV_EOF on write: remote reset the connection
                if(ev.flags & EV_EOF) {
                    Close(ctx);
                    continue;
                }
                HandleWriteReady(ctx, ev.filter);
            }
        }

        if(listenReady) {
            while(true) {
                sockaddr_storage addr{};
                socklen_t len = sizeof(addr);

                // Edge-triggered listener: accept until the queue is empty.
                int clientFd = accept(listenFd_, (sockaddr*)&addr, &len);
                if(clientFd < 0) {
                    if(errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    else
                        continue;
                }

                if(!SetNonBlocking(clientFd)) {
                    close(clientFd);
                    continue;
                }

                if(!SetNoSigPipe(clientFd)) {
                    close(clientFd);
                    continue;
                }

                int nodelay = 1;
                (void)setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

                WFXIpAddress tmpIp;
                if(!ResolveIP(addr, tmpIp)) {
                    close(clientFd);
                    continue;
                }

                if(!ipLimiter_.AllowConnection(tmpIp)) {
                    close(clientFd);
                    continue;
                }

                ConnectionContext* ctx = EnsureAcceptSlot();
                if(!ctx) {
                    ipLimiter_.ReleaseConnection(tmpIp);
                    close(clientFd);
                    continue;
                }

                ctx->socket = clientFd;
                ctx->connInfo = tmpIp;

                WrapAccept(ctx);
            }
        }
    }

    DrainAllConnections();
}

ConnectionContext* KqueueConnectionHandler::EnsureAcceptSlot()
{
    if(ConnectionContext* ctx = GetConnection())
        return ctx;

    return nullptr;
}

void KqueueConnectionHandler::DrainAllConnections()
{
    std::vector<ConnectionContext*> pending;
    pending.reserve(activeConnections_.size());

    // Copy first; Close() mutates the active lists while releasing slots.
    pending.insert(pending.end(), activeConnections_.begin(), activeConnections_.end());

    for(auto& activeEndpoint : activeEndpoints_)
        pending.insert(pending.end(), activeEndpoint.begin(), activeEndpoint.end());

    for(ConnectionContext* ctx : pending)
        Close(ctx, true);
}

void KqueueConnectionHandler::TrackConnection(ConnectionContext* ctx)
{
    // BitmapPool intentionally stays simple, so kqueue tracks active slots
    // itself for shutdown cleanup.
    if(ctx->IsEndpoint())
        activeEndpoints_[ctx->endpointIdx].push_back(ctx);
    else
        activeConnections_.push_back(ctx);
}

void KqueueConnectionHandler::UntrackConnection(ConnectionContext* ctx)
{
    auto& active = ctx->IsEndpoint() ? activeEndpoints_[ctx->endpointIdx] : activeConnections_;
    auto it = std::find(active.begin(), active.end(), ctx);
    if(it != active.end())
        active.erase(it);
}

void KqueueConnectionHandler::RefreshExpiry(ConnectionContext* ctx, std::uint16_t timeoutSeconds)
{
    ConnectionPool* pool = nullptr;
    std::uint32_t extra = CLIENT_CONNECTION_TAG;

    if(ctx->IsEndpoint()) {
        extra = ctx->endpointIdx;
        pool = &endpoints_[extra].second;
    }
    else
        pool = &connections_;

    std::uint32_t idx = pool->GetIndex(ctx);
    timerWheel_.Schedule(idx, extra, timeoutSeconds);
}

bool KqueueConnectionHandler::RefreshAsyncTimer(ConnectionContext* ctx, std::uint32_t delayMs, AsyncData asyncData)
{
    std::uint32_t idx = connections_.GetIndex(ctx);
    std::uint64_t expire = NowMs() + delayMs;

    if(!timerHeap_.Insert(idx, expire, 10)) {
        logger_.Warn("[Kqueue]: Failed to refresh async timer");
        return false;
    }

    ctx->isAsyncTimerOperation = 1;
    ctx->asyncData = asyncData;

    UpdateAsyncTimer();

    return true;
}

void KqueueConnectionHandler::Stop()
{
    running_ = false;
}

// vvv Helper Functions vvv
ConnectionContext* KqueueConnectionHandler::GetConnection(std::uint16_t endpointIndex)
{
    WFX_TRACE();

    ConnectionContext* ctx = nullptr;

    if(endpointIndex == CLIENT_CONNECTION_TAG)
        ctx = connections_.AllocSlot();
    else
        ctx = endpoints_[endpointIndex].second.AllocSlot();

    if(!ctx)
        return nullptr;

    ctx->generationId++;
    metrics_->network.activeConns++;

    if(ctx->generationId == 0)
        ctx->generationId = 1;

    TrackConnection(ctx);

    return ctx;
}

void KqueueConnectionHandler::ReleaseConnection(ConnectionContext* ctx, bool freeOnly)
{
    WFX_TRACE();

    if(!ctx)
        return;

    metrics_->network.activeConns--;

    auto& pool = ctx->IsEndpoint() ? endpoints_[ctx->endpointIdx].second : connections_;
    std::uint32_t idx = pool.GetIndex(ctx);
    UntrackConnection(ctx);

    // Some paths allocate a slot but fail before the fd/timers/API callbacks
    // are fully wired. In that case we only need to return the pool slot.
    if(freeOnly)
        goto __FreeContext;

    timerWheel_.Cancel(idx);

    if(!ctx->IsEndpoint()) {
        if(ctx->isAsyncTimerOperation) {
            if(timerHeap_.Remove(idx))
                UpdateAsyncTimer();
        }

        HandleAsyncCallback(ctx, {}, true);

        if(ctx->endpointContext)
            Close(ctx->endpointContext, true);

        ipLimiter_.ReleaseConnection(ctx->connInfo);
    }
    else if(ctx->clientContext)
        HandleAsyncCallback(ctx->clientContext, {nullptr, 0, MiddlewareAction::CONTINUE, AsyncStatus::IO_FAILURE},
                            false);

    if(ctx->socket > 0)
        close(ctx->socket);

__FreeContext:
    ctx->ResetContext();
    pool.FreeSlot(idx);
}

std::uint64_t KqueueConnectionHandler::NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - startTime_).count();
}

bool KqueueConnectionHandler::SetNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags < 0)
        return false;

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool KqueueConnectionHandler::SetNoSigPipe(int fd)
{
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt)) == 0;
}

bool KqueueConnectionHandler::EnsureFileReady(ConnectionContext* ctx, std::string path)
{
    auto [fd, size] = fileCache_.GetFileDesc(std::move(path));
    if(fd < 0)
        return false;

    ctx->fileInfo.fd = fd;
    ctx->fileInfo.offset = 0;
    ctx->fileInfo.fileSize = size;

    return true;
}

bool KqueueConnectionHandler::EnsureReadReady(ConnectionContext* ctx)
{
    auto& rwBuffer = ctx->rwBuffer;
    auto& netCfg = config_.networkConfig;

    if(rwBuffer.IsReadInitialized())
        return true;

    if(!rwBuffer.InitReadBuffer(netCfg.readBufferIncSize)) {
        logger_.Error("[Kqueue]: Failed to init read buffer");
        Close(ctx);
        return false;
    }
    return true;
}

bool KqueueConnectionHandler::ResolveHost(const char* host, const char* port, sockaddr_storage* outAddr,
                                          socklen_t* outLen)
{
    addrinfo hints = {0};
    addrinfo* res = nullptr;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;

    int ret = getaddrinfo(host, port, &hints, &res);
    if(ret != 0)
        return false;

    bool found = false;
    if(res != nullptr) {
        if(res->ai_addrlen <= sizeof(sockaddr_storage)) {
            memcpy(outAddr, res->ai_addr, res->ai_addrlen);
            if(outLen)
                *outLen = res->ai_addrlen;
            found = true;
        }
    }

    freeaddrinfo(res);
    return found;
}

bool KqueueConnectionHandler::ResolveIP(const sockaddr_storage& addr, WFXIpAddress& out)
{
    const sockaddr* sa = reinterpret_cast<const sockaddr*>(&addr);

    switch(sa->sa_family) {
        case AF_INET: {
            const auto* v4 = reinterpret_cast<const sockaddr_in*>(sa);
            out.ip.v4 = v4->sin_addr;
            out.type = AF_INET;
            return true;
        }
        case AF_INET6: {
            const auto* v6 = reinterpret_cast<const sockaddr_in6*>(sa);
            out.ip.v6 = v6->sin6_addr;
            out.type = AF_INET6;
            return true;
        }
        default:
            return false;
    }
}

void KqueueConnectionHandler::Receive(ConnectionContext* ctx)
{
    WFX_TRACE();

    if(!EnsureReadReady(ctx))
        return;

    auto& rwBuffer = ctx->rwBuffer;
    bool gotData = false;

    while(true) {
        ValidRegion region = rwBuffer.GetWritableReadRegion();
        if(!region.ptr || region.len == 0) {
            if(!rwBuffer.GrowReadBuffer(config_.networkConfig.readBufferIncSize,
                                        config_.networkConfig.maxReadBufferSize)) {
                logger_.Warn("[Kqueue]: Read buffer full, closing connection");
                Close(ctx);
                return;
            }
            region = rwBuffer.GetWritableReadRegion();
        }

        ssize_t res = WrapRead(ctx, region.ptr, region.len);
        if(res > 0) {
            rwBuffer.AdvanceReadLength(res);
            gotData = true;
        }
        else if(res == 0) {
            Close(ctx);
            return;
        }
        else {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                ctx->eventType = EventType::EVENT_RECV;
                break;
            }
            Close(ctx);
            return;
        }
    }

    if(gotData)
        onReceive_(ctx);
}

void KqueueConnectionHandler::SendFile(ConnectionContext* ctx)
{
    WFX_TRACE();

    if(ctx->fileInfo.fd < 0 || ctx->fileInfo.fileSize <= 0) {
        logger_.Warn("[Kqueue]: 'SendFile' expects 'ctx->fileInfo' to be set, got invalid data");
        Close(ctx);
        return;
    }

    auto& fileInfo = ctx->fileInfo;
    int fd = fileInfo.fd;

    while(fileInfo.offset < fileInfo.fileSize) {
        ssize_t n = WrapFile(ctx, fd, &fileInfo.offset, fileInfo.fileSize - fileInfo.offset);
        if(n > 0)
            continue;

        if(n < 0) {
            if(n == SWITCH_FILE_TO_STREAM) {
                ResumeStream(ctx);
                return;
            }

            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                ctx->eventType = EventType::EVENT_SEND_FILE;
                if(!RegisterKqueue(ctx, KQ_ADD_WRITE))
                    Close(ctx);
            }
            else
                Close(ctx);

            return;
        }

        break;
    }

    if(ctx->GetConnectionState() == ConnectionState::CONNECTION_CLOSE)
        Close(ctx);
    else
        FinishWriteCycle(ctx);
}

void KqueueConnectionHandler::ResumeStream(ConnectionContext* ctx)
{
    WFX_TRACE();

    if(!ctx->streamGenerator.ctx || !ctx->streamGenerator.Next) {
        logger_.Warn("[Kqueue]: 'ResumeStream' called but received empty generator");
        Close(ctx);
        return;
    }

    auto& netCfg = config_.networkConfig;
    auto& rwBuffer = ctx->rwBuffer;

    auto writeMeta = rwBuffer.GetWriteMeta();
    if(!writeMeta) {
        Close(ctx);
        return;
    }

    auto writeRegion = rwBuffer.GetWritableWriteRegion();
    char localBuf[4096];
    const std::size_t cap = std::min(writeRegion.len, sizeof(localBuf));
    if(cap == 0) {
        Close(ctx);
        return;
    }

    auto streamResult = ctx->streamGenerator.Next(ctx->streamGenerator.ctx, {localBuf, cap});

    RefreshExpiry(ctx, config_.networkConfig.idleTimeout);

    switch(streamResult.action) {
        case StreamAction::CONTINUE: {
            if(streamResult.writtenBytes == 0 || streamResult.writtenBytes > UINT32_MAX) {
                Close(ctx);
                return;
            }

            rwBuffer.ClearWriteBuffer();

            if(!ctx->streamChunked) {
                if(!rwBuffer.AppendWriteData(localBuf, static_cast<std::uint32_t>(streamResult.writtenBytes),
                                             netCfg.sendBufferIncSize, netCfg.maxSendBufferSize)) {
                    Close(ctx);
                    return;
                }
            }
            else {
                char chunkHeader[16];
                int headerLen = snprintf(chunkHeader, sizeof(chunkHeader), "%zX\r\n", streamResult.writtenBytes);
                if(headerLen <= 0 || headerLen >= static_cast<int>(sizeof(chunkHeader))) {
                    Close(ctx);
                    return;
                }

                if(!rwBuffer.AppendWriteData(chunkHeader, static_cast<std::uint32_t>(headerLen),
                                             netCfg.sendBufferIncSize, netCfg.maxSendBufferSize) ||
                   !rwBuffer.AppendWriteData(localBuf, static_cast<std::uint32_t>(streamResult.writtenBytes),
                                             netCfg.sendBufferIncSize, netCfg.maxSendBufferSize) ||
                   !rwBuffer.AppendWriteData("\r\n", 2, netCfg.sendBufferIncSize, netCfg.maxSendBufferSize)) {
                    Close(ctx);
                    return;
                }
            }

            Write(ctx);
            return;
        }
        case StreamAction::STOP_AND_ALIVE_CONN:
            ctx->SetConnectionState(ConnectionState::CONNECTION_ALIVE);
            break;
        case StreamAction::STOP_AND_CLOSE_CONN:
        default:
            ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
            break;
    }

    if(ctx->streamGenerator.Destroy)
        ctx->streamGenerator.Destroy(ctx->streamGenerator.ctx);

    bool wasChunked = static_cast<bool>(ctx->streamChunked);

    rwBuffer.ClearWriteBuffer();
    ctx->isStreamOperation = 0;
    ctx->streamChunked = 0;
    ctx->streamGenerator = {0};

    if(wasChunked)
        rwBuffer.AppendWriteData(CHUNK_END, sizeof(CHUNK_END) - 1, config_.networkConfig.sendBufferIncSize,
                                 config_.networkConfig.maxSendBufferSize)
            ? Write(ctx)
            : Close(ctx);

    else if(ctx->GetConnectionState() == ConnectionState::CONNECTION_ALIVE)
        FinishWriteCycle(ctx);
    else
        Close(ctx);
}

void KqueueConnectionHandler::HandleAsyncCallback(ConnectionContext* ctx, AsyncResult res, bool destroy)
{
    WFX_TRACE();

    auto& async = ctx->asyncData;

    if(!async.AsyncComplete && !async.AsyncDestroy)
        return;

    auto complete = async.AsyncComplete;
    auto kill = async.AsyncDestroy;
    auto ud = async.userData;

    async.AsyncComplete = nullptr;
    async.AsyncDestroy = nullptr;
    async.userData = nullptr;

    Shared::GetHttpAPIExt1()->SetGlobalPtrData(ctx);

    if(destroy) {
        if(kill)
            kill(ud);
    }
    else {
        if(complete)
            complete(ud, res);
    }

    Shared::GetHttpAPIExt1()->SetGlobalPtrData(nullptr);
}

void KqueueConnectionHandler::HandleTimeoutTimer()
{
    std::uint64_t nowSec = NowMs() / 1000;
    timerWheel_.Tick(nowSec);
}

void KqueueConnectionHandler::HandleAsyncTimer()
{
    std::uint64_t newTick = NowMs();
    std::uint64_t connId = 0;

    while(timerHeap_.PopExpired(newTick, connId)) {
        ConnectionContext* ctx = connections_.GetPtr(connId);
        ctx->isAsyncTimerOperation = 0;

        HandleAsyncCallback(ctx, {nullptr, 0, MiddlewareAction::CONTINUE, AsyncStatus::COMPLETED}, false);
    }

    UpdateAsyncTimer();
}

void KqueueConnectionHandler::HandleHandshake(ConnectionContext* ctx, std::int16_t filter)
{
    WFX_TRACE();

    EventType onSuccess = ctx->IsEndpoint() ? EventType::EVENT_SEND : EventType::EVENT_RECV;

    if(!TryHandshake(ctx, onSuccess)) {
        Close(ctx);
        return;
    }

    if(ctx->eventType == EventType::EVENT_HANDSHAKE)
        return;

    if(ctx->eventType == EventType::EVENT_SEND)
        Write(ctx, {});
    else if(filter == EVFILT_READ)
        Receive(ctx);
}

void KqueueConnectionHandler::HandleWriteReady(ConnectionContext* ctx, std::int16_t filter)
{
    WFX_TRACE();

    switch(ctx->eventType) {
        case EventType::EVENT_SEND:
            Write(ctx, {});
            break;

        case EventType::EVENT_SEND_FILE:
            SendFile(ctx);
            break;

        case EventType::EVENT_CONNECT: {
            int err = 0;
            socklen_t len = sizeof(err);

            if(getsockopt(ctx->socket, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
                Close(ctx);
                break;
            }

            if(ctx->GetEndpointState() == EndpointState::ENDPOINT_SECURE) {
                ctx->sslConn = sslHandler_->WrapClient(ctx->socket, endpoints_[ctx->endpointIdx].first.host.c_str());

                if(!ctx->sslConn) {
                    Close(ctx);
                    break;
                }

                HandleHandshake(ctx, filter);
                break;
            }

            Write(ctx, {});
        } break;

        default:
            break;
    }
}

void KqueueConnectionHandler::UpdateAsyncTimer()
{
    TimerNode* min = timerHeap_.GetMin();

    if(!min) {
        // Disarm the async timer by deleting it from kqueue
        struct kevent kev;
        EV_SET(&kev, ASYNC_TIMER_IDENT, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
        kevent(kqFd_, &kev, 1, nullptr, 0, nullptr);
        return;
    }

    std::uint64_t now = NowMs();
    std::uint64_t expire = min->delay;
    std::uint64_t remain = (expire <= now) ? 1 : (expire - now);

    struct kevent kev;
    // fflags = 0: default unit is milliseconds; EV_ONESHOT fires once then auto-removes
    EV_SET(&kev, ASYNC_TIMER_IDENT, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT, 0, (intptr_t)remain, nullptr);
    kevent(kqFd_, &kev, 1, nullptr, 0, nullptr);
}

std::uint64_t KqueueConnectionHandler::PackKqueueData(ConnectionContext* ctx)
{
    bool isEndpoint = ctx->IsEndpoint();

    std::uint16_t tag = isEndpoint ? ctx->endpointIdx : CLIENT_CONNECTION_TAG;
    auto& pool = isEndpoint ? endpoints_[ctx->endpointIdx].second : connections_;

    std::uint32_t idx = pool.GetIndex(ctx);

    // tag:16 | generation:16 | pool index:32
    return (static_cast<std::uint64_t>(tag) << 48) | (static_cast<std::uint64_t>(ctx->generationId) << 32) |
           static_cast<std::uint64_t>(idx);
}

bool KqueueConnectionHandler::RegisterKqueue(ConnectionContext* ctx, int op)
{
    struct kevent changes[2];
    int n = 0;
    std::uint64_t packed = 0;
    void* udata = nullptr;

    if(op == KQ_DEL) {
        EV_SET(&changes[n++], ctx->socket, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        EV_SET(&changes[n++], ctx->socket, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    }
    else {
        packed = PackKqueueData(ctx);
        udata = (void*)(uintptr_t)packed;

        switch(op) {
            case KQ_ADD:
                // Start read-only; write is added only when bytes are pending.
                EV_SET(&changes[n++], ctx->socket, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, udata);
                break;
            case KQ_MOD:
            case KQ_ADD_WRITE:
                EV_SET(&changes[n++], ctx->socket, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, udata);
                break;
            case KQ_DROP_WRITE:
                EV_SET(&changes[n++], ctx->socket, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
                break;
            case KQ_REARM_READ:
                EV_SET(&changes[n++], ctx->socket, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
                EV_SET(&changes[n++], ctx->socket, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, udata);
                break;
        }
    }

    while(true) {
        if(kevent(kqFd_, changes, n, nullptr, 0, nullptr) == 0)
            return true;

        if(errno == EINTR)
            continue;

        if((op == KQ_DEL || op == KQ_DROP_WRITE) && errno == ENOENT)
            return true;

        if((op == KQ_ADD || op == KQ_REARM_READ || op == KQ_MOD || op == KQ_ADD_WRITE) && errno == EEXIST)
            return true;

        return false;
    }
}

bool KqueueConnectionHandler::TryHandshake(ConnectionContext* ctx, EventType onSuccess)
{
    switch(sslHandler_->Handshake(ctx->sslConn)) {
        case SSLReturn::SUCCESS:
            ctx->eventType = onSuccess;
            return true;

        case SSLReturn::WANT_READ:
        case SSLReturn::WANT_WRITE:
            ctx->eventType = EventType::EVENT_HANDSHAKE;
            return true;

        default:
            return false;
    }
}

bool KqueueConnectionHandler::CreateAndConnect(ConnectionContext* ctx, EndpointContext& epCtx)
{
    ctx->socket = socket(epCtx.addr.ss_family, SOCK_STREAM, 0);
    if(ctx->socket < 0)
        return false;

    if(!SetNonBlocking(ctx->socket))
        return false;

    if(!SetNoSigPipe(ctx->socket))
        return false;

    while(true) {
        int ret = connect(ctx->socket, reinterpret_cast<const sockaddr*>(&epCtx.addr), epCtx.addrLen);
        if(ret == 0)
            return true;

        if(errno == EINTR)
            continue;

        // Non-blocking connect completes later through EVFILT_WRITE.
        if(errno == EINPROGRESS) {
            ctx->eventType = EventType::EVENT_CONNECT;
            return true;
        }

        return false;
    }
}

void KqueueConnectionHandler::WrapAccept(ConnectionContext* ctx)
{
    WFX_TRACE();

    int clientFd = ctx->socket;

    if(useHttps_) {
        // TLS accept can finish now or continue through the normal handshake state.
        ctx->sslConn = sslHandler_->Wrap(clientFd);
        if(!ctx->sslConn) {
            ReleaseConnection(ctx);
            return;
        }

        if(!TryHandshake(ctx, EventType::EVENT_RECV)) {
            Close(ctx);
            return;
        }
    }
    else
        ctx->eventType = EventType::EVENT_RECV;

    if(!RegisterKqueue(ctx, KQ_ADD)) {
        Close(ctx);
        return;
    }

    metrics_->network.accepts++;

    RefreshExpiry(ctx, config_.networkConfig.idleTimeout);
}

EndpointStatus KqueueConnectionHandler::WrapConnect(ConnectionContext* ctx, EndpointContainer& ecnt)
{
    WFX_TRACE();

    auto& endpointCtx = ecnt.first;

    if(!CreateAndConnect(ctx, endpointCtx))
        return EndpointStatus::CONNECT_FAILURE;

    if(ctx->eventType != EventType::EVENT_CONNECT && ctx->GetEndpointState() == EndpointState::ENDPOINT_SECURE) {
        ctx->sslConn = sslHandler_->WrapClient(ctx->socket, endpointCtx.host.c_str());
        if(!ctx->sslConn)
            return EndpointStatus::SSL_FAILURE;

        if(!TryHandshake(ctx, EventType::EVENT_SEND))
            return EndpointStatus::SSL_FAILURE;
    }

    if(!RegisterKqueue(ctx, KQ_ADD))
        return EndpointStatus::INTERNAL_ERROR;

    // Force kqueue to re-check writability after a fast connect.
    if(ctx->eventType != EventType::EVENT_CONNECT && !RegisterKqueue(ctx, KQ_MOD))
        return EndpointStatus::INTERNAL_ERROR;

    RefreshExpiry(ctx, config_.networkConfig.idleTimeout);
    return EndpointStatus::PENDING;
}

ssize_t KqueueConnectionHandler::WrapRead(ConnectionContext* ctx, char* buf, std::size_t len)
{
    if(!ctx->sslConn) {
        ssize_t n;
        do {
            n = ::recv(ctx->socket, buf, len, 0);
        } while(n < 0 && errno == EINTR);

        if(n > 0) {
            metrics_->network.reads++;
            metrics_->network.bytesRead += static_cast<std::uint64_t>(n);
        }

        return n;
    }

    SSLResult result = sslHandler_->Read(ctx->sslConn, buf, static_cast<int>(len));

    switch(result.error) {
        case SSLReturn::SUCCESS: {
            metrics_->network.reads++;
            metrics_->network.bytesRead += static_cast<std::uint64_t>(result.res);
            return result.res;
        }
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

ssize_t KqueueConnectionHandler::WrapWrite(ConnectionContext* ctx, const char* buf, std::size_t len)
{
    if(!ctx->sslConn) {
        // macOS: SO_NOSIGPIPE is set on socket; send without MSG_NOSIGNAL
        ssize_t n;
        do {
            n = ::send(ctx->socket, buf, len, 0);
        } while(n < 0 && errno == EINTR);

        if(n > 0) {
            metrics_->network.writes++;
            metrics_->network.bytesWritten += static_cast<std::uint64_t>(n);
        }

        return n;
    }

    SSLResult result = sslHandler_->Write(ctx->sslConn, buf, static_cast<int>(len));

    switch(result.error) {
        case SSLReturn::SUCCESS: {
            metrics_->network.writes++;
            metrics_->network.bytesWritten += static_cast<std::uint64_t>(result.res);
            return result.res;
        }
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

ssize_t KqueueConnectionHandler::WrapFile(ConnectionContext* ctx, int fd, off_t* offset, std::size_t count)
{
    if(!ctx->sslConn) {
        // macOS sendfile writes the transferred byte count back into len.
        off_t len = static_cast<off_t>(count);
        int ret = ::sendfile(fd, ctx->socket, *offset, &len, nullptr, 0);

        bool sentSome = (len > 0);

        if(sentSome) {
            *offset += len;
            metrics_->network.fileCalls++;
            metrics_->network.fileBytesWritten += static_cast<std::uint64_t>(len);
        }

        if(ret == 0)
            return (ssize_t)len; // All bytes sent successfully

        // ret == -1: either EAGAIN (partial ok) or fatal error
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            if(sentSome)
                return (ssize_t)len;
            return -1;
        }

        return -1; // Fatal error
    }

    // SSL: macOS sendfile is not supported over SSL; fall back to read-then-write streaming
    SSLResult result = sslHandler_->WriteFile(ctx->sslConn, fd, offset ? *offset : 0, count);

    switch(result.error) {
        case SSLReturn::NO_IMPL: {
            metrics_->network.fileFallbacks++;

            ctx->isFileOperation = 0;
            ctx->isStreamOperation = 1;
            ctx->streamChunked = 0;
            ctx->streamGenerator = {&ctx->fileInfo,
                                    [](void* c, StreamBuffer buffer) -> StreamResult {
                                        auto* fileInfo = static_cast<FileInfo*>(c);

                                        ssize_t res = pread(fileInfo->fd, buffer.buffer, buffer.size, fileInfo->offset);

                                        if(res <= 0) {
                                            return StreamResult{0, res == 0 ? StreamAction::STOP_AND_ALIVE_CONN
                                                                            : StreamAction::STOP_AND_CLOSE_CONN};
                                        }

                                        fileInfo->offset += res;
                                        return StreamResult{static_cast<std::size_t>(res), StreamAction::CONTINUE};
                                    },
                                    nullptr};

            return SWITCH_FILE_TO_STREAM;
        }
        case SSLReturn::SUCCESS: {
            metrics_->network.fileCalls++;
            metrics_->network.fileBytesWritten += static_cast<std::uint64_t>(result.res);

            if(offset)
                *offset += result.res;

            return result.res;
        }
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
