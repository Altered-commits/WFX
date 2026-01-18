#ifndef WFX_LINUX_USE_IO_URING

#include "epoll_connection.hpp"

#include "http/common/http_error_msgs.hpp"
#include "http/common/http_global_state.hpp"
#include "http/ssl/http_ssl_factory.hpp"
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
void EpollConnectionHandler::Initialize(const std::string& host, int port)
{
    auto& osConfig      = config_.osSpecificConfig;
    auto& networkConfig = config_.networkConfig;

    // Maximum valid 64-aligned slot count for uint32_t
    constexpr std::uint32_t MAX_64_ALIGNED = 0xFFFF'FFC0u;

    // Round up to 64-bit boundary so every allocated bit always maps to valid storage
    // In simpler words, it rounds to the next 64 divisible number pretty much
    std::uint64_t rounded = std::uint64_t(networkConfig.maxConnections) + 63;
    rounded &= ~std::uint64_t(63);

    // Clamp to avoid exceeding valid range
    if(rounded > MAX_64_ALIGNED)
        rounded = MAX_64_ALIGNED;

    connSlots_ = std::uint32_t(rounded);
    connWords_ = connSlots_ >> 6;

    // Connections
    connections_ = std::make_unique<ConnectionContext[]>(connSlots_);
    connBitmap_  = std::make_unique<std::uint64_t[]>(connWords_);
    // Events
    events_      = std::make_unique<epoll_event[]>(maxEvents_);

    // Idk but shits necessary btw, need zeroed out stuff or 'AllocSlot' stuff dies
    std::fill_n(connBitmap_.get(), connWords_, 0);

    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if(listenFd_ < 0)
        logger_.Fatal("[Epoll]: Failed to create listening socket: ", strerror(errno));

    int opt = 1;
    if(setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        logger_.Fatal("[Epoll]: Failed to set SO_REUSEADDR: ", strerror(errno));

    if(setsockopt(listenFd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0)
        logger_.Fatal("[Epoll]: Failed to set SO_REUSEPORT: ", strerror(errno));
    
    if(!SetNonBlocking(listenFd_))
        logger_.Fatal("[Epoll]: Failed to make listening socket non-blocking: ", strerror(errno));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if(!ResolveHostToIpv4(host.c_str(), &addr.sin_addr))
        logger_.Fatal("[Epoll]: Failed to resolve host '", host, '\'');

    if(bind(listenFd_, (sockaddr*)&addr, sizeof(addr)) < 0)
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
        connSlots_,
        1024, 1, TimeUnit::SECONDS,
        [this](std::uint32_t connId) {
            ConnectionContext* ctx = &connections_[connId];

            // So the logic behind the if condition is, in normal sync path, if a connection is marked-
            // -'close', it will trigger cleanup after it sent data so no need to clash with it
            // But on the other hand, in the async path, if a connections is marked 'close' and the callback,-
            // -for some odd reason, just hung up and isn't responding, we shouldn't care about connection atp
            // WE CLOSE IT OURSELVES
            if(
                ctx->GetConnectionState() != ConnectionState::CONNECTION_CLOSE
                || ctx->IsAsyncOperation()
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
    tev.data.fd = timeoutTimerFd_;
    if(epoll_ctl(epollFd_, EPOLL_CTL_ADD, timeoutTimerFd_, &tev) < 0)
        logger_.Fatal("[Epoll]: Failed to add timeout timer to epoll: ", strerror(errno));

    // vvv Initializing async timer vvv
    asyncTimerFd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if(asyncTimerFd_ < 0)
        logger_.Fatal("[Epoll]: Failed to create async timer: ", strerror(errno));

    epoll_event aev{};
    aev.events  = EPOLLIN;
    aev.data.fd = asyncTimerFd_;
    if(epoll_ctl(epollFd_, EPOLL_CTL_ADD, asyncTimerFd_, &aev) < 0)
        logger_.Fatal("[Epoll]: Failed to add async timer to epoll: ", strerror(errno));
}

void EpollConnectionHandler::SetEngineCallbacks(ReceiveCallback onData, CompletionCallback onComplete)
{
    onReceive_         = std::move(onData);
    onAsyncCompletion_ = std::move(onComplete);
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
            else {
                Close(ctx);
                return;
            }
        }
    }

__CleanupOrRearm:
    // Special case, stream operation, stream the content via streamGenerator
    if(ctx->streamGenerator) {
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

void EpollConnectionHandler::Stream(ConnectionContext* ctx, StreamGenerator generator, bool streamChunked)
{
    // Sanity checks
    if(!generator) {
        logger_.Error("[Epoll]: Stream() called with null generator");
        Close(ctx);
        return;
    }

    // Store the generator function in context for future use
    ctx->streamGenerator = std::move(generator);

    // For streaming operations, we first want to finish writing out headers-
    // -and mark it as stream operation, so when 'Write' completes, it should-
    // -start the streaming process
    ctx->isStreamOperation = 1;
    ctx->streamChunked     = streamChunked;
    Write(ctx, {});
}

void EpollConnectionHandler::Close(ConnectionContext* ctx, bool forceClose)
{
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
    epoll_ctl(epollFd_, EPOLL_CTL_DEL, ctx->socket, nullptr);
    ReleaseConnection(ctx);
}

// vvv Main Functions vvv
void EpollConnectionHandler::Run()
{
    // Just a simple sanity check before we do anything
    if(!onReceive_ || !onAsyncCompletion_)
        logger_.Fatal(
            "[Epoll]: Member 'onReceive_' or 'onAsyncCompletion_' was not initialized."
            " Call 'SetEngineCallbacks' before calling 'Run'"
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
            std::uint32_t gen  = meta >> 32;

            // Existing connection, handle it
            if(gen > 0)
                goto __HandleExistingConnection;

            sfd = events_[i].data.fd;

            // Handle timeouts timers
            if(sfd == timeoutTimerFd_) {
                // We just need to drain the sfd, we dont care about the 'expirations' value
                std::uint64_t expirations = 0;
                (void)read(sfd, &expirations, sizeof(expirations));

                // Calculate elapsed time since the server started in seconds
                std::uint64_t nowSec = NowMs() / 1000;

                timerWheel_.Tick(nowSec);

                logger_.Info("<TimeoutTimer>: ", numConnectionsAlive_, ' ', nowSec);
                continue;
            }

            // Handle async timers
            if(sfd == asyncTimerFd_) {
                std::uint64_t expirations = 0;
                (void)read(sfd, &expirations, sizeof(expirations));

                std::uint64_t newTick = NowMs();
                std::uint64_t connId  = 0;

                while(timerHeap_.PopExpired(newTick, connId)) {
                    ConnectionContext* ctx = &connections_[connId];

                    // Well, we are done with our timer operation so yeah
                    ctx->isAsyncTimerOperation = 0;

                    switch(ctx->TryFinishCoroutines()) {
                        case Async::Status::COMPLETED:
                            onAsyncCompletion_(ctx);
                            break;

                        // Errors
                        case Async::Status::TIMER_FAILURE:
                        case Async::Status::IO_FAILURE:
                        case Async::Status::INTERNAL_FAILURE:
                            ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
                            Write(ctx, HttpError::internalError);
                            break;
                    }
                }

                // Because the async timer is one shot, update it just in case there exists more async-
                // -registered timers
                UpdateAsyncTimer();

                logger_.Info("<AsyncTimer>: ", numConnectionsAlive_, ' ', newTick);
                continue;
            }

            // Accept new connections
            if(sfd == listenFd_) {
                while(true) {
                    sockaddr_in addr{};
                    socklen_t len = sizeof(addr);
                    
                    int clientFd = accept4(listenFd_, (sockaddr*)&addr, &len, SOCK_NONBLOCK);
                    if(clientFd < 0) {
                        if(errno == EAGAIN || errno == EWOULDBLOCK)
                            break; // Queue drained, stop
                        else
                            continue; // Transient error, skip this one
                    }

                    // // Disable Nagle's algorithm. Send small packets without buffering
                    // int flag = 1;
                    // setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

                    // Extract IP info first
                    WFXIpAddress tmpIp;
                    sockaddr* sa = reinterpret_cast<sockaddr*>(&addr);
                    if(sa->sa_family == AF_INET) {
                        tmpIp.ip.v4  = reinterpret_cast<sockaddr_in*>(sa)->sin_addr;
                        tmpIp.ipType = AF_INET;
                    }
                    else if(sa->sa_family == AF_INET6) {
                        tmpIp.ip.v6  = reinterpret_cast<sockaddr_in6*>(sa)->sin6_addr;
                        tmpIp.ipType = AF_INET6;
                    }
                    // Garbage IPs not allowed, close connection
                    else {
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
            std::uint32_t idx = meta & 0xFFFFFFFF;
            ConnectionContext* ctx = &connections_[idx];

            // If the slot's current generation doesn't match the event's generation, it means-
            // -this event is for a dead connection
            if(ctx->generationId != gen) 
                continue;

            // SSL handshake in progress
            if(ctx->eventType == EventType::EVENT_HANDSHAKE) {
                SSLReturn hsResult = sslHandler_->Handshake(ctx->sslConn);

                switch(hsResult) {
                    case SSLReturn::SUCCESS:
                        // Handshake done, switch to EVENT_RECV as we are ready to read data
                        ctx->eventType = EventType::EVENT_RECV;

                        // Try to immediately read if we have any pending requests
                        if(ev & EPOLLIN)
                            Receive(ctx);

                        break;

                    // Handshake isn't finished, wait for more events
                    case SSLReturn::WANT_READ:
                    case SSLReturn::WANT_WRITE:
                        break;

                    // Any error or closed connection
                    case SSLReturn::CLOSED:
                    case SSLReturn::SYSCALL:
                    case SSLReturn::FATAL:
                    default:
                        Close(ctx);
                        break;
                }

                continue; // Handshake handled, skip further processing
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
                    // Cannot call 'Close' cuz its not needed simply
                    case SSLReturn::SUCCESS:
                    case SSLReturn::FATAL:
                    default:
                        ctx->sslConn = nullptr;
                        epoll_ctl(epollFd_, EPOLL_CTL_DEL, ctx->socket, nullptr);
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
            
            if(ev & EPOLLOUT) {
                if(ctx->eventType == EventType::EVENT_SEND_FILE)
                    SendFile(ctx);
                else if(ctx->eventType == EventType::EVENT_SEND)
                    Write(ctx, {});
            }
        }
    }
}

void EpollConnectionHandler::RefreshExpiry(ConnectionContext* ctx, std::uint16_t timeoutSeconds)
{
    std::uint32_t idx = ctx - &connections_[0];
    timerWheel_.Schedule(idx, timeoutSeconds);
}

bool EpollConnectionHandler::RefreshAsyncTimer(ConnectionContext* ctx, std::uint32_t delayMilliseconds)
{
    std::uint32_t idx    = ctx - &connections_[0];
    std::uint64_t expire = NowMs() + delayMilliseconds;

    // Timers are coalesced if they fall within +-10ms of each other
    if(!timerHeap_.Insert(idx, expire, 10)) {
        logger_.Warn("[Epoll]: Failed to refresh async timer");
        return false;
    }

    ctx->isAsyncTimerOperation = 1;
    UpdateAsyncTimer();

    return true;
}

void EpollConnectionHandler::Stop()
{
    running_ = false;
}

// vvv Helper Functions vvv
//  --- Connection Handlers ---
std::int64_t EpollConnectionHandler::AllocSlot(std::uint64_t* bitmap, std::uint32_t numWords)
{
    // Rn this is purely used for Connection slot handling, but still made a seperate function-
    // -incase in future i need to use this for other common stuff
    std::uint32_t w = connLastIndex_;

    // Primary scan: from last index to end
    for(; w < numWords; ++w) {
        std::uint64_t inv = ~bitmap[w];
        if(inv) {
            int bit = __builtin_ctzll(inv);
            bitmap[w] |= 1ULL << bit;
            connLastIndex_ = w;
            return (std::int64_t(w) << 6) + bit;
        }
    }

    // Wrap around scan: from start to old index
    w = 0;
    for(; w < connLastIndex_; ++w) {
        std::uint64_t inv = ~bitmap[w];
        if(inv) {
            int bit = __builtin_ctzll(inv);
            bitmap[w] |= 1ULL << bit;
            connLastIndex_ = w;
            return (std::int64_t(w) << 6) + bit;
        }
    }

    return -1; // Fully exhausted
}

void EpollConnectionHandler::FreeSlot(std::uint64_t* bitmap, std::uint32_t idx)
{
    std::uint32_t w   = idx >> 6;
    std::uint32_t bit = idx & 63;
    bitmap[w] &= ~(1ULL << bit);
}

ConnectionContext* EpollConnectionHandler::GetConnection()
{
    std::int64_t idx = AllocSlot(connBitmap_.get(), connWords_);
    if(idx < 0)
        return nullptr;

    auto* ctx = &connections_[idx];
    ctx->generationId++;

    // If it wraps to 0, bump it to 1 cuz 0 is reserved for identifying fds such as Listen/Timer
    if(ctx->generationId == 0)
        ctx->generationId = 1;

    return ctx;
}

void EpollConnectionHandler::ReleaseConnection(ConnectionContext* ctx)
{
    if(!ctx)
        return;

    // For debugging purposes
    numConnectionsAlive_--;

    // Slot index is [current pointer] - [base pointer]
    std::uint32_t idx = ctx - &connections_[0];

    // Cancelling timer in 'Close' kinda sucks cuz during SSL async shutdown-
    // -the client might bail, never finish it, and we just be stuck in-
    // -closing state forever aaand timeout won't do anything cuz... we cancelled it
    timerWheel_.Cancel(idx);

    if(ctx->isAsyncTimerOperation) {
        if(timerHeap_.Remove(idx))
            UpdateAsyncTimer();
        else
            logger_.Warn("[Epoll]: Failed to cancel async timer");
    }

    if(ctx->socket > 0)
        close(ctx->socket);

    ipLimiter_.ReleaseConnection(ctx->connInfo);

    ctx->ResetContext();

    FreeSlot(connBitmap_.get(), idx);
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

    if(!ctx->fileInfo)
        ctx->fileInfo = new FileInfo{};
    
    auto* fileInfo = ctx->fileInfo;
    fileInfo->fd       = fd;
    fileInfo->offset   = 0;
    fileInfo->fileSize = size;

    return true;
}

bool EpollConnectionHandler::EnsureReadReady(ConnectionContext* ctx)
{
    auto& rwBuffer = ctx->rwBuffer;
    auto& netCfg   = config_.networkConfig;

    if(rwBuffer.IsReadInitialized())
        return true;

    if(!rwBuffer.InitReadBuffer(netCfg.bufferIncrSize)) {
        logger_.Error("[Epoll]: Failed to init read buffer");
        Close(ctx);
        return false;
    }
    return true;
}

bool EpollConnectionHandler::ResolveHostToIpv4(const char* host, in_addr* outAddr)
{
    addrinfo hints = { 0 };
    addrinfo *res = nullptr, *rp = nullptr;

    hints.ai_family   = AF_INET;       // Force IPv4
    hints.ai_socktype = SOCK_STREAM;   // TCP style (doesn't really matter here)
    hints.ai_flags    = AI_ADDRCONFIG; // Use only configured addr families

    int ret = getaddrinfo(host, NULL, &hints, &res);
    if(ret != 0)
        return false;

    // Pick the first IPv4 result
    bool found = false;
    for(rp = res; rp != NULL; rp = rp->ai_next) {
        if(rp->ai_family == AF_INET) {
            sockaddr_in* addr = (sockaddr_in*)rp->ai_addr;
            *outAddr = addr->sin_addr; // Copy the IPv4 address
            found = true;
            break;
        }
    }

    freeaddrinfo(res);
    return found; 
}

void EpollConnectionHandler::Receive(ConnectionContext* ctx)
{
    // Ensure buffer is ready
    if(!EnsureReadReady(ctx))
        return;
    
    auto& rwBuffer = ctx->rwBuffer;
    bool  gotData  = false;

    // Drain loop (ET mode: must read until EAGAIN)
    while(true) {
        ValidRegion region = rwBuffer.GetWritableReadRegion();
        if(!region.ptr || region.len == 0) {
            if(!rwBuffer.GrowReadBuffer(config_.networkConfig.bufferIncrSize,
                                        config_.networkConfig.maxRecvBufferSize)) {
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
    // This is called in this order: WriteFile() -> Write() [Headers sent] -> SendFile()
    // This expects fileInfo to be constructed and set beforehand
    // If not, its UB. GG
    if(!ctx->fileInfo) {
        logger_.Warn("[Epoll]: SendFile expects ctx->fileInfo to be set, got nullptr");
        ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
        Write(ctx, HttpError::internalError);
        return;
    }

    auto* fileInfo = ctx->fileInfo;
    int   fd       = fileInfo->fd;

    while(fileInfo->offset < fileInfo->fileSize) {
        ssize_t n = WrapFile(ctx, fd, &fileInfo->offset,
                               fileInfo->fileSize - fileInfo->offset);
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
    // Paranoia check
    if(!ctx->streamGenerator) {
        logger_.Warn("[Epoll]: 'streamGenerator' function called but is nullptr");
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

    auto streamResult = ctx->streamGenerator({ chunkPtr, chunkCap });

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
    // Now its not necessary to reset "ALL" the data here but uk, fun
    writeMeta->dataLength    = 0;
    writeMeta->writtenLength = 0;
    ctx->isStreamOperation   = 0;
    ctx->streamChunked       = 0;
    ctx->streamGenerator     = {};

    // Write final chunk or finalize stream
    if(wasChunked)
        rwBuffer.AppendData(CHUNK_END, sizeof(CHUNK_END) - 1)
            ? Write(ctx)
            : Close(ctx);

    else if(ctx->GetConnectionState() == ConnectionState::CONNECTION_ALIVE) {
        ctx->ClearContext();
        ResumeReceive(ctx);
    }

    else Close(ctx);
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

void EpollConnectionHandler::WrapAccept(ConnectionContext* ctx)
{
    // Poll once, then we just won't touch epoll_ctl again till we close connection
    // We will use 'ctx->eventType' to control the flow of data pretty much, preventing-
    // -any sort of race condition and such
    epoll_event cev{};
    cev.events   = EPOLLIN | EPOLLOUT | EPOLLET;

    // Pack GenerationID (High 32) and Index (Low 32)
    std::uint32_t idx = static_cast<std::uint32_t>(ctx - connections_.get());
    cev.data.u64 = (static_cast<std::uint64_t>(ctx->generationId) << 32) | idx;

    int clientFd = ctx->socket;

    if(useHttps_) {
        ctx->sslConn = sslHandler_->Wrap(clientFd);
        if(!ctx->sslConn) {
            Close(ctx);
            return;
        }

        // Try handshake immediately
        SSLReturn hsResult = sslHandler_->Handshake(ctx->sslConn);

        // Handshake done, check if its finished or still remaining
        switch(hsResult) {
            case SSLReturn::SUCCESS:
                ctx->eventType = EventType::EVENT_RECV;
                break;

            case SSLReturn::WANT_READ:
            case SSLReturn::WANT_WRITE:
                ctx->eventType = EventType::EVENT_HANDSHAKE;
                break;
            
            // Handshake failed or connection closed
            default:
                Close(ctx);
                return;
        }
    }
    // Plain HTTP
    else
        ctx->eventType = EventType::EVENT_RECV;

    if(epoll_ctl(epollFd_, EPOLL_CTL_ADD, clientFd, &cev) < 0) {
        Close(ctx);
        return;
    }

    // Set an initial timeout for the new connection so they don't connect-
    // -and stay idle forever
    RefreshExpiry(ctx, config_.networkConfig.idleTimeout); 
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
            ctx->streamGenerator   = [
                fileInfo = ctx->fileInfo
            ](StreamBuffer buffer) {
                std::int64_t res = pread(fileInfo->fd, buffer.buffer, buffer.size, fileInfo->offset);
                // Error or EOF
                if(res <= 0)
                    return StreamResult{ 
                        0, res == 0
                            ? StreamAction::STOP_AND_ALIVE_CONN
                            : StreamAction::STOP_AND_CLOSE_CONN
                    };

                // No error
                fileInfo->offset += res;
                return StreamResult{ static_cast<std::size_t>(res), StreamAction::CONTINUE };
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