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
    timeoutHandler_.Start([this](TickScheduler::TickType currentTick) -> void {
        std::size_t elements = 0;
        connections_.ForEachEraseIf([this, &elements](WFXSocket socket, ConnectionContextPtr& value) -> bool {
            elements += 1;
            std::uint16_t timeoutTicks = 0;

            switch(value->parseState)
            {
                case static_cast<std::uint8_t>(WFX::Http::HttpParseState::PARSE_INCOMPLETE_HEADERS):
                    timeoutTicks = config_.networkConfig.headerTimeout;
                    break;

                case static_cast<std::uint8_t>(WFX::Http::HttpParseState::PARSE_INCOMPLETE_BODY):
                    timeoutTicks = config_.networkConfig.bodyTimeout;
                    break;

                case static_cast<std::uint8_t>(WFX::Http::HttpParseState::PARSE_IDLE):
                    timeoutTicks = config_.networkConfig.idleTimeout;
                    break;

                default:
                    return false;  // Skip all other states
            }

            bool shouldExpire = timeoutHandler_.IsExpired(
                                    timeoutHandler_.GetCurrentTick(), value->timeoutTick, timeoutTicks
                                );
            
            if(shouldExpire) {
                // We only cleanup if the state is ACTIVE
                // If its CLOSING_DEFAULT or CLOSING_IMMEDIATE, another thread is handling it already
                if(value->TransitionTo(HttpConnectionState::CLOSING_IMMEDIATE)) {
                    InternalSocketCleanup(socket);
                    // Signal to 'ForEachEraseIf' that it should now erase entry from map
                    return true;
                }
            }
            // Don't erase if not expired or if another thread has control
            return false;
        });

        logger_.Info("[IOCP-Debug]: Total elements: ", elements);
    });

    return true;
}

