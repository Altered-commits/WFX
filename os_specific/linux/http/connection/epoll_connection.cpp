#include "epoll_connection.hpp"

#include "http/common/http_error_msgs.hpp"
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h>

namespace WFX::OSSpecific {

// vvv Destructor vvv
EpollConnectionHandler::~EpollConnectionHandler() {
    if(listenFd_ > 0) {
        close(listenFd_);
        listenFd_ = -1;
    }
    logger_.Info("[Epoll]: Cleaned up sockets successfully");
}

// vvv Initializing Functions vvv
void EpollConnectionHandler::Initialize(const std::string& host, int port) {
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
        logger_.Fatal("[Epoll]: Failed to create listening socket");

    int opt = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    SetNonBlocking(listenFd_);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if(ResolveHostToIpv4(host.c_str(), &addr.sin_addr) != 0)
        logger_.Fatal("[Epoll]: Failed to resolve host");

    if(bind(listenFd_, (sockaddr*)&addr, sizeof(addr)) < 0)
        logger_.Fatal("[Epoll]: Failed to bind socket");

    if(listen(listenFd_, osConfig.backlog) < 0)
        logger_.Fatal("[Epoll]: Failed to listen");

    epollFd_ = epoll_create1(0);
    if(epollFd_ < 0)
        logger_.Fatal("[Epoll]: Failed to create epoll");

    epoll_event ev{};
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = listenFd_;
    if(epoll_ctl(epollFd_, EPOLL_CTL_ADD, listenFd_, &ev) < 0)
        logger_.Fatal("[Epoll]: Failed to add listenFd to epoll");
}

void EpollConnectionHandler::SetReceiveCallback(ReceiveCallback onData)
{
    onReceive_ = std::move(onData);
}

// vvv I/O Operations vvv
void EpollConnectionHandler::ResumeReceive(ConnectionContext *ctx)
{
    if(!EnsureReadReady(ctx))
        return;

    ctx->eventType = EventType::EVENT_RECV;

    epoll_event ev{};
    ev.events   = EPOLLIN | EPOLLET;
    ev.data.ptr = ctx;

    if(epoll_ctl(epollFd_, EPOLL_CTL_MOD, ctx->socket, &ev) < 0) {
        logger_.Warn("[Epoll]: Failed to arm EPOLLIN for socket");
        Close(ctx);
        return;
    }
}

void EpollConnectionHandler::Write(ConnectionContext * ctx, std::string_view msg)
{
    // We handle both Closing and Writing inline in this function
    // Same for other functions as well
    auto& networkConfig = config_.networkConfig;
    ssize_t n = 0;

    // Case 1: Direct send (used only for static error codes)
    // NOTE: CHANGE OF PLANS, msg is fire and forget, i don't care if they get delivered-
    // -or not, if u want good error messages u will go the hard route anyways (res.Status().SendText()...)
    if(!msg.empty()) {
        (void)::send(ctx->socket, msg.data(), msg.size(), MSG_NOSIGNAL);
        // We ignore result intentionally. If state says close -> close, else -> resume receive
        goto __CleanupOrRearm;
    }
    
    // Case 2: Send from buffer
    else {
        auto* writeMeta = ctx->rwBuffer.GetWriteMeta();
        if(!writeMeta || writeMeta->writtenLength >= writeMeta->dataLength)
            goto __CleanupOrRearm;

        char* buf = ctx->rwBuffer.GetWriteData() + writeMeta->writtenLength;
        std::uint32_t remaining = writeMeta->dataLength - writeMeta->writtenLength;

        // Early bail out
        if(remaining == 0)
            goto __CleanupOrRearm;

        n = ::send(ctx->socket, buf, remaining, MSG_NOSIGNAL);

        if(n > 0) {
            writeMeta->writtenLength += n;

            // If everything sent, disarm EPOLLOUT
            if(writeMeta->writtenLength >= writeMeta->dataLength)
                goto __CleanupOrRearm;

            // Partial write, must wait for EPOLLOUT
            else
                PollAgain(ctx, EventType::EVENT_SEND);
        }
        else if(n < 0) {
            // Socket not ready yet, wait for EPOLLOUT
            if(errno == EAGAIN || errno == EWOULDBLOCK)
                PollAgain(ctx, EventType::EVENT_SEND);
            
            // Fatal error
            else
                goto __CleanupOrRearm;
        }

        return;
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

void EpollConnectionHandler::WriteFile(ConnectionContext *ctx, std::string path)
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

void EpollConnectionHandler::Close(ConnectionContext *ctx)
{
    if(!ctx)
        return;

    epoll_ctl(epollFd_, EPOLL_CTL_DEL, ctx->socket, nullptr);
    ReleaseConnection(ctx);
}

// vvv Main Functions vvv
void EpollConnectionHandler::Run()
{
    // Just a simple sanity check before we do anything
    if(!onReceive_)
        logger_.Fatal("[Epoll]: Member 'onReceive_' was not initialized");

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

            // Accept new connections
            if(fd == listenFd_) {
                while(true) {
                    sockaddr_in addr{};
                    socklen_t len = sizeof(addr);
                    
                    int clientFd = accept4(listenFd_, (sockaddr*)&addr, &len, SOCK_NONBLOCK);
                    if(clientFd < 0)
                        break;

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
                        break;
                    }

                    // Grab a connection slot
                    ConnectionContext* ctx = GetConnection();
                    if(!ctx) {
                        close(clientFd);
                        break;
                    }

                    // Set connection info
                    ctx->socket    = clientFd;
                    ctx->eventType = EventType::EVENT_RECV;
                    ctx->connInfo  = tmpIp;

                    epoll_event cev{};
                    cev.events   = EPOLLIN | EPOLLET; // Allow both
                    cev.data.ptr = ctx;
                    epoll_ctl(epollFd_, EPOLL_CTL_ADD, clientFd, &cev);
                }
            }
            // Handle existing connections
            else {
                auto* ctx = reinterpret_cast<ConnectionContext*>(events_[i].data.ptr);

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
}

HttpTickType EpollConnectionHandler::GetCurrentTick()
{
    return 0;
}

void EpollConnectionHandler::Stop()
{
    running_ = false;
}

// vvv Helper Functions vvv
//  --- Connection Handlers ---
int EpollConnectionHandler::AllocSlot(std::uint64_t* bitmap, int numWords, int maxSlots)
{
    for(int w = 0; w < numWords; w++) {
        std::uint64_t bits = bitmap[w];

        // If even a single '0' exists in the bitmap, we will take it
        // '0' means free slot
        if(~bits) {
            int bit = __builtin_ctzll(~bits);
            int idx = (w << 6) + bit;
            if(idx < maxSlots) {
                bitmap[w] |= (1ULL << bit);
                return idx;
            }
        }
    }
    return -1;
}

void EpollConnectionHandler::FreeSlot(std::uint64_t* bitmap, int idx)
{
    int w   = idx >> 6;
    int bit = idx & 63;
    bitmap[w] &= ~(1ULL << bit);
}

ConnectionContext* EpollConnectionHandler::GetConnection()
{
    int idx = AllocSlot(connBitmap_.get(), connWords_, config_.networkConfig.maxConnections);
    if(idx < 0)
        return nullptr;

    return &connections_[idx];
}

void EpollConnectionHandler::ReleaseConnection(ConnectionContext* ctx)
{
    // Sanity checks
    if(!ctx)
        return;

    if(ctx->socket > 0)
        close(ctx->socket);

    ipLimiter_.ReleaseConnection(ctx->connInfo);

    ctx->ResetContext();

    // Slot index is [current pointer] - [base pointer]
    FreeSlot(connBitmap_.get(), ctx - (&connections_[0]));
}

//  --- MISC Handlers ---
void EpollConnectionHandler::SetNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags != -1)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
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

bool EpollConnectionHandler::EnsureReadReady(ConnectionContext *ctx)
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

int EpollConnectionHandler::ResolveHostToIpv4(const char *host, in_addr *outAddr)
{
    addrinfo hints = { 0 };
    addrinfo *res = nullptr, *rp = nullptr;
    int ret = -1;

    hints.ai_family   = AF_INET;       // Force IPv4
    hints.ai_socktype = SOCK_STREAM;   // TCP style (doesn't really matter here)
    hints.ai_flags    = AI_ADDRCONFIG; // Use only configured addr families

    ret = getaddrinfo(host, NULL, &hints, &res);
    if(ret != 0)
        return -1;

    // Pick the first IPv4 result
    for(rp = res; rp != NULL; rp = rp->ai_next) {
        if(rp->ai_family == AF_INET) {
            sockaddr_in* addr = (sockaddr_in*)rp->ai_addr;
            *outAddr = addr->sin_addr; // Copy the IPv4 address
            freeaddrinfo(res);
            return 0;
        }
    }

    freeaddrinfo(res);
    return -1;
}

void EpollConnectionHandler::Receive(ConnectionContext *ctx)
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

        ssize_t res = ::recv(ctx->socket, region.ptr, region.len - 1, 0);
        if(res <= 0) {
            if(res == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                Close(ctx);

            break; // Stop on EAGAIN / closed
        }

        // Advance buffer + null-terminate
        rwBuffer.AdvanceReadLength(res);
        // rwBuffer.GetReadData()[rwBuffer.GetReadMeta()->dataLength - 1] = '\0';
        gotData = true;
    }

    // Notify app
    if(gotData)
        onReceive_(ctx);
}

void EpollConnectionHandler::SendFile(ConnectionContext *ctx)
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
    int fd       = fileInfo->fd;
    off_t size   = fileInfo->fileSize;
    off_t offset = fileInfo->offset;

