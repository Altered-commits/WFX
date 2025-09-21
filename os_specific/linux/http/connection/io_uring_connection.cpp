#ifdef WFX_LINUX_USE_IO_URING

#include "io_uring_connection.hpp"

#include "http/common/http_error_msgs.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <sys/poll.h>
#include <unistd.h>

namespace WFX::OSSpecific {

// vvv Destructor vvv
IoUringConnectionHandler::~IoUringConnectionHandler()
{
    if(listenFd_ > 0) {
        close(listenFd_);
        listenFd_ = -1;
    }
    io_uring_queue_exit(&ring_);

    logger_.Info("[IoUring]: Cleaned up sockets successfully");
}

// vvv Initializing Functions vvv
void IoUringConnectionHandler::Initialize(const std::string &host, int port)
{
    // vvv Initialize our pre-fixed array of connections and accept handlers vvv
    auto& osConfig      = config_.osSpecificConfig;
    auto& networkConfig = config_.networkConfig;

    connWords_   = (networkConfig.maxConnections + 63) / 64;
    acceptWords_ = (osConfig.acceptSlots + 63) / 64;

    connections_  = std::make_unique<ConnectionContext[]>(networkConfig.maxConnections);
    connBitmap_   = std::make_unique<std::uint64_t[]>(connWords_);

    acceptSlots_  = std::make_unique<AcceptSlot[]>(osConfig.acceptSlots);
    acceptBitmap_ = std::make_unique<std::uint64_t[]>(acceptWords_);

    std::fill_n(connBitmap_.get(), (networkConfig.maxConnections + 63)/64, 0);
    std::fill_n(acceptBitmap_.get(), (osConfig.acceptSlots + 63)/64, 0);

    // vvv Initialize our sockets vvv
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if(listenFd_ < 0)
        logger_.Fatal("[IoUring]: Failed to create listening socket for host: ", host);

    int opt = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    if(ResolveHostToIpv4(host.c_str(), &addr.sin_addr) != 0)
        logger_.Fatal("[IoUring]: Failed to resolve host address");

    if(bind(listenFd_, (struct sockaddr*)&addr, sizeof(addr)) < 0)
        logger_.Fatal("[IoUring]: Failed to bind to socket");

    if(listen(listenFd_, osConfig.backlog) < 0)
        logger_.Fatal("[IoUring]: Failed to listen on socket");

    SetNonBlocking(listenFd_);

    if(io_uring_queue_init(osConfig.queueDepth, &ring_, 0) < 0)
        logger_.Fatal("[IoUring]: Failed to initialize io_uring");
}

void IoUringConnectionHandler::SetReceiveCallback(ReceiveCallback onData)
{
    onReceive_ = std::move(onData);
}

// vvv I/O Operations vvv
void IoUringConnectionHandler::ResumeReceive(ConnectionContext* ctx)                    { AddRecv(ctx); }
void IoUringConnectionHandler::Write(ConnectionContext* ctx, std::string_view buffer)   { AddSend(ctx, buffer); }
void IoUringConnectionHandler::Close(ConnectionContext* ctx)                            { ReleaseConnection(ctx); }
void IoUringConnectionHandler::WriteFile(ConnectionContext *ctx, std::string path)
{
    // Ensure that the file exists and our context is ready for sending file
    // If not we 404 error
    if(!EnsureFileReady(ctx, std::move(path))) {
        AddSend(ctx, notFound);
        return;
    }

    // Cool we can send file, but before that we need to send header
    // AddSend will handle the case where isFileOperation is 1, meaning it will invoke-
    // -AddFile for us after header is sent successfully
    ctx->isFileOperation = 1;
    AddSend(ctx, {});
}

// vvv Main Functions vvv
void IoUringConnectionHandler::Run()
{
    // Sanity checks
    if(!onReceive_)
        logger_.Fatal("[IoUring]: 'onReceive_' function is not initialized");

    running_ = true;

    // Initial accept SQEs
    for(int i = 0; i < config_.osSpecificConfig.acceptSlots; ++i)
        AddAccept();
    SubmitBatch();

    // Simple lambda for re-arming accept slots
    auto RearmAcceptSlot = [this](AcceptSlot* slot) {
        ReleaseAccept(slot);
        AddAccept();
    };

    while(running_) {
        io_uring_cqe* cqe = nullptr;
        int ret = io_uring_wait_cqe(&ring_, &cqe);
        if(ret < 0) {
            if(errno == EINTR)
                continue;

            break;
        }

        auto* ptr = static_cast<void*>(io_uring_cqe_get_data(cqe));
        if(!ptr) { 
            io_uring_cqe_seen(&ring_, cqe); 
            continue; 
        }
        
        int res = cqe->res;
        // Safely cast to the base Tag to read the type
        ConnectionTag* baseTag = static_cast<ConnectionTag*>(ptr);

        switch(baseTag->eventType) {
            case EventType::EVENT_ACCEPT:
            {
                AcceptSlot* acceptSlot = static_cast<AcceptSlot*>(baseTag);

                if(res >= 0) {
                    int clientFd = res;
                    SetNonBlocking(clientFd);

                    int flag = 1;
                    setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

                    // Extract IP info first
                    WFXIpAddress tmpIp;
                    sockaddr* sa = reinterpret_cast<sockaddr*>(&acceptSlot->addr);
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
                        RearmAcceptSlot(acceptSlot);
                        break;
                    }

                    // Grab a connection slot
                    ConnectionContext* ctx = GetConnection();
                    if(!ctx) {
                        close(clientFd);
                        RearmAcceptSlot(acceptSlot);
                        break;
                    }

                    ctx->socket    = clientFd;
                    ctx->eventType = EventType::EVENT_RECV;
                    ctx->connInfo  = tmpIp;

                    // Start receiving immediately
                    AddRecv(ctx);
                }

                // Re-arm accept for next incoming connection
                RearmAcceptSlot(acceptSlot);
                break;
            }

            case EventType::EVENT_RECV: {
                ConnectionContext* ctx = static_cast<ConnectionContext*>(baseTag);

                if(res <= 0) {
                    // Client closed connection / Error
                    ReleaseConnection(ctx);
                    break;
                }

                // TODO: FIX RandomPool which fixes SipHash24 which fixes ConcurrentHashMap which fixes the below
                // if(!ipLimiter_.AllowRequest(ctx->connInfo)) {
                //     SendHttp429(ctx);
                //     break;
                // }

                auto& rwBuffer = ctx->rwBuffer;
                
                // Update buffer state
                rwBuffer.AdvanceReadLength(res);

                // Add null terminator safely
                ReadMetadata* readMeta = rwBuffer.GetReadMeta();
                char*         dataPtr  = rwBuffer.GetReadData();
                dataPtr[readMeta->dataLength] = '\0';

                onReceive_(ctx);
                break;
            }

            case EventType::EVENT_SEND: {
                ConnectionContext* ctx = static_cast<ConnectionContext*>(baseTag);

                if(res < 0) {
                    // Just retry with whatever was pending
                    if(res == -EAGAIN || res == -EWOULDBLOCK) {
                        AddSend(ctx, {});
                        break;
                    }
                    // Fatal send error
                    ReleaseConnection(ctx);
                    break;
                }
                
                auto& rwBuffer  = ctx->rwBuffer;
                auto* writeMeta = rwBuffer.GetWriteMeta();

                if(writeMeta && writeMeta->dataLength > 0) {
                    // This was a buffered send -> advance writtenLength
                    rwBuffer.AdvanceWriteLength(res);

                    if(writeMeta->writtenLength < writeMeta->dataLength) {
                        // Still data left -> re-arm send from buffer
                        AddSend(ctx, {});
                        break;
                    }
                }

                // Before we do anything else, check if its a file operation, if it-
                // -is, then send file before doing anything else
                if(ctx->isFileOperation) {
                    AddFile(ctx);
                    break;
                }

                // If we get here, send is fully done
                if(ctx->GetConnectionState() == ConnectionState::CONNECTION_CLOSE)
                    ReleaseConnection(ctx);
                else {
                    ctx->ClearContext();
                    AddRecv(ctx);
                }
                break;
            }

            case EventType::EVENT_SEND_FILE: {
                ConnectionContext* ctx = static_cast<ConnectionContext*>(baseTag);

                // For some stupid reason, if it were to be nullptr, even tho it shouldnt be
                auto* fileInfo = ctx->fileInfo;
                if(!fileInfo) {
                    ReleaseConnection(ctx);
                    break;
                }

                // Check for <= 0 to handle both errors and client disconnects
                if(res <= 0) {
                    // Re-arm the same sendfile SQE later
                    if(res < 0 && (res == -EAGAIN || res == -EWOULDBLOCK))
                        AddFile(ctx);
                    // Fatal error
                    else
                        ReleaseConnection(ctx);
                    break;
                }

                // res bytes were successfully sent
                fileInfo->offset += res;

                off_t remaining = fileInfo->fileSize - fileInfo->offset;
                
                // More to send -> re-arm another sendfile SQE
                if(remaining > 0) {
                    AddFile(ctx);
                    break;
                }

                if(ctx->GetConnectionState() == ConnectionState::CONNECTION_CLOSE)
                    ReleaseConnection(ctx);
                else {
                    ctx->ClearContext();
                    AddRecv(ctx);
                }
                break;
            }
        }

        io_uring_cqe_seen(&ring_, cqe);

        // Submit any batched SQEs
        SubmitBatch();
    }
}

