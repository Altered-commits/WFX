#ifndef WFX_LINUX_IO_URING_CONNECTION_HPP
#define WFX_LINUX_IO_URING_CONNECTION_HPP

#include "config/config.hpp"
#include "http/connection/http_connection.hpp"
#include "utils/logger/logger.hpp"
#include "utils/buffer_pool/buffer_pool.hpp"

#include <liburing.h>
#include <atomic>
#include <cstring>

// NOTE: NOT TO BE HARDCODED, NEEDS TO BE FROM WITHIN SETTINGS
#define MAX_CONNECTIONS  (16 * 1024)
#define CONNECTION_WORDS (MAX_CONNECTIONS / 64)

#define MAX_CONN_ACCEPT  (64)
#define ACCEPT_WORDS     (MAX_CONN_ACCEPT / 64)

#define BACKLOG          (1 * 1024)
#define QUEUE_DEPTH      (4096)
#define BATCH_SIZE       (64)

namespace WFX::OSSpecific {

using namespace WFX::Http;  // For 'HttpConnectionHandler', 'ReceiveCallback', 'ConnectionContext', ...
using namespace WFX::Utils; // For 'Logger', 'RWBuffer', ...
using namespace WFX::Core;  // For 'Config'

struct AcceptSlot {
    EventType        eventType = EventType::EVENT_ACCEPT;
    socklen_t        addrLen   = 0;
    sockaddr_storage addr      = { 0 };
};
static_assert(offsetof(AcceptSlot, eventType) == 0, "[AcceptSlot] 'eventType' must strictly be at an offset of 0.");

class IoUringConnectionHandler : public HttpConnectionHandler {
public:
    IoUringConnectionHandler()  = default;
    ~IoUringConnectionHandler();

public: // Initializing
    void Initialize(const std::string& host, int port) override;
    void SetReceiveCallback(ReceiveCallback onData)    override;
    
public: // I/O Operations
    void ResumeReceive(ConnectionContext* ctx)                       override;
    void Write(ConnectionContext* ctx, std::string_view buffer = {}) override;
    void WriteFile(ConnectionContext* ctx, std::string_view path)    override;
    void Close(ConnectionContext* ctx)                               override;
    
public: // Main Functions
    void         Run()            override;
    HttpTickType GetCurrentTick() override;
    void         Stop()           override;

private: // Helper Functions
    int                AllocSlot(std::uint64_t* bitmap, int numWords, int maxSlots);
    void               FreeSlot(std::uint64_t* bitmap, int idx);
    
    ConnectionContext* GetConnection();
    void               ReleaseConnection(ConnectionContext* ctx);
    AcceptSlot*        GetAccept();
    void               ReleaseAccept(AcceptSlot* slot);

    void               SetNonBlocking(int fd);
    int                ResolveHostToIpv4(const char* host, in_addr* outAddr);

    void               AddAccept();
    void               AddRecv(ConnectionContext* ctx);
    void               AddSend(ConnectionContext* ctx, std::string_view msg);
    void               AddFile(ConnectionContext* ctx, std::string_view path);
    void               SubmitBatch();

private:
    // Misc
    BufferPool        pool_{1, 1024 * 1024, [](std::size_t currSize){ return currSize * 2; }};
    Config&           config_                       = Config::GetInstance();
    Logger&           logger_                       = Logger::GetInstance();
    ReceiveCallback   onReceive_;
    std::atomic<bool> running_                      = true;
    // IoUring
    int               listenFd_                     = -1;
    int               sqeBatch_                     = 0;
    io_uring          ring_                         = { 0 };
    // Connection Context
    ConnectionContext connections_[MAX_CONNECTIONS] = { ConnectionContext{} };
    std::uint64_t     connBitmap_[CONNECTION_WORDS] = { 0 };
    // Connection Accept
    AcceptSlot        acceptSlots_[MAX_CONN_ACCEPT] = { AcceptSlot{} };
    std::uint64_t     acceptBitmap_[ACCEPT_WORDS]   = { 0 };
};

} // namespace WFX::OSSpecific

#endif // WFX_LINUX_IO_URING_CONNECTION_HPP