    // Early exit
    if(offset >= size)
        goto __CleanupOrRearm;

    else {
        while(offset < size) {
            ssize_t n = ::sendfile(ctx->socket, fd, &offset, size - offset);

            if(n > 0) {
                fileInfo->offset = offset; // Update after successful send
                continue;                  // Try to push more in same event
            }

            if(n < 0) {
                // Partial progress, wait for EPOLLOUT again
                if(errno == EAGAIN || errno == EWOULDBLOCK) {
                    fileInfo->offset = offset;
                    PollAgain(ctx, EventType::EVENT_SEND_FILE);
                    return;
                }
                // Fatal error
                else {
                    Close(ctx);
                    return;
                }
            }

            // n == 0 means nothing was written, break to cleanup
            goto __CleanupOrRearm;
        }
    }

__CleanupOrRearm:
    if(ctx->GetConnectionState() == ConnectionState::CONNECTION_CLOSE)
        Close(ctx);
    else {
        ctx->ClearContext();
        ResumeReceive(ctx);
    }
}

void EpollConnectionHandler::PollAgain(ConnectionContext *ctx, EventType eventType)
{
    ctx->eventType = eventType;
                
    epoll_event ev{};
    ev.events   = EPOLLOUT | EPOLLET;
    ev.data.ptr = ctx;
    epoll_ctl(epollFd_, EPOLL_CTL_MOD, ctx->socket, &ev);
}

} // namespace WFX::OSSpecific