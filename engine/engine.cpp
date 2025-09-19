#include "engine.hpp"

#include "include/http/response.hpp"
#include "http/common/http_error_msgs.hpp"
#include "http/formatters/parser/http_parser.hpp"
#include "http/formatters/serializer/http_serializer.hpp"
#include "http/routing/router.hpp"
#include "shared/apis/master_api.hpp"
#include "utils/backport/string.hpp"
#include "utils/filesystem/filesystem.hpp"
#include "utils/process/process.hpp"

#if defined(__linux__)
    #include <dlfcn.h>
#endif

namespace WFX::Core {

Engine::Engine(bool noCache)
    : connHandler_(CreateConnectionHandler())
{
    // This will be used in both compiling and injecting of dll
    const std::string dllDir  = config_.projectConfig.projectName + "/build/dlls/";
#if defined(_WIN32)
    const std::string dllPath = dllDir + "user_entry.dll";
#else
    const std::string dllPath = dllDir + "user_entry.so";
#endif

    const char* dllPathCStr = dllPath.c_str();
    const char* dllDirCStr  = dllDir.c_str();

    // Handle the public/ directory routing automatically
    // To serve stuff like css, js and so on
    HandlePublicRoute();

    // Let's do this cuz its getting annoying for me, if flag isn't there, we compile it
    auto& fs = FileSystem::GetFileSystem();
    
    if(noCache || !fs.FileExists(dllPathCStr))
        HandleUserSrcCompilation(dllDirCStr, dllPathCStr);
    else
        logger_.Info("[Engine]: File already exists, skipping user code compilation");

    // Load user's DLL file which we compiled / is cached
    HandleUserDLLInjection(dllPathCStr);

    // Now that user code is available to us, load middleware in proper order
    HandleMiddlewareLoading();
}

void Engine::Listen(const std::string& host, int port)
{
    connHandler_->Initialize(host, port);

    connHandler_->SetReceiveCallback([this](ConnectionContext* ctx){
        this->HandleRequest(ctx);
    });
    connHandler_->Run();
}

void Engine::Stop()
{
    connHandler_->Stop();

    logger_.Info("[Engine]: Stopped Successfully!");
}

// vvv Internals vvv
void Engine::HandleRequest(ConnectionContext* ctx)
{
    // This will be transmitted through all the layers (from here to middleware to user)
    HttpResponse res;
    Response userRes{&res, WFX::Shared::GetHttpAPIV1(), WFX::Shared::GetConfigAPIV1()};

    HttpParseState state = HttpParser::Parse(ctx);

    // Version is important for Serializer to properly create a response
    // HTTP/1.1 and HTTP/2 have different formats dawg
    res.version = ctx->requestInfo->version;

    switch(state)
    {        
        case HttpParseState::PARSE_INCOMPLETE_HEADERS:
        case HttpParseState::PARSE_INCOMPLETE_BODY:
            // ctx->timeoutTick = connHandler_->GetCurrentTick();
            ctx->SetConnectionState(ConnectionState::CONNECTION_ALIVE);
            connHandler_->ResumeReceive(ctx);
            return;
        
        case HttpParseState::PARSE_EXPECT_100:
            // ctx->timeoutTick = connHandler_->GetCurrentTick();
            ctx->SetConnectionState(ConnectionState::CONNECTION_ALIVE);
            connHandler_->Write(ctx, "HTTP/1.1 100 Continue\r\n\r\n");
            return;
        
        case HttpParseState::PARSE_EXPECT_417:
            ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
            connHandler_->Write(ctx, "HTTP/1.1 417 Expectation Failed\r\n\r\n");
            return;

        case HttpParseState::PARSE_SUCCESS:
        {
            auto& reqInfo     = ctx->requestInfo;
            auto  conn        = reqInfo->headers.GetHeader("Connection");
            bool  shouldClose = true;

            if(!conn.empty()) {
                res.Set("Connection", std::string{conn});
                shouldClose = StringGuard::CaseInsensitiveCompare(conn, "close");
            }
            else
                res.Set("Connection", "close");

            // Get the callback for the route we got, if it doesn't exist, we display error
            auto callback = Router::GetInstance().MatchRoute(
                                reqInfo->method,
                                reqInfo->path,
                                reqInfo->pathSegments
                            );

            if(!callback)
                res.Status(HttpStatus::NOT_FOUND).SendText("404: Route not found :(");
            else {
                // middleware_.ExecuteMiddleware(*reqInfo, userRes);
                (*callback)(*reqInfo, userRes);
            }

            ctx->parseState = static_cast<std::uint8_t>(HttpParseState::PARSE_IDLE);
            // ctx->timeoutTick = connHandler_->GetCurrentTick();

            HandleResponse(res, ctx, shouldClose);
            return;
        } 

        case HttpParseState::PARSE_ERROR:
            ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
            connHandler_->Write(ctx, badRequest);
            return;

        case HttpParseState::PARSE_STREAMING_BODY:
        default:
            ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
            connHandler_->Write(ctx, notImplemented);
            return;
    }
}

void Engine::HandleResponse(HttpResponse& res, ConnectionContext* ctx, bool shouldClose)
{
    auto&& [serializeResult, bodyView] = HttpSerializer::SerializeToBuffer(res, ctx->rwBuffer);

    ConnectionState afterWriteState = shouldClose
                                        ? ConnectionState::CONNECTION_CLOSE
                                        : ConnectionState::CONNECTION_ALIVE;
    
    ctx->SetConnectionState(afterWriteState);

    switch(serializeResult)
    {
        case SerializeResult::SERIALIZE_SUCCESS:
            if(res.IsFileOperation())
                connHandler_->WriteFile(ctx, std::string(bodyView));
            else
                connHandler_->Write(ctx, std::string_view{});

            return;

        case SerializeResult::SERIALIZE_BUFFER_INSUFFICIENT:
            connHandler_->Write(ctx, std::string_view{});
            return;

        default:
            logger_.Error("[Engine]: Failed to serialize response");
            connHandler_->Close(ctx);
            return;
    }
}

// vvv HELPER STUFF vvv
void Engine::HandlePublicRoute()
{
    Router::GetInstance().RegisterRoute(
        HttpMethod::GET, "/public/*",
        [this](HttpRequest& req, Response& res) {
            // The route is pre normalised before it reaches here, so we can safely use the-
            // -wildcard which we get, no issue of directory traversal attacks and such
            auto wildcardPath = std::get<std::string_view>(req.pathSegments[0]);
            std::string fullRoute = config_.projectConfig.publicDir + wildcardPath.data();

            // Send the file
            res.Status(HttpStatus::OK)
                .SendFile(std::move(fullRoute));
        }
    );
}

void Engine::HandleUserSrcCompilation(const char* dllDir, const char* dllPath)
{
    const std::string& projName  = config_.projectConfig.projectName;
    const auto&        toolchain = config_.toolchainConfig;
    const std::string  srcDir    = projName + "/src";
    const std::string  objDir    = projName + "/build/objs";

    auto& fs   = FileSystem::GetFileSystem();
    auto& proc = ProcessUtils::GetInstance();

    if(!fs.DirectoryExists(srcDir.c_str()))
        logger_.Fatal("[Engine]: Failed to locate 'src' directory inside of '", projName, "/src'.");

    if(!fs.CreateDirectory(objDir))
        logger_.Fatal("[Engine]: Failed to create obj dir: ", objDir, '.');

    if(!fs.CreateDirectory(dllDir))
        logger_.Fatal("[Engine]: Failed to create dll dir: ", dllDir, '.');

    // Prebuild fixed portions of compiler and linker commands
    const std::string compilerBase = toolchain.ccmd + " " + toolchain.cargs + " ";
    const std::string objPrefix    = toolchain.objFlag + "\"";
    const std::string dllLinkTail  = toolchain.largs + " " + toolchain.dllFlag + "\"" + dllPath + '"';

    std::string linkCmd = toolchain.lcmd + " ";

    // Recurse through src/ files
    fs.ListDirectory(srcDir, true, [&](const std::string& cppFile) {
        if(!EndsWith(cppFile.c_str(), ".cpp") &&
            !EndsWith(cppFile.c_str(), ".cxx") &&
            !EndsWith(cppFile.c_str(), ".cc")) return;

        logger_.Info("[Engine]: Compiling src/ file: ", cppFile);

        // Construct relative path
        std::string relPath = cppFile.substr(srcDir.size());
        if(!relPath.empty() && (relPath[0] == '/' || relPath[0] == '\\'))
            relPath.erase(0, 1);

        // Replace .cpp with .obj
        std::string objFile = objDir + "/" + relPath;
        objFile.replace(objFile.size() - 4, 4, ".obj");

        // Ensure obj subdir exists
        std::size_t slash = objFile.find_last_of("/\\");
        if(slash != std::string::npos) {
            std::string dir = objFile.substr(0, slash);
            if(!fs.DirectoryExists(dir.c_str()) && !fs.CreateDirectory(dir))
                logger_.Fatal("[Engine]: Failed to create obj subdirectory: ", dir);
        }

        // Construct compile command
        std::string compileCmd = compilerBase + "\"" + cppFile + "\" " + objPrefix + objFile + "\"";
        auto result = proc.RunProcess(compileCmd);
        if(result.exitCode < 0)
            logger_.Fatal("[Engine]: Compilation failed for: ", cppFile,
                ". Engine code: ", result.exitCode, ", OS code: ", result.osCode);

        // Append obj to link command
        linkCmd += "\"" + objFile + "\" ";
    });

    // Final linking
    linkCmd += dllLinkTail;
    auto linkResult = proc.RunProcess(linkCmd);
    if(linkResult.exitCode < 0)
        logger_.Fatal("[Engine]: Linking failed. DLL not created. Error: ", linkResult.osCode);

    logger_.Info("[Engine]: User project successfully compiled to ", dllDir);
}

void Engine::HandleUserDLLInjection(const char* dllPath)
{
#if defined(_WIN32)
    // Windows
    HMODULE userModule = LoadLibraryA(dllPath);
    if (!userModule) {
        DWORD err = GetLastError();
        logger_.Fatal("[Engine]: ", dllPath, " was not found. Error: ", err);
        return; // logger_.Fatal probably terminates — keep for clarity
    }

    FARPROC rawProc = GetProcAddress(userModule, "RegisterMasterAPI");
    if (!rawProc) {
        DWORD err = GetLastError();
        logger_.Fatal("[Engine]: Failed to find RegisterMasterAPI() in user DLL. Error: ", err);
        return;
    }

    // Cast to your function type (assumes calling convention matches)
    auto registerFn = reinterpret_cast<WFX::Shared::RegisterMasterAPIFn>(rawProc);
#else
    // POSIX (Linux / macOS / *nix)
    // RTLD_NOW: resolve symbols immediately; RTLD_GLOBAL: let module export symbols globally if needed.
    void* handle = dlopen(dllPath, RTLD_NOW | RTLD_GLOBAL);
    if(!handle) {
        const char* err = dlerror();
        logger_.Fatal("[Engine]: ", dllPath, " dlopen failed: ", (err ? err : "unknown error"));
    }

    // Clear any existing error
    dlerror();
    void* rawSym = dlsym(handle, "RegisterMasterAPI");
    const char* dlsym_err = dlerror();
    if(!rawSym || dlsym_err)
        logger_.Fatal("[Engine]: Failed to find RegisterMasterAPI() in user SO: ",
                      (dlsym_err ? dlsym_err : "symbol not found"));

    auto registerFn = reinterpret_cast<WFX::Shared::RegisterMasterAPIFn>(rawSym);
#endif
    // Call into the user module to inject the API
    registerFn(WFX::Shared::GetMasterAPI());
    logger_.Info("[Engine]: Successfully injected API and initialized user module: ", dllPath);
}

void Engine::HandleMiddlewareLoading()
{
    // Just for testing, let me register simple middleware
    middleware_.RegisterMiddleware("Logger", [this](HttpRequest& req, Response& res) {
        logger_.Info("[Logger-Middleware]: Request on path: ", req.path);
        return true;
    });

    middleware_.LoadMiddlewareFromConfig(config_.projectConfig.middlewareList);

    // After we load the middleware, we no longer need the map thingy as all the stuff is properly loaded-
    // -inside of middlewareCallbacks_ stack
    // K I L L
    // I T
    middleware_.DiscardFactoryMap();
}

} // namespace WFX