HttpTickType IoUringConnectionHandler::GetCurrentTick()
{
    return 0;
}

void IoUringConnectionHandler::Stop()
{
    running_ = false;
}

// vvv Helper Functions vvv
//  --- Connection Handlers ---
int IoUringConnectionHandler::AllocSlot(std::uint64_t* bitmap, int numWords, int maxSlots)
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

void IoUringConnectionHandler::FreeSlot(std::uint64_t* bitmap, int idx)
{
    int w   = idx >> 6;
    int bit = idx & 63;
    bitmap[w] &= ~(1ULL << bit);
}

ConnectionContext* IoUringConnectionHandler::GetConnection()
{
    int idx = AllocSlot(connBitmap_.get(), connWords_, config_.networkConfig.maxConnections);
    if(idx < 0)
        return nullptr;

    return &connections_[idx];
}

void IoUringConnectionHandler::ReleaseConnection(ConnectionContext* ctx)
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

AcceptSlot* IoUringConnectionHandler::GetAccept()
{
    int idx = AllocSlot(acceptBitmap_.get(), acceptWords_, config_.osSpecificConfig.acceptSlots);
    if(idx < 0)
        return nullptr;

    return &acceptSlots_[idx];
}

void IoUringConnectionHandler::ReleaseAccept(AcceptSlot *slot)
{
    // Sanity checks
    if(!slot)
        return;
    
    // Slot index is [current pointer] - [base pointer]
    FreeSlot(acceptBitmap_.get(), slot - (&acceptSlots_[0]));
}