void IocpConnectionHandler::SetReceiveCallback(WFXSocket socket, ReceiveCallback callback)
{
    if(!callback) {
        logger_.Error("[IOCP]: Invalid callback in 'SetReceiveCallback'");
        return;
    }

    auto* ctx = connections_.Get(socket);
    if(!ctx || !(*ctx))
        return;

    // Store the callback for this connection
    (*ctx)->onReceive = std::move(callback);
    
    // Hand off PostReceive to IOCP thread.
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

int IocpConnectionHandler::Write(WFXSocket socket, std::string_view fastPathString)
{
    std::size_t length = 0;
    const char* buffer = nullptr;

    // Two paths which will decide how Write functions
    // 1) If std::string_view is given, it is assumed its a static ro string
    // 2) Else, it is assumed we will use ConnectionContext rwBuffer_.writeBuffer_
    // Both managed not by Write call, but by other stuff
    // Write is expected to be called after buffering up data for (2) point, it will set dataLength-
    // -to 0 failure or not

    if(!fastPathString.empty()) {
        length = fastPathString.length();
        buffer = fastPathString.data();
    }
    else {
        auto* ctxPtr = connections_.Get(socket);
        if(!ctxPtr || !(*ctxPtr))
            return -1;
        
        // Access writeBuffer_ from ConnectionContext as it contains the write data, hopefully
        auto& rwBuffer  = (*ctxPtr)->rwBuffer;
        auto* writeData = rwBuffer.GetWriteData();
        auto* writeMeta = rwBuffer.GetWriteMeta();
    
        if(!writeMeta || !writeData || writeMeta->dataLength == 0) {
            logger_.Error("[IOCP]: Write failed - no data in rwBuffer for socket: ", socket);
            return -1;
        }

        length = writeMeta->dataLength;
        buffer = writeData;
    }

    // Because its fixed size alloc, we use alloc pool for efficiency
    PerIoData* ioData = static_cast<PerIoData*>(allocPool_.Allocate(sizeof(PerIoData)));
    if(!ioData)
        return -1;

    ioData->overlapped    = { 0 };
    ioData->operationType = PerIoOperationType::SEND;
    ioData->socket        = socket;
    ioData->wsaBuf.buf    = const_cast<char*>(buffer); // In the hopes WSASend doesn't modify it
    ioData->wsaBuf.len    = static_cast<ULONG>(length);

    DWORD bytesSent;
    int ret = WSASend(socket, &ioData->wsaBuf, 1, &bytesSent, 0, &ioData->overlapped, nullptr);
    if(ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        SafeDeleteIoData(ioData, false);
        return -1;
    }

    return 1;
}

int IocpConnectionHandler::WriteFile(WFXSocket socket, std::string_view path)
{
    // Get the connection context for write buffer
    auto* ctxPtr = connections_.Get(socket);
    if(!ctxPtr || !(*ctxPtr))
        return -1;

    auto& rwBuffer = (*ctxPtr)->rwBuffer;

    // Buffer contains header, path contains file path
    WriteMetadata* writeMeta = rwBuffer.GetWriteMeta();
    char*          writeData = rwBuffer.GetWriteData();

    // Because its fixed size alloc, we use alloc pool for efficiency
    void* rawMem = allocPool_.Allocate(sizeof(PerTransmitFileContext));
    if(!rawMem) return -1;

    // Placement new for construction
    PerTransmitFileContext* fileData = new (rawMem) PerTransmitFileContext();

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
    fileData->fileHandle     = file;
    fileData->tfb.Head       = writeData;
    fileData->tfb.HeadLength = writeMeta->dataLength;
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

    return 1;
}

void IocpConnectionHandler::MarkConnectionDirty(WFXSocket socket)
{
    // Vector is cleared each tick in WorkerLoop()
    std::lock_guard<std::mutex> lock(dirtyMutex_);
    dirtyFlush_.push_back(socket);
}

void IocpConnectionHandler::Close(WFXSocket socket)
{
    auto* ctxPtr = connections_.Get(socket);
    if(!ctxPtr || !(*ctxPtr))
        return;

    ConnectionContext& ctx = *(*ctxPtr);

    // Atomically try to seize control
    if(!ctx.TransitionTo(HttpConnectionState::CLOSING_IMMEDIATE))
        return;
    
    // We got the control. Call the helper for socket operations
    InternalSocketCleanup(socket);

    //Erase the context from the map
    if(!connections_.Erase(socket))
        logger_.Warn("[IOCP]: Erase failed for a socket that was just closed: ", socket);
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

    CreateWorkerThreads(
        config_.osSpecificConfig.workerThreadCount, config_.osSpecificConfig.callbackThreadCount
    );
    CreateFlushTimer();
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
    auto* ctxPtr = connections_.Get(socket);
    if(!ctxPtr || !(*ctxPtr))
        return;

    ConnectionContext& ctx = *(*ctxPtr);

    // Lazy-allocate receive buffer
    std::size_t defaultSize = config_.networkConfig.bufferIncrSize;
    RWBuffer&   buf         = ctx.rwBuffer;

    // Init read buffer if not already
    if(!buf.IsReadInitialized()) {
        if(!buf.InitReadBuffer(bufferPool_, defaultSize)) {
            logger_.Error("[IOCP]: Failed to allocate receive buffer for socket: ", socket);
            Close(socket);
            return;
        }

        // Page-fault avoidance
        volatile char* dataPtr = buf.GetReadData();
        dataPtr[0] = 0;
    }

    // The way we want it is simple, we have a buffer which can grow to a certain limit 'config_.networkConfig.maxRecvBufferSize'
    // The buffer, when growing, will increment in 'defaultSize' till 'ctx.dataLength' reaches 'ctx.bufferSize - 1'
    // '-1' for the null terminator :)
    ReadMetadata* meta = buf.GetReadMeta();
    if(meta->dataLength >= meta->bufferSize - 1) { // -1 for null terminator
        if(meta->bufferSize >= config_.networkConfig.maxRecvBufferSize) {
            logger_.Error("[IOCP]: Max buffer limit reached for socket: ", socket);
            Close(socket);
            return;
        }

        if(!buf.GrowReadBuffer(defaultSize, config_.networkConfig.maxRecvBufferSize)) {
            logger_.Error("[IOCP]: Failed to grow connection buffer for socket: ", socket);
            Close(socket);
            return;
        }

        meta = buf.GetReadMeta(); // Refresh after grow
    }

    // Allocate IO context
    PerIoData* ioData = static_cast<PerIoData*>(allocPool_.Allocate(sizeof(PerIoData)));
    if(!ioData) {
        logger_.Error("[IOCP]: Failed to allocate PerIoData");
        Close(socket);
        return;
    }

    // Compute safe length that won't exceed buffer or policy. -1 for null terminator :)
    const std::size_t remainingBufferSize = meta->bufferSize - meta->dataLength - 1;

    ioData->overlapped    = {};
    ioData->operationType = PerIoOperationType::RECV;
    ioData->socket        = socket;
    ioData->wsaBuf.buf    = buf.GetReadData() + meta->dataLength;
    ioData->wsaBuf.len    = static_cast<ULONG>(remainingBufferSize);

    DWORD flags = 0, bytesRecv = 0;
    int ret = WSARecv(socket, &ioData->wsaBuf, 1, &bytesRecv, &flags, &ioData->overlapped, nullptr);
    if(ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        logger_.Debug("[IOCP]: WSARecv failed immediately with error code: ", WSAGetLastError(), " on socket: ", socket);
        SafeDeleteIoData(ioData, false);
        Close(socket);
        return;
    }
}

void IocpConnectionHandler::HandleReceive(ConnectionContext& ctx, ReceiveDirective data, WFXSocket socket)
{
    int writeResult = 0;

    // Quite important, or engine will stay stuck in OCCUPIED state leaking connections
    ctx.SetState(data.state);

    switch(data.action)
    {
        case ReceiveResult::RESUME:
        {
            // Just post another WSARecv on this socket so IOCP continues reading
            PostReceive(socket);
            return;
        }
        
        case ReceiveResult::WRITE:
        {
            writeResult = Write(socket, data.staticBody);
            break;
        }

        case ReceiveResult::WRITE_FILE:
        {
            writeResult = WriteFile(socket, data.staticBody);
            break;
        }

        case ReceiveResult::WRITE_DEFERRED:
        {
            // Just mark the connection dirty and move on
            MarkConnectionDirty(socket);
            return;
        }
        
        case ReceiveResult::CLOSE:
        {
            Close(socket);
            return;
        }
    }

    // Here we handle the case where Write call failed and return -1
    // Because Write will not post any event to WorkerLoop, we need to manually-
    // -cleanup the connection
    if(writeResult < 0) {
        ctx.SetState(HttpConnectionState::CLOSING_IMMEDIATE);
        Close(socket);
        return;
    }

    // Because engine isn't doing state resetting, it's our responsibility to do it
    if(data.state != HttpConnectionState::CLOSING_DEFAULT) {
        ctx.rwBuffer.GetReadMeta()->dataLength = 0;
        ctx.expectedBodyLength = 0;
        ctx.trackBytes         = 0;
        // state remains ACTIVE
    }
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

        if(key == FLUSH_KEY) {
            FlushWriteBuffers();
            continue;
        }

        if(!overlapped)
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
                // We do not need buffer to be released as ioData does not own the buffer, ConnectionContext does
                // Hence the 'false' in the PerIoDataDeleter
                std::unique_ptr<PerIoData, PerIoDataDeleter> ioData(
                    static_cast<PerIoData*>(base), PerIoDataDeleter{this, false}
                );
                SOCKET socket = ioData->socket;

                if(!result) {
                    if(GetLastError() != ERROR_OPERATION_ABORTED)
                        Close(socket);
                    
                    break; // Always free overlapped via unique_ptr
                }

                if(bytesTransferred <= 0) {
                    Close(socket);
                    break;
                }

                auto* ctxPtr = connections_.Get(socket);
                // This can happen if closed by timeout handler. Not an error
                if(!ctxPtr || !(*ctxPtr))
                    break;

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
                    ctx.SetState(HttpConnectionState::CLOSING_DEFAULT);

                    // Not going to bother checking for return value. This is just for response
                    Write(socket, std::string_view(kRateLimitResponse));
                    break;
                }

                // Update buffer state
                ctx.rwBuffer.AdvanceReadLength(bytesTransferred);

                // Add null terminator safely
                ReadMetadata* readMeta = ctx.rwBuffer.GetReadMeta();
                char*         dataPtr  = ctx.rwBuffer.GetReadData();
                dataPtr[readMeta->dataLength] = '\0';

                // Try to gain ownership of the context, if not, no point in moving ahead
                // Only in closing state we cannot go into occupied state
                if(!ctx.TransitionTo(HttpConnectionState::OCCUPIED))
                    break;

                // The callback will handle the actual writing and closing
                // Engine provides the necessary data needed via the return value
                offloadCallbacks_.enqueue([&ctx, this, socket]() mutable {
                    auto receiveDirective = ctx.onReceive(ctx);
                    HandleReceive(ctx, receiveDirective, socket);
                });

                break;
            }

            case PerIoOperationType::SEND:
            {
                std::unique_ptr<PerIoData, PerIoDataDeleter> ioData(
                    static_cast<PerIoData*>(base), PerIoDataDeleter{this, false}
                );

                SOCKET socket = ioData->socket;

                if(!result) {
                    if(GetLastError() != ERROR_OPERATION_ABORTED)
                        Close(socket);
                    break;
                }

                auto* ctx = connections_.Get(socket);
                // Context could've been cleaned up, likely by an immediate close
                if(!ctx || !(*ctx))
                    break;

                // Reset data length after sending
                (*ctx)->rwBuffer.GetWriteMeta()->dataLength = 0;

                switch((*ctx)->GetState())
                {
                    case HttpConnectionState::CLOSING_DEFAULT:
                        Close(socket);
                        break;
                    
                    case HttpConnectionState::ACTIVE:
                        ResumeReceive(socket);
                        break;

                    // For CLOSING_IMMEDIATE, we do nothing. The thread that set-
                    // -that state is responsible for the full cleanup
                }
                break;
            }

            case PerIoOperationType::SEND_FILE:
            {
                std::unique_ptr<PerTransmitFileContext, PerTransmitFileCtxDeleter> transmitFileCtx(
                    static_cast<PerTransmitFileContext*>(base), PerTransmitFileCtxDeleter{this}
                );

                SOCKET socket = transmitFileCtx->socket;

                if(!result) {
                    if(GetLastError() != ERROR_OPERATION_ABORTED)
                        Close(socket);
                    break;
                }

                auto* ctx = connections_.Get(socket);
                // Context could've been cleaned up, likely by an immediate close
                if(!ctx || !(*ctx))
                    break;

                // Reset data length after sending
                (*ctx)->rwBuffer.GetWriteMeta()->dataLength = 0;

                switch((*ctx)->GetState())
                {
                    case HttpConnectionState::CLOSING_DEFAULT:
                        Close(socket);
                        break;
                    
                    case HttpConnectionState::ACTIVE:
                        ResumeReceive(socket);
                        break;

                    // For CLOSING_IMMEDIATE, we do nothing. The thread that set-
                    // -that state is responsible for the full cleanup
                }
                break;
            }

            case PerIoOperationType::ACCEPT:
            {
                acceptManager_.HandleAcceptCompletion(static_cast<PerIoContext*>(base));
                break;
            }

            case PerIoOperationType::ACCEPT_DEFERRED:
            {
                SOCKET socket = base->socket;
                acceptManager_.HandleSocketOptions(socket);

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

                // Just close connection and move on, most probably error with allocating memory block
                if(!connections_.Emplace(socket, std::move(connectionContext))) {
                    InternalSocketCleanup(socket);
                    break;
                }

                offloadCallbacks_.enqueue([this, socket]() mutable {
                    acceptCallback_(socket);
                });

                break;
            }

            default:
                logger_.Fatal("Unknown IOCP operation type: ", (int)opType);
                break;
        }
    }
}

