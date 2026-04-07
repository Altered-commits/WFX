#ifndef WFX_LINUX_USE_IO_URING

#include "epoll_connection.hpp"

#include "http/common/http_error_msgs.hpp"
#include "http/common/http_global_state.hpp"
#include "http/ssl/http_ssl_factory.hpp"
#include "utils/crash_tracer/crash_tracer.hpp"
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

// vvv Constructor & Destructor vvv
EpollConnectionHandler::EpollConnectionHandler(bool useHttps)
    : useHttps_(useHttps)
{
    if(useHttps)
        sslHandler_ = CreateSSLHandler();
}

EpollConnectionHandler::~EpollConnectionHandler()
{
    if(listenFd_ > 0)       { close(listenFd_);       listenFd_ = -1;       }
    if(timeoutTimerFd_ > 0) { close(timeoutTimerFd_); timeoutTimerFd_ = -1; }
    if(asyncTimerFd_ > 0)   { close(asyncTimerFd_);   asyncTimerFd_ = -1;   }
    if(epollFd_ > 0)        { close(epollFd_);        epollFd_ = -1;        }

    logger_.Info("[Epoll]: Cleaned up resources successfully");
}

// vvv Initializing Functions vvv
void EpollConnectionHandler::Initialize(const std::string& host, std::uint16_t port)
{
    WFX_TRACE();

    auto& osConfig      = config_.osSpecificConfig;
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
        if(setsockopt(listenFd_, IPPROTO_IPV6, IPV6_V6ONLY, (void *)&no, sizeof(no)) < 0)
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
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = listenFd_;
    if(epoll_ctl(epollFd_, EPOLL_CTL_ADD, listenFd_, &ev) < 0)
        logger_.Fatal("[Epoll]: Failed to add listening socket to epoll: ", strerror(errno));

    // vvv Initialize timeout handler vvv
    timerWheel_.Init(
        connections_.GetSlots(),
        4096, 1, TimeUnit::SECONDS,
        [this](std::uint32_t connId, std::uint32_t extra) {
            // 'extra' for now just contains a 16 bit value. If value
            //     >= CLIENT_CONNECTION_TAG, then its a client connection
            //     <  CLIENT_CONNECTION_TAG, then its an endpoint connection
            // It is going to be a 16 bit value, but for correctness sake-
            // -we do '>=' instead of '=='
            // NOTE: extra will be used as endpoint index, set inside of 'RefreshExpiry'
            ConnectionContext* ctx = nullptr;

            if(extra >= CLIENT_CONNECTION_TAG)
                ctx = connections_.GetPtr(connId);
            else
                ctx = endpoints_[extra].second.GetPtr(connId);

            // So the logic behind the if condition is, in normal sync path, if a connection is marked-
            // -'close', it will trigger cleanup after it sent data so no need to clash with it
            // But on the other hand, in the async / endpoint path, if a connections is marked 'close' and-
            // -the callback, for some odd reason, just hung up and isn't responding, we shouldn't care-
            // -about connection atp. WE CLOSE IT OURSELVES
            if(
                ctx->GetConnectionState() != ConnectionState::CONNECTION_CLOSE
                || (ctx->IsEndpoint() || ctx->IsAsyncOperation())
            )
                Close(ctx, true);
        }
    );

    timeoutTimerFd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if(timeoutTimerFd_ < 0)
        logger_.Fatal("[Epoll]: Failed to create timeout timer: ", strerror(errno));

    itimerspec ts{};
    ts.it_interval.tv_sec  = INVOKE_TIMEOUT_COOLDOWN;
    ts.it_interval.tv_nsec = 0;
    ts.it_value.tv_sec     = INVOKE_TIMEOUT_DELAY;
    ts.it_value.tv_nsec    = 0;

    if(timerfd_settime(timeoutTimerFd_, 0, &ts, nullptr) < 0)
        logger_.Fatal("[Epoll]: Failed to set timeout timer: ", strerror(errno));

    epoll_event tev{};
    tev.events  = EPOLLIN;
    tev.data.u64 = static_cast<std::uint64_t>(timeoutTimerFd_) & 0xFFFFFFFFULL; // Lower 32 bits = fd, upper 32 = 0
    if(epoll_ctl(epollFd_, EPOLL_CTL_ADD, timeoutTimerFd_, &tev) < 0)
        logger_.Fatal("[Epoll]: Failed to add timeout timer to epoll: ", strerror(errno));

    // vvv Initializing async timer vvv
    asyncTimerFd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if(asyncTimerFd_ < 0)
        logger_.Fatal("[Epoll]: Failed to create async timer: ", strerror(errno));

    epoll_event aev{};
    aev.events  = EPOLLIN;
    aev.data.u64 = static_cast<std::uint64_t>(asyncTimerFd_) & 0xFFFFFFFFULL; // Lower 32 bits = fd, upper 32 = 0
    if(epoll_ctl(epollFd_, EPOLL_CTL_ADD, asyncTimerFd_, &aev) < 0)
        logger_.Fatal("[Epoll]: Failed to add async timer to epoll: ", strerror(errno));
}

