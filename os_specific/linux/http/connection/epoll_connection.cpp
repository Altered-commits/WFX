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
    if(listenFd_ > 0) { close(listenFd_); listenFd_ = -1; }
    if(timerFd_ > 0)  { close(timerFd_); timerFd_ = -1; }
    if(epollFd_ > 0)  { close(epollFd_); epollFd_ = -1; }

    logger_.Info("[Epoll]: Cleaned up resources successfully");
}

// vvv Initializing Functions vvv
void EpollConnectionHandler::Initialize(const std::string& host, int port)
{
    auto& osConfig      = config_.osSpecificConfig;
    auto& networkConfig = config_.networkConfig;

    // Connections
    connWords_   = (networkConfig.maxConnections + 63) / 64;
    connections_ = std::make_unique<ConnectionContext[]>(networkConfig.maxConnections);
    connBitmap_  = std::make_unique<std::uint64_t[]>(connWords_);
    // Events
    events_      = std::make_unique<epoll_event[]>(maxEvents_);

    // Idk
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

    // Initialize timeout handler
    timerWheel_.Init(
        networkConfig.maxConnections,
        128,               // Number of slots, power of 2
        1,                 // 1 tick = 1 second
        TimeUnit::Seconds
    );
    
    timerFd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if(timerFd_ < 0)
        logger_.Fatal("[Epoll]: Failed to create timer: ", strerror(errno));

    itimerspec ts{};
    ts.it_interval.tv_sec  = INVOKE_TIMEOUT_COOLDOWN;
    ts.it_interval.tv_nsec = 0;
    ts.it_value.tv_sec     = INVOKE_TIMEOUT_DELAY;
    ts.it_value.tv_nsec    = 0;

    if(timerfd_settime(timerFd_, 0, &ts, nullptr) < 0)
        logger_.Fatal("[Epoll]: Failed to set timer: ", strerror(errno));

    epoll_event tev{};
    tev.events  = EPOLLIN;
    tev.data.fd = timerFd_;
    if(epoll_ctl(epollFd_, EPOLL_CTL_ADD, timerFd_, &tev) < 0)
        logger_.Fatal("[Epoll]: Failed to add timerfd to epoll: ", strerror(errno));
}

void EpollConnectionHandler::SetReceiveCallback(ReceiveCallback onData)
{
    onReceive_ = std::move(onData);
}

// vvv I/O Operations vvv
void EpollConnectionHandler::ResumeReceive(ConnectionContext* ctx)
{
    if(!EnsureReadReady(ctx))
        return;

    PollAgain(ctx, EventType::EVENT_RECV, EPOLLIN | EPOLLET);
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
            
            else if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                PollAgain(ctx, EventType::EVENT_SEND, EPOLLOUT | EPOLLET);
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
        Write(ctx, internalError);
        return;
    }

    // Cool so for file operation, we first send headers, mark it as file operation
    // So when 'Write' completes, it should send file without any issue
    ctx->isFileOperation = 1;
    Write(ctx, {});
}

