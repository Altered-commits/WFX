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

    // 1.1 Build config
    CreateFile(projBase / "CMakeLists.txt", R"(cmake_minimum_required(VERSION 3.24)

project(user_plugin)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Output directory
set(OUTPUT_DIR "${CMAKE_BINARY_DIR}")

# --------------------------------------------------
# Compile configuration
# --------------------------------------------------
function(configure_compile target)
    target_include_directories(${target} PRIVATE
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/../WFX/include"
        "${CMAKE_SOURCE_DIR}/../WFX"
    )

    if(MSVC)
        target_compile_options(${target} PRIVATE
            /EHsc /Gw /Gy
            $<$<CONFIG:Release>:/O2 /GL /MD>
            $<$<CONFIG:Debug>:/Od /MDd>
        )
    else()
        target_compile_options(${target} PRIVATE
            -fPIC
            $<$<CONFIG:Release>:
                -O3
                -march=native
                -fvisibility=hidden
                -fvisibility-inlines-hidden
                -ffunction-sections
                -fdata-sections
            >
            $<$<CONFIG:Debug>:-O0>
        )

        set_target_properties(${target} PROPERTIES
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN YES
        )
    endif()
endfunction()

# --------------------------------------------------
# Shared-library link configuration
# --------------------------------------------------
function(configure_shared target)
    if(MSVC)
        target_link_options(${target} PRIVATE
            /DLL
            $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF /DEBUG:OFF>
            $<$<CONFIG:Debug>:/DEBUG>
        )

        set_target_properties(${target} PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
        )
    else()
        target_link_options(${target} PRIVATE
            -shared
            -fPIC
            -Wl,-rpath,../WFX/lib
        )

        if(APPLE)
            target_link_options(${target} PRIVATE
                $<$<CONFIG:Release>:-Wl,-dead_strip>
            )
        else()
            target_link_options(${target} PRIVATE
                $<$<CONFIG:Release>:
                    -Wl,--gc-sections
                    -Wl,--strip-all
                >
            )
        endif()

        set_target_properties(${target} PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
        )
    endif()

    set_target_properties(${target} PROPERTIES
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}"
    )
endfunction()

# vvv USER ENTRY (always defined) vvv
file(GLOB_RECURSE USER_ENTRY_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
)

add_library(user_entry SHARED ${USER_ENTRY_SOURCES})
configure_compile(user_entry)
configure_shared(user_entry)

# vvv USER TEMPLATES (Engine guarantees directory + source exists) vvv
file(GLOB_RECURSE USER_TEMPLATE_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/intermediate/dynamic/*.cpp"
)

add_library(user_templates SHARED ${USER_TEMPLATE_SOURCES})
configure_compile(user_templates)
configure_shared(user_templates)

# -----------
# | Summary |
# -----------
message(STATUS "================ Build Configuration ================")
message(STATUS "Targets available:")
message(STATUS "  - user_entry")
message(STATUS "  - user_templates")
message(STATUS "Output directory: ${OUTPUT_DIR}")
message(STATUS "=====================================================")
)");

    // 1.2. Git ignore file
    CreateFile(projBase / ".gitignore", R"(# Build artifacts
build/
intermediate/
)");

    // 2. Create essential config
    CreateFile(projBase / "wfx.toml", R"([Project]
middleware_list = []    # Order of middleware registered by either User or Engine

[Build]
dir_name            = "build"          # Build directory used by CMake
preferred_config    = "Debug"          # Active build configuration (must match CMake: Debug / Release)
preferred_generator = "Unix Makefiles" # Exact CMake generator used to configure the build directory

[Network]
send_buffer_max              = 2048    # Max total send buffer size per connection (in bytes)
recv_buffer_max              = 16384   # Max total recv buffer size per connection (in bytes)
recv_buffer_incr             = 4096    # Buffer growth step (in bytes)
header_reserve_hint          = 512     # Initial header allocation hint size (in bytes)
max_header_size              = 8192    # Max total size of all headers (in bytes)
max_header_count             = 64      # Max number of headers allowed
max_body_size                = 8192    # Max size of request body (in bytes)
header_timeout               = 15      # Max time limit for entire header to arrive (in seconds)
body_timeout                 = 20      # Max time limit for entire body to arrive (in seconds)
idle_timeout                 = 40      # Max time limit for a connection to stay idle (in seconds)
max_connections              = 2000    # Max total concurrent connections (Rounded up to the nearest multiple of 64)
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
min_proto_version    = 3               # Minimum TLS protocol version (1->TLSv1.1, 2->TLSv1.2, 3->TLSv1.3)
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
file_cache_size     = 20     # Number of files cached for efficiency (LFU)
template_chunk_size = 16384  # Max chunk size to read / write at once when compiling templates (in bytes)
cache_chunk_size    = 2048   # Max chunk size to read / write from template cache file (in bytes)
)");

    // 3. Bridge between engine and user code
    CreateFile(projBase / "src/api_entry.cpp", R"(#include <shared/apis/master_api.hpp>
#include <shared/utils/deferred_init_vector.hpp>
#include <shared/utils/compiler_macro.hpp>

// WARNING: DO NOT MODIFY THIS SYMBOL OR THIS FILE
// __WFXApi is reserved for WFX internal API injection
// Modifying or redefining it will break the interface between WFX and USER
const WFX::Shared::MASTER_API_TABLE* __WFXApi = nullptr;

// To prevent name mangling 
extern "C" {
    WFX_EXPORT void RegisterMasterAPI(const WFX::Shared::MASTER_API_TABLE* api)
    {
        static bool registered = false;
        if(registered)
            return;

        if(api) {
            __WFXApi = api;

            auto& constructors = WFX::Shared::__WFXDeferredConstructors;
            auto& middlewares  = WFX::Shared::__WFXDeferredMiddleware;
            auto& routes       = WFX::Shared::__WFXDeferredRoutes;

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

    // 4. Code example
    CreateFile(projBase / "src/main.cpp", R"cxx(#include <http/routes.hpp>

WFX_GET("/", [](Request& req, Response res) {
    res.SendTemplate("index.html");
});

WFX_GET("/text", [](Request& req, Response res) {
    res.SendText("Hello from WFX :)");
});

WFX_GET("/json", [](Request& req, Response res) {
    res.SendJson(Json::object({
        {"WFX says", "Hello :)"}
    }));
});
)cxx");

    // 5. Create example template and static asset
    CreateFile(projBase / "templates/index.html", R"(<html><head><link rel="stylesheet" href="/public/style.css"></head><body><h1>Hello from WFX Template</h1><script src="/public/script.js"></script></body></html>)");
    CreateFile(projBase / "public/style.css", "body { font-family: sans-serif; }");
    CreateFile(projBase / "public/script.js", "console.log(\"Hello from WFX\")");

    Logger::GetInstance().Info("[WFX]: Project '", projectName, "' created successfully!");
}

int CreateProject(const std::string& projectName)
{
    const std::filesystem::path projectPath = std::filesystem::current_path() / projectName;

    if(fs::exists(projectPath))
        Logger::GetInstance().Fatal("[WFX]: Project already exists: ", projectPath.c_str());

    ScaffoldProject(projectName);
    return 0;
}

}  // namespace WFX::New