void EpollConnectionHandler::SetEngineCallback(ReceiveCallback onData)
{
    onReceive_ = std::move(onData);
}

std::uint16_t EpollConnectionHandler::AllocateEndpoint(
    std::string_view host, std::string_view port, std::uint32_t cLimit, std::uint32_t ifLimit, bool useTLS
) {
    /*
     * IMPORTANT:
     *  - 'host' and 'port' must strictly be null terminated (or gg)
     *  - 'ifLimit' is currently ignored, will be used in future
     */
    (void)ifLimit;

    // We DO NOT allow more than 0xFFFE distinct endpoints (aka max(uint16_t) - 1)
    // Simple reason being, it makes no sense for a single server to handle THAT MANY-
    // -DISTINCT ENDPOINTS (not routes or metadata, pure distinct endpoints)
    if(endpoints_.size() > MAX_DISTINCT_ENDPOINTS)
        logger_.Fatal("[Epoll]: Too many distinct domain endpoints registered");

    auto& endpointSlot = endpoints_.emplace_back(
                            std::piecewise_construct,
                            std::forward_as_tuple(),      // EndpointContext{}
                            std::forward_as_tuple(cLimit) // BitmapPool{cLimit}
                        );

    auto& endpointInfo = endpointSlot.first;
    auto& endpointPool = endpointSlot.second;

    std::uint16_t endpointIdx = endpoints_.size() - 1;
    
    // Initialize pool values
    for(std::uint32_t j = 0; j < endpointPool.GetSlots(); j++) {
        auto* ctx = endpointPool.GetPtr(j);

        // Initialize with default values
        ctx->endpointIdx = endpointIdx;
        ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
        ctx->SetEndpointState(
            useTLS
                ? EndpointState::ENDPOINT_SECURE
                : EndpointState::ENDPOINT_INSECURE
        );
    }

    // Create std::string for safer use, its temporary so no issue (except host)
    std::string tempHost = std::string(host);
    std::string tempPort = std::string(port);

    if(!ResolveHost(tempHost.c_str(), tempPort.c_str(), &endpointInfo.addr, &endpointInfo.addrLen))
        logger_.Fatal("[Epoll]: Failed to resolve endpoint URL: ", host, ':', port);

    endpointInfo.host = std::move(tempHost);

    // Return the currently emplaced ctx's index
    return endpointIdx;
}

// vvv I/O Operations vvv
void EpollConnectionHandler::ResumeReceive(ConnectionContext* ctx)
{
    if(!EnsureReadReady(ctx))
        return;

    // We are ready to data now, set 'eventType' to EVENT_RECV
    ctx->eventType = EventType::EVENT_RECV;
}

void EpollConnectionHandler::Write(ConnectionContext* ctx, std::string_view msg)
{
    WFX_TRACE();

    // Case 1: Direct send (used only for static error codes)
    // NOTE: CHANGE OF PLANS, msg is fire and forget, i don't care if they get delivered-
    // -or not, if u want good error messages u will go the hard route anyways (res.Status().SendText()...)
    if(!msg.empty()) {
        (void)WrapWrite(ctx, msg.data(), msg.size());
        // We ignore result intentionally. If state says close -> close, else -> resume receive
        goto __CleanupOrRearm;
    }
    
    // Case 2: Send from buffer
    else {
        auto* writeMeta = ctx->rwBuffer.GetWriteMeta();
        if(!writeMeta || writeMeta->writtenLength >= writeMeta->dataLength)
            goto __CleanupOrRearm;

        while(writeMeta->writtenLength < writeMeta->dataLength) {
            const char* buf = ctx->rwBuffer.GetWriteData() + writeMeta->writtenLength;
            std::size_t remaining = writeMeta->dataLength - writeMeta->writtenLength;

            ssize_t n = WrapWrite(ctx, buf, remaining);

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
        ctx->ClearContext();
        ResumeReceive(ctx);
    }
}

void EpollConnectionHandler::WriteFile(ConnectionContext* ctx, std::string path)
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

EndpointStatus EpollConnectionHandler::WriteEndpoint(
    ConnectionContext* ctx, std::uint32_t endpointIndex, const std::byte* ptr, std::uint32_t size
) {
    /*
     * A bit of explanation on how we will proceed with writing to an endpoint:
     *  - Copy the entire data (ptr) of size (size: u32) into endpoint write buffer (if it can fit)
     *  - Let endpoint do its job of writing and reading
     *  - After it finishes doing it, it either succeeds or fails, whatever is the case, we handle it in loop
     */
    // Sanity checks
    if(endpointIndex > endpoints_.size() - 1)
        return EndpointStatus::INVALID_KEY;

    // Try to lease connection from the endpoint's connection pool
    auto* allocatedCtx = GetConnection(endpointIndex);
    if(!allocatedCtx)
        return EndpointStatus::POOL_EXHAUSTED;

    numConnectionsAlive_++; // TODO: For debugging, remove later

    // Initialize write buffer if it hasn't
    auto& endpointRWBuffer = allocatedCtx->rwBuffer;
    if(
        !endpointRWBuffer.IsWriteInitialized()
        && !endpointRWBuffer.InitWriteBuffer(config_.networkConfig.maxSendBufferSize)
    ) {
        ReleaseConnection(allocatedCtx, true);
        return EndpointStatus::BUFFER_ERROR;
    }

    if(!endpointRWBuffer.AppendWriteData(
        reinterpret_cast<const char*>(ptr),
        size,
        config_.networkConfig.sendBufferIncSize,
        config_.networkConfig.maxSendBufferSize
    )) {
        ReleaseConnection(allocatedCtx, true);
        return EndpointStatus::INSUFFICIENT_BUFFER;
    }

    allocatedCtx->clientContext = ctx;   // |   (My golang fellas)
    ctx->endpointContext = allocatedCtx; // |-> Forms a channel like structure between client and endpoint

    EndpointStatus result;

    // Not yet connected, let 'WrapConnect' handle both connecting + sending of data
    if(allocatedCtx->GetConnectionState() == ConnectionState::CONNECTION_CLOSE)
        result = WrapConnect(allocatedCtx, endpoints_[endpointIndex]);
    else {
        // Rearm endpoint so 'EPOLLOUT' fires and event loop calls 'Write'
        allocatedCtx->eventType = EventType::EVENT_SEND;
        result = RegisterEpoll(allocatedCtx, EPOLL_CTL_MOD)
            ? EndpointStatus::PENDING
            : EndpointStatus::INTERNAL_ERROR;
    }

    // Failure Case: 'Unlink' before 'Close' so it doesn't fire 'HandleAsyncResume'-
    // -on a coroutine that hasn't suspended yet
    if(result != EndpointStatus::PENDING) {
        allocatedCtx->clientContext = nullptr;
        ctx->endpointContext        = nullptr;
        Close(allocatedCtx, true);
    }

    return result;
}

void EpollConnectionHandler::Stream(ConnectionContext* ctx, StreamGenerator generator, bool streamChunked)
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
    ctx->streamChunked     = streamChunked;
    Write(ctx, {});
}