void EpollConnectionHandler::Close(ConnectionContext* ctx, bool forceClose)
{
    // If a shutdown is already in progress, do nothing
    if(!ctx || ctx->isShuttingDown)
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
                std::uint32_t events = ((res == SSLReturn::WANT_READ) ? EPOLLIN : EPOLLOUT) | EPOLLET;
                PollAgain(ctx, EventType::EVENT_SHUTDOWN, events);
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
    if(!onReceive_)
        logger_.Fatal("[Epoll]: Member 'onReceive_' was not initialized. Call 'SetReceiveCallback' before calling 'Run'");

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
            int  fd = events_[i].data.fd;
            auto ev = events_[i].events;

            // Handle timeouts
            if(events_[i].data.fd == timerFd_) {
                // `expirations` = number of timer intervals that have elapsed since the last read
                // If the process was delayed or busy, multiple expirations may have accumulated
                // Reading resets the counter to 0, so we must advance the timer wheel by
                // (interval * expirations) to stay in sync with real time
                std::uint64_t expirations = 0;
                ssize_t s = read(timerFd_, &expirations, sizeof(expirations));
                
                if(expirations == 0 || s != sizeof(expirations))
                    continue;
                
                logger_.Info("<Debug Connections>: ", numConnectionsAlive_);
                
                // Timeout is done by TimerWheel which is O(1) if im not wrong
                std::uint64_t newTick = timerWheel_.GetTick() + (INVOKE_TIMEOUT_COOLDOWN * expirations);
                timerWheel_.Tick(newTick, [this](std::uint32_t connId) {
                                            ConnectionContext* ctx = &connections_[connId];
                                            if(ctx->GetConnectionState() != ConnectionState::CONNECTION_CLOSE)
                                                Close(ctx, true);
                                        });
                continue;
            }

            // Accept new connections
            if(fd == listenFd_) {
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
                    else
                        tmpIp.ipType = 255;

                    // Check limiter
                    if(!ipLimiter_.AllowConnection(tmpIp)) {
                        close(clientFd);
                        continue;
                    }

                    // Grab a connection slot
                    ConnectionContext* ctx = GetConnection();
                    if(!ctx) {
                        close(clientFd);
                        continue;
                    }

                    // Set connection info
                    ctx->socket    = clientFd;
                    ctx->connInfo  = tmpIp;
                    
                    numConnectionsAlive_++;
                    WrapAccept(ctx, clientFd);
                }
                continue;
            }
            
            // Handle existing connections
            auto* ctx = reinterpret_cast<ConnectionContext*>(events_[i].data.ptr);

            // If a connection is shutting down, ignore all events except the final SHUTDOWN-
            // -event itself. This prevents trying to read / write to a connection-
            // -that is bout to die anyways
            if(ctx->isShuttingDown && ctx->eventType != EventType::EVENT_SHUTDOWN)
                continue;

            // SSL handshake in progress
            if(ctx->eventType == EventType::EVENT_HANDSHAKE) {
                SSLReturn hsResult = sslHandler_->Handshake(ctx->sslConn);

                switch(hsResult) {
                    case SSLReturn::SUCCESS:
                        // Handshake done, switch to normal receive
                        PollAgain(ctx, EventType::EVENT_RECV, EPOLLIN | EPOLLET);

                        // Immediately read if EPOLLIN is set
                        if(ev & EPOLLIN)
                            Receive(ctx);
                        break;

                    case SSLReturn::WANT_READ:
                        // Need more input: wait for EPOLLIN
                        PollAgain(ctx, EventType::EVENT_HANDSHAKE, EPOLLIN | EPOLLET);
                        break;

                    case SSLReturn::WANT_WRITE:
                        // Need to write handshake data: wait for EPOLLOUT
                        PollAgain(ctx, EventType::EVENT_HANDSHAKE, EPOLLOUT | EPOLLET);
                        break;

                    case SSLReturn::CLOSED:
                    case SSLReturn::SYSCALL:
                    case SSLReturn::FATAL:
                    default:
                        // Any error or closed connection
                        Close(ctx);
                        break;
                }

                continue; // Handshake handled, skip further processing
            }
            
            // SSL shutdown is in progress
            if(ctx->eventType == EventType::EVENT_SHUTDOWN) {
                auto res = sslHandler_->Shutdown(ctx->sslConn);

                switch(res) {
                    case SSLReturn::SUCCESS:
                    case SSLReturn::FATAL:
                        ctx->sslConn = nullptr;
                        epoll_ctl(epollFd_, EPOLL_CTL_DEL, ctx->socket, nullptr);
                        ReleaseConnection(ctx);
                        break;

                    case SSLReturn::WANT_READ:
                        PollAgain(ctx, EventType::EVENT_SHUTDOWN, EPOLLIN | EPOLLET);
                        break;

                    case SSLReturn::WANT_WRITE:
                        PollAgain(ctx, EventType::EVENT_SHUTDOWN, EPOLLOUT | EPOLLET);
                        break;
                }
                continue;
            }

            if(ev & (EPOLLERR | EPOLLHUP)) {
                Close(ctx);
                continue;
            }

            if(ev & EPOLLIN) {
                Receive(ctx);
                continue;
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
    // Use array index as wheel position
    std::uint32_t idx = ctx - &connections_[0];
    timerWheel_.Schedule(idx, timeoutSeconds);
}

void EpollConnectionHandler::Stop()
{
    running_ = false;
}

