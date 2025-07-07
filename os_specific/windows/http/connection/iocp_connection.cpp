#include "iocp_connection.hpp"

#undef max
#undef min

namespace WFX::OSSpecific {

IocpConnectionHandler::IocpConnectionHandler()
{
    WSADATA wsaData;
    if(WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        logger_.Fatal("[IOCP]: WSAStartup failed");

    if(LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2)
        logger_.Fatal("[IOCP]: Incorrect Winsock version");

    allocPool_.PreWarmAll(8);
}

IocpConnectionHandler::~IocpConnectionHandler()
{
    // Final cleanup on destruction
    InternalCleanup();
}

bool IocpConnectionHandler::Initialize(const std::string& host, int port)
{
    listenSocket_ = WSASocket(AF_INET, SOCK_STREAM, 0, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if(listenSocket_ == INVALID_SOCKET)
        return logger_.Error("[IOCP]: WSASocket failed"), false;

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    // Handle special cases: "localhost" and "0.0.0.0"
    if(host == "localhost")
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1

    else if(host == "0.0.0.0")
        addr.sin_addr.s_addr = htonl(INADDR_ANY);       // Bind all interfaces

    else if(inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
        return logger_.Error("[IOCP]: Failed to parse host IP: ", host), false;

    // Bind and Listen on the host:port combo provided
    if(bind(listenSocket_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
        return logger_.Error("[IOCP]: Bind failed"), false;

    if(listen(listenSocket_, SOMAXCONN) == SOCKET_ERROR)
        return logger_.Error("[IOCP]: Listen failed"), false;

    // We using IOCP for async connection handling
    iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if(!iocp_)
        return logger_.Error("[IOCP]: CreateIoCompletionPort failed"), false;

    if(!CreateIoCompletionPort((HANDLE)listenSocket_, iocp_, (ULONG_PTR)listenSocket_, 0))
        return logger_.Error("[IOCP]: Association failed"), false;

    // All the networking stuff has been initialized, time to initialize the timeout handler now

    return true;
}

void IocpConnectionHandler::SetReceiveCallback(WFXSocket socket, ReceiveCallback callback)
{
    if(!callback) {
        logger_.Error("[IOCP]: Invalid callback in 'SetReceiveCallback'");
        return;
    }

    auto* ctx = connections_.Get(socket);
    if(!ctx || !(*ctx)) {
        logger_.Error("[IOCP]: No Connection Context found for socket: ", socket);
        Close(socket);
        return;
    }

    // Store the callback for this connection
    (*ctx)->onReceive = std::move(callback);
    
    // Hand off PostReceive to IOCP thread. sizeof(PostRecvOp) rounded to 64 bytes
    if(!PostQueuedCompletionStatus(iocp_, 0, static_cast<ULONG_PTR>(socket), &(ARM_RECV_OP.overlapped))) {
        logger_.Error("[IOCP]: Failed to queue PostReceive for socket: ", socket);
        Close(socket);
    }
}

void IocpConnectionHandler::ResumeReceive(WFXSocket socket)
{
    // Simply 're-arm' WSARecv
    PostReceive(socket);
}

int IocpConnectionHandler::Write(WFXSocket socket, std::string_view buffer_)
{
    std::size_t length = buffer_.length();
    const char* buffer = buffer_.data();

    void* outBuffer = bufferPool_.Lease(length);
    if(!outBuffer)
        return -1;

    std::memcpy(outBuffer, buffer, length);

    // Because its fixed size alloc, we use alloc pool for efficiency
    PerIoData* ioData = static_cast<PerIoData*>(allocPool_.Allocate(sizeof(PerIoData)));
    if(!ioData) {
        bufferPool_.Release(outBuffer);
        return -1;
    }

    ioData->overlapped    = { 0 };
    ioData->operationType = PerIoOperationType::SEND;
    ioData->socket        = socket;
    ioData->wsaBuf.buf    = static_cast<char*>(outBuffer);
    ioData->wsaBuf.len    = static_cast<ULONG>(length);

    DWORD bytesSent;
    int ret = WSASend(socket, &ioData->wsaBuf, 1, &bytesSent, 0, &ioData->overlapped, nullptr);
    if(ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        SafeDeleteIoData(ioData);
        return -1;
    }

    return static_cast<int>(length);
}

int IocpConnectionHandler::WriteFile(WFXSocket socket, std::string&& header, std::string_view path)
{
    void* rawMem = allocPool_.Allocate(sizeof(PerTransmitFileContext));
    if(!rawMem) return -1;

    // Because its fixed size alloc, we use alloc pool for efficiency
    PerTransmitFileContext* fileData = new (rawMem) PerTransmitFileContext(); // Placement new for construction

    // Open the file for sending
    HANDLE file = CreateFileA(
        path.data(), GENERIC_READ,
        FILE_SHARE_READ, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if(file == INVALID_HANDLE_VALUE) {
        SafeDeleteTransmitFileCtx(fileData);
        return -1;
    }

    // Set all the necessary stuff so we can clean them up later in WorkerThreads
    fileData->overlapped     = { 0 };
    fileData->operationType  = PerIoOperationType::SEND_FILE;
    fileData->socket         = socket;
    fileData->header         = std::move(header);
    fileData->fileHandle     = file;
    fileData->tfb.Head       = fileData->header.data();
    fileData->tfb.HeadLength = fileData->header.length();
    fileData->tfb.Tail       = nullptr;
    fileData->tfb.TailLength = 0;

    BOOL ok = TransmitFile(
        socket,
        file,
        0, 0,
        &fileData->overlapped,
        &fileData->tfb,
        TF_USE_KERNEL_APC
    );

    if(!ok && WSAGetLastError() != ERROR_IO_PENDING) {
        SafeDeleteTransmitFileCtx(fileData);
        return -1;
    }

    return static_cast<int>(fileData->header.length());
}

void IocpConnectionHandler::Close(WFXSocket socket)
{
    CancelIoEx((HANDLE)socket, NULL);
    shutdown(socket, SD_BOTH);
    closesocket(socket);

    if(!connections_.Erase(socket))
        logger_.Warn("[IOCP]: Failed to erase Connection Context for socket: ", socket);
}

void IocpConnectionHandler::Run(AcceptedConnectionCallback onAccepted)
{
    logger_.Info("[IOCP]: Starting connection handler...");
    running_ = true;

    if(!onAccepted)
        logger_.Fatal("[IOCP]: Failed to get 'onAccepted' callback");
    acceptCallback_ = std::move(onAccepted);

    if(!acceptManager_.Initialize(listenSocket_, iocp_))
        logger_.Fatal("[IOCP]: Failed to initialize AcceptExManager");

    if(!CreateWorkerThreads(
        config_.osSpecificConfig.workerThreadCount, config_.osSpecificConfig.callbackThreadCount
    ))
        return logger_.Fatal("[IOCP]: Failed to start worker threads.");
}

void IocpConnectionHandler::Stop()
{
    running_ = false;

    // Wake up all our sleepy ass threads and start to prepare for ANHILATION
    for(size_t i = 0; i < workerThreads_.size(); ++i)
        PostQueuedCompletionStatus(iocp_, 0, 0, nullptr);
}

void IocpConnectionHandler::PostReceive(WFXSocket socket)
{
    WFX_PROFILE_BLOCK_START(PostReceive_Lookup);

    auto* ctxPtr = connections_.Get(socket);
    if(!ctxPtr || !(*ctxPtr)) {
        logger_.Error("[IOCP]: PostReceive failed - no Connection Context for socket: ", socket);
        Close(socket);
        return;
    }

    ConnectionContext& ctx = *(*ctxPtr);

    WFX_PROFILE_BLOCK_END(PostReceive_Lookup);
    WFX_PROFILE_BLOCK_START(PostReceive_Lease);

    // Lazy-allocate receive buffer
    std::size_t defaultSize = config_.networkConfig.bufferIncrSize;

    if(!ctx.buffer) {
        ctx.buffer = static_cast<char*>(bufferPool_.Lease(defaultSize));
        if(!ctx.buffer) {
            logger_.Error("[IOCP]: Failed to allocate receive buffer for socket: ", socket);
            Close(socket);
            return;
        }

        ((volatile char*)ctx.buffer)[0] = 0;  // Page fault avoidance
        ctx.bufferSize = defaultSize;
        ctx.dataLength = 0;
    }

    // The way we want it is simple, we have a buffer which can grow to a certain limit 'config_.networkConfig.maxRecvBufferSize'
    // The buffer, when growing, will increment in 'defaultSize' till 'ctx.dataLength' reaches 'ctx.bufferSize - 1'
    // '-1' for the null terminator :)
    if(ctx.dataLength >= ctx.bufferSize - 1) {
        // The buffer can no longer grow. Hmmm, lets kill the connection lmao
        if(ctx.bufferSize >= config_.networkConfig.maxRecvBufferSize) {
            logger_.Error("[IOCP]: Max buffer limit reached for socket: ", socket);
            Close(socket);
            return;
        }

        // The buffer can grow
        std::size_t newSize = ctx.bufferSize + defaultSize;
        void* newBuffer = bufferPool_.Lease(newSize);
        if(!newBuffer) {
            logger_.Error("[IOCP]: Failed to resize connection buffer for socket: ", socket);
            Close(socket);
            return;
        }

        std::memcpy(newBuffer, ctx.buffer, ctx.dataLength);
        bufferPool_.Release(ctx.buffer);
        
        // Congrats, our buffer just grew :)
        ctx.buffer     = static_cast<char*>(newBuffer);
        ctx.bufferSize = newSize;
    }
    
    WFX_PROFILE_BLOCK_END(PostReceive_Lease);
    WFX_PROFILE_BLOCK_START(PostReceive_New);

    // Allocate IO context
    PerIoData* ioData = static_cast<PerIoData*>(allocPool_.Allocate(sizeof(PerIoData)));
    if(!ioData) {
        logger_.Error("[IOCP]: Failed to allocate PerIoData");
        Close(socket);
        return;
    }

    // Compute safe length that won't exceed buffer or policy. -1 for null terminator :)
    const size_t remainingBufferSize = ctx.bufferSize - ctx.dataLength - 1;

    ioData->overlapped    = {};
    ioData->operationType = PerIoOperationType::RECV;
    ioData->socket        = socket;
    ioData->wsaBuf.buf    = ctx.buffer + ctx.dataLength;
    ioData->wsaBuf.len    = static_cast<ULONG>(remainingBufferSize);

    DWORD flags = 0, bytesRecv = 0;
    int ret = WSARecv(socket, &ioData->wsaBuf, 1, &bytesRecv, &flags, &ioData->overlapped, nullptr);
    if(ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        logger_.Error("[IOCP]: WSARecv immediate failure: ", WSAGetLastError());
        SafeDeleteIoData(ioData);
        Close(socket);
        return;
    }

    WFX_PROFILE_BLOCK_END(PostReceive_New);
}

void IocpConnectionHandler::WorkerLoop()
{
    DWORD       bytesTransferred;
    ULONG_PTR   key;
    OVERLAPPED* overlapped;

    // One quick protection, OVERLAPPED must be the first thing in the structs we use
    static_assert(offsetof(PerIoBase, overlapped) == 0, "OVERLAPPED must be first!");

    while(running_) {
        BOOL result = GetQueuedCompletionStatus(iocp_, &bytesTransferred, &key, &overlapped, 1000);
        if(!result || !overlapped)
            continue;

        auto*              base   = reinterpret_cast<PerIoBase*>(overlapped);
        PerIoOperationType opType = base->operationType;

        switch(opType) {
            case PerIoOperationType::ARM_RECV:
            {
                PostReceive(static_cast<SOCKET>(key));
                break;
            }

            case PerIoOperationType::RECV:
            {
                WFX_PROFILE_BLOCK_START(RECV_Handle);

                // We do not need buffer to be released as ioData does not own the buffer, ConnectionContext does
                // Hence the 'false' in the PerIoDataDeleter
                std::unique_ptr<PerIoData, PerIoDataDeleter> ioData(static_cast<PerIoData*>(base), PerIoDataDeleter{this, false});
                SOCKET socket = ioData->socket;

                if(bytesTransferred <= 0) {
                    Close(socket);
                    break;
                }

                auto* ctxPtr = connections_.Get(socket);
                if(!ctxPtr || !(*ctxPtr)) {
                    logger_.Error("[IOCP]: No Connection Context found for RECV socket: ", socket);
                    Close(socket);
                    break;
                }

                ConnectionContext& ctx = *(*ctxPtr);

                // Just in case
                if(!ctx.onReceive) {
                    logger_.Error("[IOCP]: No Receive Callback set for socket: ", socket);
                    Close(socket);
                    break;
                }

                // Too many requests are not allowed
                if(!limiter_.AllowRequest(ctx.connInfo)) {
                    // Temporary solution rn, will change in future
                    static constexpr const char* kRateLimitResponse =
                        "HTTP/1.1 503 Service Unavailable\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n"
                        "\r\n";

                    // Mark this socket for closure on next SEND call. No need to handle further
                    ctx.shouldClose = true;

                    // Not going to bother checking for return value. This is just for response
                    Write(socket, std::string_view(kRateLimitResponse));
                    break;
                }

                // Update buffer state
                ctx.dataLength += bytesTransferred;

                // Add null terminator
                ctx.buffer[ctx.dataLength] = '\0';

                // Just call the callback from within the offload thread without moving it
                offloadCallbacks_.enqueue([&ctx]() mutable {
                    ctx.onReceive(ctx);
                });

                WFX_PROFILE_BLOCK_END(RECV_Handle);
                break;
            }

            case PerIoOperationType::SEND:
            {
                std::unique_ptr<PerIoData, PerIoDataDeleter> ioData(
                    static_cast<PerIoData*>(base), PerIoDataDeleter{this}
                );

                // Close if client wants to, by checking ctx.shouldClose
                // Also Get is quite fast so no issue calling it in hot path
                auto* ctx = connections_.Get(ioData->socket);
                if(!ctx || !(*ctx)) {
                    logger_.Warn("[IOCP]: SEND complete but no context for socket: ", ioData->socket);
                    break;
                }

                if((*ctx)->shouldClose)
                    Close(ioData->socket);
                else
                    ResumeReceive(ioData->socket);

                break;
            }

            case PerIoOperationType::SEND_FILE:
            {
                std::unique_ptr<PerTransmitFileContext, PerTransmitFileCtxDeleter> transmitFileCtx(
                    static_cast<PerTransmitFileContext*>(base), PerTransmitFileCtxDeleter{this}
                );

                // Close if client wants to, by checking ctx.shouldClose
                // Also Get is quite fast so no issue calling it in hot path
                auto* ctx = connections_.Get(transmitFileCtx->socket);
                if(!ctx || !(*ctx)) {
                    logger_.Warn("[IOCP]: SEND complete but no context for socket: ", transmitFileCtx->socket);
                    break;
                }

                if((*ctx)->shouldClose)
                    Close(transmitFileCtx->socket);
                else
                    ResumeReceive(transmitFileCtx->socket);
                
                break;
            }

            case PerIoOperationType::ACCEPT:
            {
                WFX_PROFILE_BLOCK_START(ACCEPT_Handle);
                acceptManager_.HandleAcceptCompletion(static_cast<PerIoContext*>(base));
                WFX_PROFILE_BLOCK_END(ACCEPT_Handle);
                break;
            }

            case PerIoOperationType::ACCEPT_DEFERRED:
            {
                WFX_PROFILE_BLOCK_START(ACCEPT_Deferred);

                SOCKET client = base->socket;
                acceptManager_.HandleSocketOptions(client);

                // Take ownership of PostAcceptOp directly
                std::unique_ptr<PostAcceptOp, std::function<void(PostAcceptOp*)>> acceptOp(
                    static_cast<PostAcceptOp*>(base),
                    [this](PostAcceptOp* ptr) {
                        bufferPool_.Release(ptr);
                    }
                );

                // Create the ConnectionContext needed for this connection to be kept alive
                // We will lazy allocate buffer because we don't know if we need it later or not
                ConnectionContextPtr connectionContext(
                    new ConnectionContext(), ConnectionContextDeleter{this}
                );
                connectionContext->connInfo = acceptOp->ipAddr;

                if(!connections_.Emplace(client, std::move(connectionContext)))
                    logger_.Fatal("[IOCP]: Failed to create ConnectionContext for socket: ", client);

                // Offload callbacks, save time :)
                offloadCallbacks_.enqueue([this, client]() mutable {
                    acceptCallback_(client);
                });

                WFX_PROFILE_BLOCK_END(ACCEPT_Deferred);
                break;
            }

            default:
                logger_.Warn("Unknown IOCP operation type: ", (int)opType);
                break;
        }
    }
}

bool IocpConnectionHandler::CreateWorkerThreads(unsigned int iocpThreads, unsigned int offloadThreads)
{
    // Launch IOCP worker threads
    for(unsigned int i = 0; i < iocpThreads; ++i)
        workerThreads_.emplace_back(&IocpConnectionHandler::WorkerLoop, this);

    // Launch offload callback threads
    for(unsigned int i = 0; i < offloadThreads; ++i)
        offloadThreads_.emplace_back([this]() {
            while(running_) {
                std::function<void(void)> cb;
                if(offloadCallbacks_.wait_dequeue_timed(cb, std::chrono::milliseconds(10)))
                    cb();
            }
        });

    return true;
}

void IocpConnectionHandler::SafeDeleteIoData(PerIoData* data, bool shouldCleanBuffer)
{
    if(!data) return;
    
    // Buffer is variable, so we used buffer pool.
    if(data->wsaBuf.buf && shouldCleanBuffer) {
        bufferPool_.Release(data->wsaBuf.buf);
        data->wsaBuf.buf = nullptr;
    }
    
    // PerIoData is fixed, so we used alloc pool
    allocPool_.Free(data, sizeof(PerIoData));
}

void IocpConnectionHandler::SafeDeleteTransmitFileCtx(PerTransmitFileContext* transmitFileCtx)
{
    if(!transmitFileCtx) return;
    
    // Close file handle if valid
    if(transmitFileCtx->fileHandle != INVALID_HANDLE_VALUE)
        CloseHandle(transmitFileCtx->fileHandle);
    
    // Manually call destructor (needed due to placement new)
    transmitFileCtx->~PerTransmitFileContext();

    // Finally, free za memory
    allocPool_.Free(transmitFileCtx, sizeof(PerTransmitFileContext));
}

void IocpConnectionHandler::InternalCleanup()
{
    for(auto& t : workerThreads_)
        if(t.joinable()) t.join();
    workerThreads_.clear();

    for(auto& t : offloadThreads_)
        if(t.joinable()) t.join();
    offloadThreads_.clear();

    acceptManager_.DeInitialize();

    if(listenSocket_ != WFX_INVALID_SOCKET) {
        closesocket(listenSocket_);
        listenSocket_ = WFX_INVALID_SOCKET;
    }

    if(iocp_) {
        CloseHandle(iocp_);
        iocp_ = nullptr;
    }

    WSACleanup();
    logger_.Info("[IOCP]: Cleaned up Socket resources.");
}

} // namespace WFX::OSSpecific