void EpollConnectionHandler::Close(ConnectionContext* ctx, bool forceClose)
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
                ctx->eventType = EventType::EVENT_SHUTDOWN;
                return;
            }
        }
    }

    // Synchronous cleanup for both non-SSL and SSL paths
    (void)RegisterEpoll(ctx, EPOLL_CTL_DEL);
    ReleaseConnection(ctx);
}

// vvv Main Functions vvv
void EpollConnectionHandler::Run()
{
    WFX_TRACE();

    // Just a simple sanity check before we do anything
    if(!onReceive_)
        logger_.Fatal(
            "[Epoll]: Member 'onReceive_' was not initialized. Call 'SetEngineCallback' before calling 'Run'"
        );

    // Used for special fds like timers, accepts, etc
    int sfd = 0;

    while(running_) {
        int nfds = epoll_wait(epollFd_, events_.get(), maxEvents_, -1);
        if(nfds < 0) {
            // Interrupted by signal
            if(errno == EINTR)
                continue;
            break;
        }

        // Handle nfds events which epoll gave us
        for(std::uint32_t i = 0; i < nfds; i++) {
            std::uint32_t ev   = events_[i].events;
            std::uint64_t meta = events_[i].data.u64;
            std::uint16_t gen  = (meta >> 32) & 0xFFFF; // First half's lower 16 bits

            // Existing connection, handle it
            if(gen > 0)
                goto __HandleExistingConnection;

            sfd = static_cast<int>(meta & 0xFFFFFFFFULL);

            // Handle timeouts timers
            if(sfd == timeoutTimerFd_) {
                HandleTimeoutTimer(sfd);
                continue;
            }

            // Handle async timers
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
                        if(errno == EAGAIN || errno == EWOULDBLOCK)
                            break; // Queue drained, stop
                        else
                            continue; // Transient error, skip this one
                    }

                    // Extract IP info first
                    WFXIpAddress tmpIp;
                    if(!ResolveIP(addr, tmpIp)) {
                        close(clientFd);
                        continue;
                    }

                    // Check limiter and try to grab a slot if its valid
                    ConnectionContext* ctx = nullptr;
                    if(!ipLimiter_.AllowConnection(tmpIp) || !(ctx = GetConnection())) {
                        close(clientFd);
                        continue;
                    }

                    // Set connection info
                    ctx->socket   = clientFd;
                    ctx->connInfo = tmpIp;
                    
                    numConnectionsAlive_++;
                    WrapAccept(ctx);
                }
                continue;
            }

        __HandleExistingConnection:
            // Get connection context
            // Also if you are confused with the below hardcoded numbers, check 'PackEpollData'-
            // -function, you can see how data is packed in event.u64 member field
            std::uint16_t endpointIdx = meta >> 48;
            std::uint32_t poolIdx     = meta & 0xFFFFFFFF;
            
            ConnectionContext* ctx = nullptr;

            // Client connection
            if(endpointIdx == CLIENT_CONNECTION_TAG)
                ctx = connections_.GetPtr(poolIdx);
            // Endpoint connection
            else
                ctx = endpoints_[endpointIdx].second.GetPtr(poolIdx);

            // If the slot's current generation doesn't match the event's generation, it means-
            // -this event is for a dead connection
            if(ctx->generationId != gen)
                continue;

            // SSL handshake in progress
            if(ctx->eventType == EventType::EVENT_HANDSHAKE) {
                HandleHandshake(ctx, ev);
                continue;
            }
            
            // SSL shutdown is in progress
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
                        ReleaseConnection(ctx);
                        break;
                }
                continue;
            }

            if(ev & (EPOLLERR | EPOLLHUP)) {
                Close(ctx);
                continue;
            }

            // If the 'ctx->eventType' is NOT EVENT_RECV, its most probably:
            //  - I forgot to set it somewhere
            //  - We are doing other task and client is trying to send more data
            // In any case, we just ignore it
            if((ev & EPOLLIN) && ctx->eventType == EventType::EVENT_RECV) {
                // Check per ip request rate BEFORE processing anything
                if(!ipLimiter_.AllowRequest(ctx->connInfo)) {
                    ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
                    Write(ctx, HttpError::tooManyRequests);
                    continue;
                }
                Receive(ctx);
            }
            
            if(ev & EPOLLOUT)
                HandleWriteReady(ctx, ev);
        }
    }
}