// vvv Helper Functions vvv
//  --- Connection Handlers ---
std::int64_t EpollConnectionHandler::AllocSlot(
    std::uint64_t* bitmap, std::uint32_t numWords, std::uint32_t maxSlots
)
{
    for(std::uint32_t w = 0; w < numWords; w++) {
        std::uint64_t bits = bitmap[w];

        // If even a single '0' exists in the bitmap, we will take it
        // '0' means free slot
        if(~bits) {
            int          bit = __builtin_ctzll(~bits);
            std::int64_t idx = (w << 6) + bit;
            if(idx < maxSlots) {
                bitmap[w] |= (1ULL << bit);
                return idx;
            }
        }
    }
    return -1;
}

void EpollConnectionHandler::FreeSlot(std::uint64_t* bitmap, std::uint32_t idx)
{
    std::uint32_t w   = idx >> 6;
    std::uint32_t bit = idx & 63;
    bitmap[w] &= ~(1ULL << bit);
}

ConnectionContext* EpollConnectionHandler::GetConnection()
{
    std::int64_t idx = AllocSlot(connBitmap_.get(), connWords_, config_.networkConfig.maxConnections);
    if(idx < 0)
        return nullptr;

    return &connections_[idx];
}

void EpollConnectionHandler::ReleaseConnection(ConnectionContext* ctx)
{
    // Sanity checks
    if(!ctx)
        return;

    // For debugging purposes
    numConnectionsAlive_--;

    // Cancelling timer in 'Close' kinda sucks cuz during SSL async shutdown-
    // -the client might bail, never finish it, and we just be stuck in-
    // -closing state forever aaand timeout won't do anything cuz... we cancelled it
    std::uint32_t idx = ctx - &connections_[0];
    timerWheel_.Cancel(idx);

    if(ctx->socket > 0)
        close(ctx->socket);

    ipLimiter_.ReleaseConnection(ctx->connInfo);

    ctx->ResetContext();

    // Slot index is [current pointer] - [base pointer]
    FreeSlot(connBitmap_.get(), ctx - (&connections_[0]));
}

//  --- MISC Handlers ---
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

    if(!rwBuffer.InitReadBuffer(pool_, netCfg.bufferIncrSize)) {
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
            // Done reading for now
            if(errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            
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
        Close(ctx);
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
            // Partial progress, wait for EPOLLOUT again
            if(errno == EAGAIN || errno == EWOULDBLOCK)
                PollAgain(ctx, EventType::EVENT_SEND_FILE, EPOLLOUT | EPOLLET);
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

void EpollConnectionHandler::PollAgain(ConnectionContext* ctx, EventType eventType, std::uint32_t events)
{
    ctx->eventType = eventType;
            
    epoll_event ev{};
    ev.events   = events;
    ev.data.ptr = ctx;
    
    // If epoll fails, close the connection
    if(epoll_ctl(epollFd_, EPOLL_CTL_MOD, ctx->socket, &ev) < 0)
        Close(ctx);
}

void EpollConnectionHandler::WrapAccept(ConnectionContext *ctx, int clientFd)
{
    epoll_event cev{};
    cev.data.ptr = ctx;

    if(useHttps_) {
        ctx->sslConn = sslHandler_->Wrap(clientFd);
        if(!ctx->sslConn) {
            Close(ctx);
            return;
        }

        // Try handshake immediately
        SSLReturn hsResult = sslHandler_->Handshake(ctx->sslConn);

        // Handshake done, go to normal receive
        if(hsResult == SSLReturn::SUCCESS) {
            ctx->eventType = EventType::EVENT_RECV;
            cev.events = EPOLLIN | EPOLLET;
        }
        // Wait for read
        else if(hsResult == SSLReturn::WANT_READ) {
            ctx->eventType = EventType::EVENT_HANDSHAKE;
            cev.events = EPOLLIN | EPOLLET;
        }
        // Wait for write
        else if(hsResult == SSLReturn::WANT_WRITE) {
            ctx->eventType = EventType::EVENT_HANDSHAKE;
            cev.events = EPOLLOUT | EPOLLET;
        } 
        // Handshake failed or closed
        else {
            Close(ctx);
            return;
        }
    }
    else {
        // Plain HTTP
        ctx->eventType = EventType::EVENT_RECV;
        cev.events = EPOLLIN | EPOLLET;
    }

    if(epoll_ctl(epollFd_, EPOLL_CTL_ADD, clientFd, &cev) < 0)
        Close(ctx);
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