#ifndef WFX_WINDOWS_IOCP_CONNECTION_HANDLER_HPP
#define WFX_WINDOWS_IOCP_CONNECTION_HANDLER_HPP

#define WIN32_LEAN_AND_MEAN

#include "accept_ex_manager.hpp"
#include "http/connection/http_connection.hpp"
#include "http/limits/tick_scheduler/tick_scheduler.hpp"
#include "utils/fixed_pool/fixed_pool.hpp"
#include "utils/hash_map/concurrent_map/concurrent_hash_map.hpp"
#include "utils/logger/logger.hpp"
#include "utils/perf_timer/perf_timer.hpp" // For debugging

#include <concurrentqueue.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>

#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <string>

#pragma comment(lib, "ws2_32.lib")

namespace WFX::OSSpecific {

using namespace WFX::Utils; // For 'Logger', 'BufferPool' and 'ConcurrentHashMap'
using namespace WFX::Http;  // For 'ReceiveResult', 'HttpConnectionHandler', 'ReceiveDirective'
using namespace moodycamel; // For ConcurrentQueue

class IocpConnectionHandler : public HttpConnectionHandler {
public:
    IocpConnectionHandler();
    ~IocpConnectionHandler();

public:
    // Socket functions
    void SetReceiveCallback(WFXSocket socket, ReceiveCallback onData)  override;
    void ResumeReceive(WFXSocket socket)                               override;
    int  Write(WFXSocket socket, std::string_view fastPathString = {}) override;
    int  WriteFile(WFXSocket socket, std::string_view path)            override;
    void MarkConnectionDirty(WFXSocket socket)                         override;
    void Close(WFXSocket socket)                                       override;

    // Getter function
    TickScheduler::TickType GetCurrentTick() override;
    HANDLE                  GetIOCPHandle()  const;

    // Control functions
    bool Initialize(const std::string& host, int port) override;
    void Run(AcceptedConnectionCallback)               override;
    void Stop()                                        override;

private: // Helper functions
    void CreateWorkerThreads(unsigned int iocpThreads, unsigned int offloadThreads);
    void CreateFlushTimer();
    void FlushWriteBuffers();
    void WorkerLoop();
    void PostReceive(WFXSocket socket);
    void HandleReceive(ConnectionContext& ctx, ReceiveDirective data, WFXSocket socket);
    
private: // Cleanup functions
    void SafeDeleteIoData(PerIoData* data, bool shouldCleanBuffer = true);
    void SafeDeleteTransmitFileCtx(PerTransmitFileContext* transmitFileCtx);
    void DeleteFlushTimer();
    void InternalCleanup();
    void InternalSocketCleanup(WFXSocket socket);

private: // Helper structs / functions used in unique_ptr deleter
    struct PerIoDataDeleter {
        IocpConnectionHandler* handler;
        bool shouldCleanBuffer = true;
        
        void operator()(PerIoData* data) const {
            handler->SafeDeleteIoData(data, shouldCleanBuffer);
        }
    };

    struct PerTransmitFileCtxDeleter {
        IocpConnectionHandler* handler;

        void operator()(PerTransmitFileContext* data) const {
            handler->SafeDeleteTransmitFileCtx(data);
        }
    };

    struct ConnectionContextDeleter {
        IocpConnectionHandler* handler;

        void operator()(ConnectionContext* ctx) {
            // Release IP limiter state
            handler->limiter_.ReleaseConnection(ctx->connInfo);
            
            // Delete the context itself
            delete ctx;
        }
    };

    // Just for ease
    using ConnectionContextPtr = std::unique_ptr<ConnectionContext, ConnectionContextDeleter>;

private: // Main shit
    SOCKET                     listenSocket_ = INVALID_SOCKET;
    HANDLE                     iocp_         = nullptr;
    std::mutex                 connectionMutex_;
    std::atomic<bool>          running_      = false;
    std::vector<std::thread>   workerThreads_;
    std::vector<std::thread>   offloadThreads_;
    AcceptedConnectionCallback acceptCallback_;

public: // Write Buffer flushing stuff
    static constexpr ULONG_PTR FLUSH_KEY = 0xF1u; // Unique key for flush events
    std::vector<WFXSocket> dirtyFlush_;           // Sockets to flush on next tick
    std::mutex             dirtyMutex_;

    // Timer queue for ms-level flush
    HANDLE timerQueue_    = nullptr;
    HANDLE flushTimer_    = nullptr;
    DWORD  flushPeriodMs_ = 5;

public:
    Logger&    logger_  = Logger::GetInstance();
    IpLimiter& limiter_ = IpLimiter::GetInstance();
    Config&    config_  = Config::GetInstance();

    TickScheduler timeoutHandler_;
    BufferPool bufferPool_{8, 1024 * 1024, [](std::size_t curSize){ return curSize * 2; }}; // For variable size allocs
    ConfigurableFixedAllocPool allocPool_{{32, 64, 128}};                                // For fixed size small allocs
    ConcurrentQueue<std::function<void(void)>> offloadCallbacks_;
    ConcurrentHashMap<SOCKET, ConnectionContextPtr> connections_{ 1024 * 1024 };

    // Main shit
    AcceptExManager acceptManager_{bufferPool_};
};

} // namespace WFX::OSSpecific

#endif // WFX_WINDOWS_IOCP_CONNECTION_HANDLER_HPP