void EpollConnectionHandler::RefreshExpiry(ConnectionContext* ctx, std::uint16_t timeoutSeconds)
{
    ConnectionPool* pool  = nullptr;
    std::uint32_t   extra = CLIENT_CONNECTION_TAG;

    if(ctx->IsEndpoint()) {
        extra = ctx->endpointIdx;
        pool  = &endpoints_[extra].second;
    }
    else
        pool = &connections_;

    std::uint32_t idx = pool->GetIndex(ctx);
    timerWheel_.Schedule(idx, extra, timeoutSeconds);
}

bool EpollConnectionHandler::RefreshAsyncTimer(
    ConnectionContext* ctx, std::uint32_t delayMs, AsyncCompleteFn onComplete, void* userData
) {
    std::uint32_t idx    = connections_.GetIndex(ctx);
    std::uint64_t expire = NowMs() + delayMs;

    // Timers are coalesced if they fall within +-10ms of each other
    if(!timerHeap_.Insert(idx, expire, 10)) {
        logger_.Warn("[Epoll]: Failed to refresh async timer");
        return false;
    }

    ctx->isAsyncTimerOperation = 1;
    ctx->asyncOnDone           = onComplete;
    ctx->asyncUserData         = userData;

    UpdateAsyncTimer();

    return true;
}

void EpollConnectionHandler::Stop()
{
    running_ = false;
}

// vvv Helper Functions vvv
//  --- Connection Handlers ---
ConnectionContext* EpollConnectionHandler::GetConnection(std::uint16_t endpointIndex)
{
    WFX_TRACE();

    ConnectionContext* ctx = nullptr;

    // Client connection
    if(endpointIndex == CLIENT_CONNECTION_TAG)
        ctx = connections_.AllocSlot();
    // Endpoint connection
    else
        ctx = endpoints_[endpointIndex].second.AllocSlot();

    if(!ctx)
        return nullptr;

    ctx->generationId++;

    // If it wraps to 0, bump it to 1 cuz 0 is reserved for identifying fds such as Listen/Timer
    if(ctx->generationId == 0)
        ctx->generationId = 1;

    return ctx;
}

void EpollConnectionHandler::ReleaseConnection(ConnectionContext* ctx, bool freeOnly)
{
    WFX_TRACE();

    if(!ctx)
        return;

    // For debugging purposes
    numConnectionsAlive_--;

    // Pick the correct pool
    auto& pool = ctx->IsEndpoint()
                    ? endpoints_[ctx->endpointIdx].second
                    : connections_;

    // Determine index
    std::uint32_t idx = pool.GetIndex(ctx);

    // If 'freeOnly' is set, we just want to free slot and clear up context, nothing else
    if(freeOnly)
        goto __FreeContext;

    // Cancelling timer in 'Close' kinda sucks cuz during async shutdown-
    // -the client might bail, never finish it, and we just be stuck in-
    // -closing state forever aaand timeout won't do anything cuz... we cancelled it
    // So we close it here instead
    timerWheel_.Cancel(idx);

    // Only applicable for client operations
    if(!ctx->IsEndpoint()) {
        if(ctx->isAsyncTimerOperation) {
            if(timerHeap_.Remove(idx))
                UpdateAsyncTimer();
            else
                logger_.Warn("[Epoll]: Failed to cancel async timer");
        }

        // From clients POV, if the endpoint hasn't been set to nullptr after-
        // -endpoint operations complete, it means client closed before endpoint even-
        // -completed. In this case, force close the endpoint connection as well
        if(ctx->endpointContext)
            Close(ctx->endpointContext, true);

        ipLimiter_.ReleaseConnection(ctx->connInfo);
    }

    // Only applicable to endpoint operations
    // If client context exists, it means that the endpoint operation hasn't finished-
    // -but it somehow closed, in this case, just notify the client (as client is suspended-
    // -due to co_await)
    else if(ctx->clientContext)
        HandleAsyncCallback(ctx->clientContext, {
            nullptr, 0,
            MiddlewareAction::CONTINUE,
            AsyncStatus::IO_FAILURE
        });

    if(ctx->socket > 0)
        close(ctx->socket);

__FreeContext:
    ctx->ResetContext();
    pool.FreeSlot(idx);
}