void IocpConnectionHandler::CreateWorkerThreads(unsigned int iocpThreads, unsigned int offloadThreads)
{
    // Launch IOCP worker threads
    for(unsigned int i = 0; i < iocpThreads; ++i)
        workerThreads_.emplace_back(&IocpConnectionHandler::WorkerLoop, this);

    // Launch offload callback threads
    for(unsigned int i = 0; i < offloadThreads; ++i) {
        offloadThreads_.emplace_back([this]() {
            std::size_t idleCount = 0;

            while(running_) {
                std::function<void(void)> cb;
                if(offloadCallbacks_.try_dequeue(cb)) {
                    cb();
                    idleCount = 0; // Reset backoff
                }
                else {
                    // Smart backoff: spin -> yield -> sleep
                    ++idleCount;
                    if(idleCount < 64)
                        _mm_pause();
                    else if(idleCount < 256)
                        std::this_thread::yield();
                    else
                        std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
            }
        });
    }
}

void IocpConnectionHandler::CreateFlushTimer()
{
    timerQueue_ = CreateTimerQueue();
    if(!timerQueue_)
        logger_.Fatal("[IOCP]: Failed to create timer queue.");

    // Use a static thunk for callback, pass 'this' as parameter
    auto timerCb = [](PVOID param, BOOLEAN) {
        auto* self = static_cast<IocpConnectionHandler*>(param);
        // Post one flush event, exactly ONE worker thread will consume it
        PostQueuedCompletionStatus(self->GetIOCPHandle(), 0, FLUSH_KEY, nullptr);
    };

    if(!CreateTimerQueueTimer(
        &flushTimer_,
        timerQueue_,
        timerCb,
        this,
        flushPeriodMs_,
        flushPeriodMs_,
        WT_EXECUTEDEFAULT
    ))
        logger_.Fatal("[IOCP]: CreateTimerQueueTimer failed: ", GetLastError());
}

void IocpConnectionHandler::FlushWriteBuffers()
{
    // Move out to local to keep push-backs racing in parallel workers from interfering.
    std::vector<WFXSocket> batch;
    {
        std::lock_guard<std::mutex> lock(dirtyMutex_);
        if(dirtyFlush_.empty()) return;
        batch.swap(dirtyFlush_);
    }

    // Time to flush all buffers
    for(WFXSocket socket : batch) {
        auto* ctxPtr = connections_.Get(socket);
        if(!ctxPtr || !(*ctxPtr)) continue;

        auto& ctx = *(*ctxPtr);
        auto* meta = ctx.rwBuffer.GetWriteMeta();
        if(!meta || meta->dataLength == 0) continue; // Nothing to flush

        int res = Write(socket);
        if(res < 0) {
            // On send failure, close. WorkerLoop won't get a completion in this case
            ctx.SetState(HttpConnectionState::CLOSING_IMMEDIATE);
            Close(socket);
        }
    }

    // Batch destroyed here, new dirty sockets accumulate in dirtyFlush_ for next tick
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

TickScheduler::TickType IocpConnectionHandler::GetCurrentTick()
{
    return timeoutHandler_.GetCurrentTick();
}

HANDLE IocpConnectionHandler::GetIOCPHandle() const
{
    return iocp_;
}

void IocpConnectionHandler::DeleteFlushTimer()
{
    if(flushTimer_) {
        DeleteTimerQueueTimer(timerQueue_, flushTimer_, INVALID_HANDLE_VALUE);
        flushTimer_ = nullptr;
    }
    
    if(timerQueue_) {
        DeleteTimerQueueEx(timerQueue_, INVALID_HANDLE_VALUE);
        timerQueue_ = nullptr;
    }
}

void IocpConnectionHandler::InternalCleanup()
{
    // Close all of our threads
    timeoutHandler_.Stop();

    for(auto& t : workerThreads_)
        if(t.joinable()) t.join();
    workerThreads_.clear();

    for(auto& t : offloadThreads_)
        if(t.joinable()) t.join();
    offloadThreads_.clear();

    // Kill our AcceptExManager
    acceptManager_.DeInitialize();

    // Delete the flush timer
    DeleteFlushTimer();

    // Cleanup entire Windows socket system
    if(listenSocket_ != WFX_INVALID_SOCKET) {
        closesocket(listenSocket_);
        listenSocket_ = WFX_INVALID_SOCKET;
    }

    if(iocp_) {
        CloseHandle(iocp_);
        iocp_ = nullptr;
    }

    WSACleanup();
    logger_.Info("[IOCP]: Cleaned up Connection resources.");
}

void IocpConnectionHandler::InternalSocketCleanup(WFXSocket socket)
{
    CancelIoEx((HANDLE)socket, NULL);
    shutdown(socket, SD_BOTH);
    closesocket(socket);
}

} // namespace WFX::OSSpecific