//  --- MISC Handlers ---
void IoUringConnectionHandler::SetNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags != -1)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

bool IoUringConnectionHandler::EnsureFileReady(ConnectionContext* ctx, std::string path)
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

int IoUringConnectionHandler::ResolveHostToIpv4(const char *host, in_addr *outAddr)
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

//  --- Socket Handlers ---
void IoUringConnectionHandler::AddAccept()
{
    // Base checks
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if(!sqe)
        return;
    
    AcceptSlot* slot = GetAccept();
    if(!slot)
        return;

    slot->addrLen = sizeof(slot->addr);

    io_uring_prep_accept(sqe, listenFd_, reinterpret_cast<sockaddr*>(&slot->addr),
                            &slot->addrLen, 0);
    io_uring_sqe_set_data(sqe, slot);

    if(++sqeBatch_ >= config_.osSpecificConfig.batchSize)
        SubmitBatch();
}

void IoUringConnectionHandler::AddRecv(ConnectionContext* ctx)
{
    // Base checks
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if(!sqe)
        return;

    auto& networkConfig = config_.networkConfig;

    // Ensure read buffer exists
    if(!ctx->rwBuffer.IsReadInitialized() && 
        !ctx->rwBuffer.InitReadBuffer(pool_, networkConfig.bufferIncrSize))
    {
        logger_.Error("[IoUring]: Failed to init read buffer");
        return;
    }

    // Get writable region of read buffer, try to grow if necessary
    ValidRegion region = ctx->rwBuffer.GetWritableReadRegion();
    if(!region.ptr || region.len == 0) {
        if(!ctx->rwBuffer.GrowReadBuffer(networkConfig.bufferIncrSize, networkConfig.maxRecvBufferSize)) {
            logger_.Warn("[IoUring]: Read buffer full, closing connection");
            ReleaseConnection(ctx);
            return;
        }
        region = ctx->rwBuffer.GetWritableReadRegion();
    }

    // Leave -1 space for null terminator
    io_uring_prep_recv(sqe, ctx->socket, region.ptr, region.len - 1, 0);
    io_uring_sqe_set_data(sqe, ctx);

    ctx->eventType = EventType::EVENT_RECV;

    if(++sqeBatch_ >= config_.osSpecificConfig.batchSize)
        SubmitBatch();
}

