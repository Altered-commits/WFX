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
project_name = ")" + projectName + R"("
middleware_list = ["Logger"]    # Order of middleware registered by either User or Engine

[Network]
send_buffer_max              = 2048    # Max total send buffer size per connection (in bytes)
recv_buffer_max              = 16384   # Max total recv buffer size per connection (in bytes)
recv_buffer_incr             = 4096    # Buffer growth step (in bytes)
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

[Windows]
accept_slots       = 4096    # Number of pre-allocated AcceptEx contexts
connection_threads = "auto"  # IOCP worker thread count
request_threads    = "all"   # Threads executing user handlers

[Linux]
worker_processes = 4    # Max simultaneous workers (cloned processes)
)");

    // Default route
    CreateFile(projBase / "src/api_entry.cpp", R"(#include <wfx/shared/apis/master_api.hpp>
#include <wfx/shared/utils/deferred_init_vector.hpp>
#include <wfx/shared/utils/export_macro.hpp>

// WARNING: DO NOT MODIFY THIS SYMBOL
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

            auto& routes = WFX::Shared::__WFXDeferredRoutes();

            // Run all deferred route registrations
            for(auto& fn : routes)
                fn();

            // Clean up memory
            routes.clear();
            routes.shrink_to_fit();

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