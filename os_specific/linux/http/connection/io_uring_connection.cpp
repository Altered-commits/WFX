#include "io_uring_connection.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
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

    if(listen(listenFd_, BACKLOG) < 0)
        logger_.Fatal("[IoUring]: Failed to listen on socket");

    SetNonBlocking(listenFd_);

    if(io_uring_queue_init(QUEUE_DEPTH, &ring_, 0) < 0)
        logger_.Fatal("[IoUring]: Failed to initialize io_uring");
}

void IoUringConnectionHandler::SetReceiveCallback(ReceiveCallback onData)
{
    onReceive_ = std::move(onData);
}

// vvv I/O Operations vvv
void IoUringConnectionHandler::ResumeReceive(ConnectionContext* ctx)                    { AddRecv(ctx); }
void IoUringConnectionHandler::Write(ConnectionContext* ctx, std::string_view buffer)   { AddSend(ctx, buffer); }
void IoUringConnectionHandler::WriteFile(ConnectionContext *ctx, std::string_view path) { AddFile(ctx, path); }
void IoUringConnectionHandler::Close(ConnectionContext* ctx)                            { ReleaseConnection(ctx); }

// vvv Main Functions vvv
void IoUringConnectionHandler::Run()
{
    // Sanity checks
    if(!onReceive_)
        logger_.Fatal("[IoUring]: 'onReceive_' function is not initialized");

    running_ = true;

    // Initial accept SQEs
    for(int i = 0; i < MAX_CONN_ACCEPT; ++i)
        AddAccept();
    SubmitBatch();

    while(running_) {
        io_uring_cqe* cqe = nullptr;
        int ret = io_uring_wait_cqe(&ring_, &cqe);
        if(ret < 0) {
            if(errno == EINTR)
                continue;

            logger_.Error("[IoUring]: io_uring_wait_cqe error: ", strerror(errno));
            break;
        }

        auto* ptr = static_cast<void*>(io_uring_cqe_get_data(cqe));
        if(!ptr) { 
            io_uring_cqe_seen(&ring_, cqe); 
            continue; 
        }
        
        int res = cqe->res;
        // Determine type fast via first byte
        EventType type = *reinterpret_cast<EventType*>(ptr);

        switch(type) {
            case EventType::EVENT_ACCEPT:
            {
                AcceptSlot* acceptSlot = reinterpret_cast<AcceptSlot*>(ptr);

                if(res >= 0) {
                    int clientFd = res;
                    SetNonBlocking(clientFd);

                    int flag = 1;
                    setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

                    // Grab a connection slot
                    ConnectionContext* ctx = GetConnection();
                    if(!ctx) {
                        close(clientFd);
                        ReleaseAccept(acceptSlot);
                        break;
                    }

                    ctx->socket    = clientFd;
                    ctx->eventType = EventType::EVENT_RECV;

                    // Extract IP address from sockaddr_storage
                    sockaddr* sa = reinterpret_cast<sockaddr*>(&acceptSlot->addr);
                    if(sa->sa_family == AF_INET) {
                        sockaddr_in* s4 = reinterpret_cast<sockaddr_in*>(sa);
                        ctx->connInfo.ip.v4 = s4->sin_addr;
                        ctx->connInfo.ipType = AF_INET;
                    }
                    else if(sa->sa_family == AF_INET6) {
                        sockaddr_in6* s6 = reinterpret_cast<sockaddr_in6*>(sa);
                        ctx->connInfo.ip.v6 = s6->sin6_addr;
                        ctx->connInfo.ipType = AF_INET6;
                    }
                    else
                        ctx->connInfo.ipType = 255; // Unknown

                    // Start receiving immediately
                    AddRecv(ctx);
                }

                // Re-arm accept for next incoming connection
                ReleaseAccept(acceptSlot);
                AddAccept();
                break;
            }

            case EventType::EVENT_RECV: {
                ConnectionContext* ctx = reinterpret_cast<ConnectionContext*>(ptr);

                if(res <= 0) {
                    // Client closed connection / Error
                    ReleaseConnection(ctx);
                    break;
                }

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
                ConnectionContext* ctx = reinterpret_cast<ConnectionContext*>(ptr);

                if(res < 0) {
                    // Just retry with whatever was pending
                    if(res == -EAGAIN || res == -EWOULDBLOCK) {
                        AddSend(ctx, {});
                        break;
                    }
                    // Fatal send error
                    else {
                        ReleaseConnection(ctx);
                        break;
                    }
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

                    // Finished buffered write
                    writeMeta->dataLength    = 0;
                    writeMeta->writtenLength = 0;
                }

                // If we get here, send is fully done
                if(ctx->GetConnectionState() == ConnectionState::CONNECTION_CLOSE)
                    ReleaseConnection(ctx);
                else
                    AddRecv(ctx);

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
    int idx = AllocSlot(connBitmap_, CONNECTION_WORDS, MAX_CONNECTIONS);
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

    ctx->ResetContext();

    // Slot index is [current pointer] - [base pointer]
    FreeSlot(connBitmap_, ctx - (&connections_[0]));
}

AcceptSlot* IoUringConnectionHandler::GetAccept()
{
    int idx = AllocSlot(acceptBitmap_, ACCEPT_WORDS, MAX_CONN_ACCEPT);
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
    FreeSlot(acceptBitmap_, slot - (&acceptSlots_[0]));
}

//  --- MISC Handlers ---
void IoUringConnectionHandler::SetNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags != -1)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
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

    if(++sqeBatch_ >= BATCH_SIZE)
        SubmitBatch();
}

void IoUringConnectionHandler::AddRecv(ConnectionContext* ctx)
{
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

    io_uring_prep_recv(sqe, ctx->socket, region.ptr, region.len, 0);
    io_uring_sqe_set_data(sqe, ctx);

    ctx->eventType = EventType::EVENT_RECV;

    if(++sqeBatch_ >= BATCH_SIZE)
        SubmitBatch();
}

void IoUringConnectionHandler::AddSend(ConnectionContext* ctx, std::string_view msg)
{
    // 1) Fail fast: check SQE first
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if(!sqe)
        return;

    auto& networkConfig = config_.networkConfig;

    // Direct send (static string, no buffering involved)
    if(!msg.empty())
        io_uring_prep_send(sqe, ctx->socket, msg.data(),
                           static_cast<unsigned int>(msg.size()), MSG_NOSIGNAL);
    // Send from buffer
    else {
        auto* writeMeta = ctx->rwBuffer.GetWriteMeta();
        if(!writeMeta || writeMeta->writtenLength >= writeMeta->dataLength)
            return; // Nothing to send

        char* buf = ctx->rwBuffer.GetWriteData() + writeMeta->writtenLength;
        std::uint32_t remaining = writeMeta->dataLength - writeMeta->writtenLength;

        io_uring_prep_send(sqe, ctx->socket, buf, remaining, MSG_NOSIGNAL);
    }

    io_uring_sqe_set_data(sqe, ctx);
    
    ctx->eventType = EventType::EVENT_SEND;
    if(++sqeBatch_ >= BATCH_SIZE)
        SubmitBatch();
}

void IoUringConnectionHandler::AddFile(ConnectionContext *ctx, std::string_view path)
{
    return;
}

void IoUringConnectionHandler::SubmitBatch()
{
    if(sqeBatch_ > 0) {
        io_uring_submit(&ring_);
        sqeBatch_ = 0;
    }
}

} // namespace WFX::OSSpecific