//  --- MISC Handlers ---
std::uint64_t EpollConnectionHandler::NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        SteadyClock::now() - startTime_
    ).count();
}

bool EpollConnectionHandler::SetNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags < 0)
        return false;
    
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool EpollConnectionHandler::EnsureFileReady(ConnectionContext* ctx, std::string path)
{
    auto [fd, size] = fileCache_.GetFileDesc(std::move(path));
    if(fd < 0)
        return false;

    ctx->fileInfo.fd       = fd;
    ctx->fileInfo.offset   = 0;
    ctx->fileInfo.fileSize = size;

    return true;
}

bool EpollConnectionHandler::EnsureReadReady(ConnectionContext* ctx)
{
    auto& rwBuffer = ctx->rwBuffer;
    auto& netCfg   = config_.networkConfig;

    if(rwBuffer.IsReadInitialized())
        return true;

    if(!rwBuffer.InitReadBuffer(netCfg.readBufferIncSize)) {
        logger_.Error("[Epoll]: Failed to init read buffer");
        Close(ctx);
        return false;
    }
    return true;
}

bool EpollConnectionHandler::ResolveHost(
    const char* host, const char* port, sockaddr_storage* outAddr, socklen_t* outLen
) {
    addrinfo  hints = {0};
    addrinfo* res   = nullptr;

    hints.ai_family   = AF_UNSPEC;     // Allow both IPv4 and IPv6
    hints.ai_socktype = SOCK_STREAM;   // TCP
    hints.ai_flags    = AI_ADDRCONFIG; // Only return addresses compatible with local interfaces

    int ret = getaddrinfo(host, port, &hints, &res);
    if(ret != 0)
        return false;

    bool found = false;
    if(res != nullptr) {
        // Sanity check to prevent buffer overflow
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

bool EpollConnectionHandler::ResolveIP(const sockaddr_storage& addr, WFXIpAddress& out)
{
    const sockaddr* sa = reinterpret_cast<const sockaddr*>(&addr);

    switch(sa->sa_family) {
        case AF_INET:
        {
            const auto* v4 = reinterpret_cast<const sockaddr_in*>(sa);
            out.ip.v4 = v4->sin_addr;
            out.type  = AF_INET;
            return true;
        }
        case AF_INET6:
        {
            const auto* v6 = reinterpret_cast<const sockaddr_in6*>(sa);
            out.ip.v6 = v6->sin6_addr;
            out.type  = AF_INET6;
            return true;
        }
        default:
            return false;
    }
}

void EpollConnectionHandler::Receive(ConnectionContext* ctx)
{
    WFX_TRACE();

    // Ensure buffer is ready
    if(!EnsureReadReady(ctx))
        return;
    
    auto& rwBuffer = ctx->rwBuffer;
    bool  gotData  = false;

    // Drain loop (ET mode: must read until EAGAIN)
    while(true) {
        ValidRegion region = rwBuffer.GetWritableReadRegion();
        if(!region.ptr || region.len == 0) {
            if(!rwBuffer.GrowReadBuffer(config_.networkConfig.readBufferIncSize,
                                        config_.networkConfig.maxReadBufferSize)) {
                logger_.Warn("[Epoll]: Read buffer full, closing connection");
                Close(ctx);
                return;
            }
            region = rwBuffer.GetWritableReadRegion();
        }

        ssize_t res = WrapRead(ctx, region.ptr, region.len);
        // Fully handle SSL + TCP edge-triggered
        if(res > 0) {
            rwBuffer.AdvanceReadLength(res);
            gotData = true;
        }
        // Connection closed by peer
        else if(res == 0) {
            Close(ctx);
            return;
        }
        // res < 0
        else {
            // Done reading for now, wait for more data in future
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                ctx->eventType = EventType::EVENT_RECV;
                break;
            }
            
            // Fatal error
            Close(ctx);
            return;
        }
    }

    // Notify app
    if(gotData)
        onReceive_(ctx);
}

void EpollConnectionHandler::SendFile(ConnectionContext* ctx)
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
    int   fd       = fileInfo.fd;

    while(fileInfo.offset < fileInfo.fileSize) {
        ssize_t n = WrapFile(ctx, fd, &fileInfo.offset, fileInfo.fileSize - fileInfo.offset);
        // Try to send more of file
        if(n > 0)
            continue;

        if(n < 0) {
            // Check if we are switching to streaming mode
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
        ctx->ClearContext();
        ResumeReceive(ctx);
    }
}

void EpollConnectionHandler::ResumeStream(ConnectionContext* ctx)
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

    writeMeta->dataLength    = 0;
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
    char*       chunkPtr = !ctx->streamChunked ? writeRegion.ptr : writeRegion.ptr + chunkHeaderReserve;
    std::size_t chunkCap = !ctx->streamChunked ? writeRegion.len : writeRegion.len - chunkHeaderReserve - 2;

    auto streamResult = ctx->streamGenerator.Next(ctx->streamGenerator.ctx, {chunkPtr, chunkCap});

    // Refresh timeout everytime a chunk is sent
    RefreshExpiry(ctx, config_.networkConfig.idleTimeout);

    switch(streamResult.action) {
        case StreamAction::CONTINUE:
        {
            // The actual rwbuffer allows chunks only upto uint32 max only, if its 0 or > uint32 max-
            // -its an invalid / corrupted output, 'Close' connection
            if(streamResult.writtenBytes == 0 || streamResult.writtenBytes > UINT32_MAX) {
                Close(ctx);
                return;
            }

            // No need to add all the stuff, just send it as is
            if(!ctx->streamChunked) {
                writeMeta->dataLength = streamResult.writtenBytes;
                Write(ctx);
                return;
            }

            // Write chunk header to an intermediate buffer first
            char chunkHeader[chunkHeaderReserve + 1] = { 0 };
            int headerLen = snprintf(
                chunkHeader, chunkHeaderReserve, "%zX\r\n", streamResult.writtenBytes
            );
            if(headerLen <= 0 || headerLen >= chunkHeaderReserve) {
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

            Write(ctx);
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

    // Storing value before resetting it below
    bool wasChunked = static_cast<bool>(ctx->streamChunked);

    // Only STOP_AND_... states can reach here
    writeMeta->dataLength    = 0;
    writeMeta->writtenLength = 0;
    ctx->isStreamOperation   = 0;
    ctx->streamChunked       = 0;
    ctx->streamGenerator     = {};

    // Write final chunk or finalize stream
    if(wasChunked)
        rwBuffer.AppendWriteData(
            CHUNK_END,
            sizeof(CHUNK_END) - 1,
            config_.networkConfig.sendBufferIncSize,
            config_.networkConfig.maxSendBufferSize
        )
            ? Write(ctx)
            : Close(ctx);

    else if(ctx->GetConnectionState() == ConnectionState::CONNECTION_ALIVE) {
        ctx->ClearContext();
        ResumeReceive(ctx);
    }

    else Close(ctx);
}

void EpollConnectionHandler::HandleAsyncCallback(ConnectionContext* ctx, AsyncResult res)
{
    WFX_TRACE();

    if(ctx->asyncOnDone) {
        auto cb = ctx->asyncOnDone;
        auto ud = ctx->asyncUserData;

        ctx->asyncOnDone   = nullptr;
        ctx->asyncUserData = nullptr;

        cb(ud, res);
    }
}

void EpollConnectionHandler::HandleTimeoutTimer(int sfd)
{
    // TODO: Both 'HandleTimeoutTimer' and 'HandleAsyncTimer' must properly handle read return value
    std::uint64_t expirations = 0;
    (void)read(sfd, &expirations, sizeof(expirations));

    // Calculate elapsed time since the server started in seconds
    std::uint64_t nowSec = NowMs() / 1000;

    timerWheel_.Tick(nowSec);

    logger_.Info("<TimeoutTimer>: ", numConnectionsAlive_, ' ', nowSec);
}

void EpollConnectionHandler::HandleAsyncTimer(int sfd)
{
    std::uint64_t expirations = 0;
    (void)read(sfd, &expirations, sizeof(expirations));

    std::uint64_t newTick = NowMs();
    std::uint64_t connId  = 0;

    while(timerHeap_.PopExpired(newTick, connId)) {
        ConnectionContext* ctx = connections_.GetPtr(connId);
        ctx->isAsyncTimerOperation = 0;

        HandleAsyncCallback(ctx, {nullptr, 0, MiddlewareAction::CONTINUE, AsyncStatus::COMPLETED});
    }

    // Because the async timer is one shot, update it just in case there exists more async-
    // -registered timers
    UpdateAsyncTimer();

    logger_.Info("<AsyncTimer>: ", numConnectionsAlive_, ' ', newTick);
}

void EpollConnectionHandler::HandleHandshake(ConnectionContext* ctx, std::uint32_t ev)
{
    WFX_TRACE();

    // For now, 'Endpoint' types will always try to 'SEND' first. Will be changed later
    // However, client connection will need to 'RECV' first because... i mean, without it how will-
    // -server know what to respond with?
    EventType onSuccess = ctx->IsEndpoint() ? EventType::EVENT_SEND : EventType::EVENT_RECV;

    if(!TryHandshake(ctx, onSuccess)) {
        Close(ctx);
        return;
    }

    // Wait for handshake to finish
    if(ctx->eventType == EventType::EVENT_HANDSHAKE)
        return;

    // Handshake finished, we either send data immediately, or read data immediately if EPOLLIN is set
    if(ctx->eventType == EventType::EVENT_SEND)
        Write(ctx, {});
    else if(ev & EPOLLIN)
        Receive(ctx);

    // In other cases we just wait for data
}

void EpollConnectionHandler::HandleWriteReady(ConnectionContext* ctx, std::uint32_t ev)
{
    WFX_TRACE();

    switch(ctx->eventType) {
        case EventType::EVENT_SEND:
            Write(ctx, {});
            break;

        case EventType::EVENT_SEND_FILE:
            SendFile(ctx);
            break;

        case EventType::EVENT_CONNECT:
        {
            int err = 0;
            socklen_t len = sizeof(err);

            if(getsockopt(ctx->socket, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
                Close(ctx);
                break;
            }

            // Finally its connected, now for SSL endpoints, we do SSL handshake
            if(ctx->GetEndpointState() == EndpointState::ENDPOINT_SECURE) {
                ctx->sslConn = sslHandler_->WrapClient(
                    ctx->socket,
                    endpoints_[ctx->endpointIdx].first.host.c_str()
                );

                if(!ctx->sslConn) {
                    Close(ctx);
                    break;
                }

                HandleHandshake(ctx, ev);
                break;
            }

            // Plain endpoints proceed directly to write
            Write(ctx, {});
        }
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

    std::uint64_t now     = NowMs();
    std::uint64_t expire  = min->delay;
    std::uint64_t remain  = (expire <= now) ? 1 : (expire - now);

    itimerspec ts{};
    ts.it_value.tv_sec  = remain / 1000;               // |
    ts.it_value.tv_nsec = (remain % 1000) * 1'000'000; // |-> Hopefully compiler can optimize '/' and '%' without me doing '(remain * 0x4189375A) >> 42'
    ts.it_interval      = {0, 0}; // Timer is one shot

    while(timerfd_settime(asyncTimerFd_, 0, &ts, nullptr) < 0) {
        if(errno == EINTR)
            continue;
        logger_.Error("[Epoll]: Failed to set async timer: ", strerror(errno));
        break;
    }
}

std::uint64_t EpollConnectionHandler::PackEpollData(ConnectionContext* ctx)
{
    bool isEndpoint = ctx->IsEndpoint();

    // For a client connection, 'EndpointIdx' will always be 'CLIENT_CONNECTION_TAG'
    std::uint16_t tag  = isEndpoint ? ctx->endpointIdx : CLIENT_CONNECTION_TAG;
    auto&         pool = isEndpoint ? endpoints_[ctx->endpointIdx].second : connections_;

    std::uint32_t idx = pool.GetIndex(ctx);

    // Pack => [( EndpointIdx (16) | GenerationID (16) ) and PoolIdx (Low 32)]
    return (static_cast<std::uint64_t>(tag)               << 48)
         | (static_cast<std::uint64_t>(ctx->generationId) << 32)
         | static_cast<std::uint64_t>(idx);
}

bool EpollConnectionHandler::RegisterEpoll(ConnectionContext* ctx, int op)
{
    // Poll once, then we just won't touch 'epoll_ctl' again till we close connection
    // We will use 'ctx->eventType' to control the flow of data pretty much, preventing-
    // -any sort of race condition and such
    // NOTE: For deletion cases, event must be 'nullptr'
    epoll_event ev{};
    epoll_event* evPtr = nullptr;

    if(op != EPOLL_CTL_DEL) {
        ev.events   = EPOLLIN | EPOLLOUT | EPOLLET;
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

bool EpollConnectionHandler::TryHandshake(ConnectionContext* ctx, EventType onSuccess)
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

bool EpollConnectionHandler::CreateAndConnect(ConnectionContext* ctx, EndpointContext& epCtx)
{
    // IMP: No error handling is done, it is expected that caller will do so (in this case its not-
    // -'WrapConnect', its the function calling 'WrapConnect')
    ctx->socket = socket(epCtx.addr.ss_family, SOCK_STREAM, 0);
    if(ctx->socket < 0)
        return false;

    if(!SetNonBlocking(ctx->socket))
        return false;

    while(true) {
        int ret = connect(ctx->socket, reinterpret_cast<const sockaddr*>(&epCtx.addr), epCtx.addrLen);
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
void EpollConnectionHandler::WrapAccept(ConnectionContext* ctx)
{
    WFX_TRACE();

    int clientFd = ctx->socket;

    if(useHttps_) {
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
    // Plain HTTP
    else
        ctx->eventType = EventType::EVENT_RECV;

    if(!RegisterEpoll(ctx, EPOLL_CTL_ADD)) {
        Close(ctx);
        return;
    }

    // Set an initial timeout for the new connection so they don't connect-
    // -and stay idle forever
    RefreshExpiry(ctx, config_.networkConfig.idleTimeout); 
}

EndpointStatus EpollConnectionHandler::WrapConnect(ConnectionContext* ctx, EndpointContainer& ecnt) 
{
    WFX_TRACE();

    // IMP: This won't release any pools or sockets (Never calls 'Close()'), it expects-
    // -caller function to do so. IT IS MANDATORY THAT ERROR HANDLING IS DONE BY CALLER OR EVENT LOOP
    auto& endpointCtx  = ecnt.first;
    auto& endpointPool = ecnt.second;

    if(!CreateAndConnect(ctx, endpointCtx))
        return EndpointStatus::CONNECT_FAILURE;

    // If we immediately connected ('eventType' is not EVENT_CONNECT) then try to SSL connect-
    // -if needed.
    if(ctx->eventType != EventType::EVENT_CONNECT
        && ctx->GetEndpointState() == EndpointState::ENDPOINT_SECURE)
    {
        ctx->sslConn = sslHandler_->WrapClient(ctx->socket, endpointCtx.host.c_str());
        if(!ctx->sslConn)
            return EndpointStatus::SSL_FAILURE;

        if(!TryHandshake(ctx, EventType::EVENT_SEND))
            return EndpointStatus::SSL_FAILURE;
    }

    if(!RegisterEpoll(ctx, EPOLL_CTL_ADD))
        return EndpointStatus::INTERNAL_ERROR;

    // 'EVENT_SEND' (SSL or Non-SSL completed) or 'EVENT_HANDSHAKE' (SSL needs to complete)-
    // -means socket is usable now. 'MOD' re-evaluates fd state in ET mode so the edge fires immediately-
    // -in the event loop, which then calls 'Write()' or continues handshake
    // 'EVENT_CONNECT' skips this as 'EPOLLOUT' fires naturally when OS completes it in event loop
    if((ctx->eventType != EventType::EVENT_CONNECT)
        && !RegisterEpoll(ctx, EPOLL_CTL_MOD))
        return EndpointStatus::INTERNAL_ERROR;

    RefreshExpiry(ctx, config_.networkConfig.idleTimeout);
    return EndpointStatus::PENDING;
}

ssize_t EpollConnectionHandler::WrapRead(ConnectionContext* ctx, char* buf, std::size_t len)
{
    if(!ctx->sslConn)
        return ::recv(ctx->socket, buf, len, 0);

    SSLResult result = sslHandler_->Read(ctx->sslConn, buf, static_cast<int>(len));

    switch(result.error) {
        case SSLReturn::SUCCESS:
            return result.res;
        case SSLReturn::WANT_READ:
        case SSLReturn::WANT_WRITE:
            errno = EAGAIN;
            return -1;
        case SSLReturn::CLOSED:
            return 0;
        case SSLReturn::SYSCALL:
            return -1; // errno is already set by SSL
        case SSLReturn::FATAL:
        default:
            errno = EIO;
            return -1;
    }
}

ssize_t EpollConnectionHandler::WrapWrite(ConnectionContext* ctx, const char* buf, std::size_t len)
{
    if(!ctx->sslConn)
        return ::send(ctx->socket, buf, len, MSG_NOSIGNAL);

    SSLResult result = sslHandler_->Write(ctx->sslConn, buf, static_cast<int>(len));

    switch(result.error) {
        case SSLReturn::SUCCESS:
            return result.res;
        case SSLReturn::WANT_READ:
        case SSLReturn::WANT_WRITE:
            errno = EAGAIN;
            return -1;
        case SSLReturn::CLOSED:
            return 0;
        case SSLReturn::SYSCALL:
            return -1; // errno is already set by SSL
        case SSLReturn::FATAL:
        default:
            errno = EIO;
            return -1;
    }
}

ssize_t EpollConnectionHandler::WrapFile(ConnectionContext* ctx, int fd, off_t* offset, std::size_t count)
{
    if(!ctx->sslConn)
        return ::sendfile(ctx->socket, fd, offset, count);

    SSLResult result = sslHandler_->WriteFile(ctx->sslConn, fd, offset ? *offset : 0, count);

    switch(result.error) {
        // Switch to streaming mode with Write instead
        // Stream will uses a non chunked mode of transferring files (cuz we already sent the header)
        // And we have access to FileInfo struct anyways (its guaranteed initialized so yeah)
        case SSLReturn::NO_IMPL:
            ctx->isFileOperation   = 0;
            ctx->isStreamOperation = 1;
            ctx->streamChunked     = 0;
            ctx->streamGenerator = {
                // ctx
                &ctx->fileInfo,

                // Next
                [](void* c, StreamBuffer buffer) -> StreamResult {
                    auto* fileInfo = static_cast<FileInfo*>(c);

                    ssize_t res = pread(fileInfo->fd, buffer.buffer, buffer.size, fileInfo->offset);

                    // Error or EOF
                    if(res <= 0) {
                        return StreamResult{
                            0, res == 0
                                ? StreamAction::STOP_AND_ALIVE_CONN
                                : StreamAction::STOP_AND_CLOSE_CONN
                        };
                    }

                    // Success
                    fileInfo->offset += res;

                    return StreamResult{
                        static_cast<std::size_t>(res),
                        StreamAction::CONTINUE
                    };
                },

                // Destroy (not needed)
                nullptr
            };

            // Signal to caller that streaming mode is engaged
            return SWITCH_FILE_TO_STREAM;

        case SSLReturn::SUCCESS:
            if(offset)
                *offset += result.res; // Manually track progress
            return result.res;
        case SSLReturn::WANT_READ:
        case SSLReturn::WANT_WRITE:
            errno = EAGAIN;
            return -1;
        case SSLReturn::CLOSED:
            return 0;
        case SSLReturn::SYSCALL:
            return -1; // errno already set by SSL
        case SSLReturn::FATAL:
        default:
            errno = EIO;
            return -1;
    }
}

} // namespace WFX::OSSpecific

#endif // !WFX_LINUX_USE_IO_URING