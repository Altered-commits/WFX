#include "new.hpp"

#include "utils/logger/logger.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace WFX::CLI {

// For 'Logger'
using namespace WFX::Utils;

static void CreateFile(const fs::path& path, const std::string& content)
{
    std::ofstream outFile(path);
    if(!outFile)
        Logger::GetInstance().Fatal("[WFX]: Failed to create file: ", path);

    outFile << content;
    Logger::GetInstance().Info("[WFX]: Created: ", path.c_str());
}

static void ScaffoldProject(const std::string& projectName)
{
    const fs::path base     = fs::current_path();
    const fs::path projBase = base / projectName;

    // 1. Create core project folders
    fs::create_directories(projBase / "src");
    fs::create_directories(projBase / "public");
    fs::create_directories(projBase / "templates");

    // 2. Create essential config
    CreateFile(base / "wfx.toml", R"([Project]
project_name    = ")" + projectName + R"("
middleware_list = ["Logger"]    # Order of middleware registered by either User or Engine

[Network]
send_buffer_max              = 2048    # Max total send buffer size per connection (in bytes)
recv_buffer_max              = 16384   # Max total recv buffer size per connection (in bytes)
recv_buffer_incr             = 4096    # Buffer growth step (in bytes)
file_cache_size              = 20      # Max number of files kept in memory cache (LFU)
header_reserve_hint          = 512     # Initial header allocation hint size
max_header_size              = 8192    # Max total size of all headers
max_header_count             = 64      # Max number of headers allowed
max_body_size                = 8192    # Max size of request body
header_timeout               = 15      # Max time limit for entire header to arrive. After that the connection closes
body_timeout                 = 20      # Max time limit for entire body to arrive. After that the connection closes
idle_timeout                 = 40      # Max time limit for a connection to stay idle. After that the connection closes
max_connections              = 10000   # Max total concurrent connections
max_connections_per_ip       = 20      # Per-IP connection cap
max_request_burst_per_ip     = 10      # Initial request tokens per IP
max_requests_per_ip_per_sec  = 5       # Refill rate (tokens per second per IP)

[ENV]
env_path = "..." # Path to .env file. IMPORTANT: Except for Windows OS, chmod 600 the env file

[SSL]
cert_path            = "..."           # Path to the server certificate (PEM format)
key_path             = "..."           # Path to the private key corresponding to the certificate
tls13_ciphers        = "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256"
                                    # ^^^ Colon-separated TLSv1.3 ciphers in preference order
tls12_ciphers        = "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384"
                                    # ^^^ Colon-separated TLSv1.2 ciphers in preference order
curves               = "X25519:P-256"  # Elliptic curves preference list for ECDHE
enable_session_cache = true            # Enable server-side session caching
enable_ktls          = false           # Enable Kernel TLS (KTLS) if supported
session_cache_size   = 32768           # Max number of cached sessions
min_proto_version    = 2               # Minimum TLS protocol version (1->TLSv1.1, 2->TLSv1.2, 3->TLSv1.3)
security_level       = 2               # SSL security level (0-5)

[Windows]
accept_slots       = 4096    # Number of pre-allocated AcceptEx contexts
connection_threads = "auto"  # IOCP worker thread count
request_threads    = "all"   # Threads executing user handlers

[Linux]
worker_processes = 2      # Max simultaneous worker connections
backlog          = 1024   # Max pending connections in OS listen queue

[Linux.IoUring]
accept_slots     = 64     # Max simultaneous connections being accepted
queue_depth      = 4096   # Internal connection queue depth
batch_size       = 64     # How many connections to process per iteration
file_chunk_size  = 65536  # How big of a file chunk to send at once

[Linux.Epoll]
max_events       = 1024   # How many events should epoll handle at a time

[Misc]
file_cache_size     = 20     # Number of files cached for efficiency
template_chunk_size = 16384  # Max chunk size to read / write at once when compiling templates
cache_chunk_size    = 2048   # Max chunk size to read / write from template cache file
)");

    // Default route
    CreateFile(projBase / "src/api_entry.cpp", R"(#include <shared/apis/master_api.hpp>
#include <shared/utils/deferred_init_vector.hpp>
#include <shared/utils/export_macro.hpp>

// WARNING: DO NOT MODIFY THIS SYMBOL OR THIS FILE
// __wfx_api is reserved for WFX internal API injection
// Modifying or redefining it will break the interface between WFX and USER
const WFX::Shared::MASTER_API_TABLE* __wfx_api = nullptr;

// To prevent name mangling 
extern "C" {
    WFX_EXPORT void RegisterMasterAPI(const WFX::Shared::MASTER_API_TABLE* api)
    {
        static bool registered = false;
        if(registered)
            return;

        if(api) {
            __wfx_api = api;

            auto& constructors = WFX::Shared::__WFXDeferredConstructors();
            auto& middlewares  = WFX::Shared::__WFXDeferredMiddleware();
            auto& routes       = WFX::Shared::__WFXDeferredRoutes();

            for(auto& fn : constructors)
                fn();

            for(auto& fn : middlewares)
                fn();

            for(auto& fn : routes)
                fn();

            // Clean up memory
            WFX::Shared::__EraseDeferredVector(constructors);
            WFX::Shared::__EraseDeferredVector(middlewares);
            WFX::Shared::__EraseDeferredVector(routes);

            registered = true;
        }
    }
})");

    // 3. Create example template and static asset
    CreateFile(projBase / "templates/index.html", R"(<html><body><h1>Hello from WFX Template</h1></body></html>)");
    CreateFile(projBase / "public/style.css", "body { font-family: sans-serif; }");
    CreateFile(projBase / "public/script.js", "console.log(\"Hello from WFX\")");

    Logger::GetInstance().Info("[WFX]: Project '", projectName, "' created successfully!");
}

int CreateProject(const std::string& projectName)
{
    const std::filesystem::path projectPath = std::filesystem::current_path() / projectName;

    if(fs::exists(projectPath))
        Logger::GetInstance().Fatal("[WFX]: Project already exists: ", projectPath);

    ScaffoldProject(projectName);
    return 0;
}

}  // namespace WFX::New