void IoUringConnectionHandler::AddSend(ConnectionContext* ctx, std::string_view msg)
{
    // Base checks
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if(!sqe)
        return;

    auto& networkConfig = config_.networkConfig;

    // Direct send (static string, no buffering involved)
    // NOTE: MSG IS PURELY USED FOR STATIC ERROR CODES, IT SHOULD NOT BE USED AS NORMAL PATH
    if(!msg.empty())
        io_uring_prep_send(sqe, ctx->socket, msg.data(),
                           static_cast<unsigned int>(msg.size()), MSG_NOSIGNAL);
    // Send from buffer
    else {
        auto* writeMeta = ctx->rwBuffer.GetWriteMeta();
        if(!writeMeta || writeMeta->writtenLength >= writeMeta->dataLength)
            return;

        char* buf = ctx->rwBuffer.GetWriteData() + writeMeta->writtenLength;
        std::uint32_t remaining = writeMeta->dataLength - writeMeta->writtenLength;

        io_uring_prep_send(sqe, ctx->socket, buf, remaining, MSG_NOSIGNAL);
    }

    io_uring_sqe_set_data(sqe, ctx);
    
    ctx->eventType = EventType::EVENT_SEND;

    if(++sqeBatch_ >= config_.osSpecificConfig.batchSize)
        SubmitBatch();
}

void IoUringConnectionHandler::AddFile(ConnectionContext *ctx)
{
    // AddSend after sending header, people expecting file im sending text
    // Dawg im cooked
    if(!ctx->fileInfo) {
        ctx->isFileOperation = 0;
        AddSend(ctx, internalError);
        return;
    }

    auto* fileInfo = ctx->fileInfo;
    off_t offset   = fileInfo->offset;
    off_t size     = fileInfo->fileSize;
    off_t remain   = size - offset;

    if(remain <= 0)
        return;

    // Get submission queue entry
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if(!sqe)
        return;

    // Use a chunk size for large files
    off_t chunk = std::min<off_t>(remain, config_.osSpecificConfig.fileChunkSize);

    // Fully async file sending
    io_uring_prep_splice(
        sqe,
        fileInfo->fd,       // input fd
        fileInfo->offset,   // input offset
        ctx->socket,        // output fd (socket)
        -1,                 // offset ignored for sockets
        chunk,
        0                   // flags (0 for default)
    );

    io_uring_sqe_set_data(sqe, ctx);
    ctx->eventType = EventType::EVENT_SEND_FILE;

    if(++sqeBatch_ >= config_.osSpecificConfig.batchSize)
        SubmitBatch();
}

void IoUringConnectionHandler::SubmitBatch()
{
    if(sqeBatch_ > 0) {
        io_uring_submit(&ring_);
        sqeBatch_ = 0;
    }
}

} // namespace WFX::OSSpecific

#endif // WFX_LINUX_USE_IO_URING