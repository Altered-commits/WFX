#include "engine.hpp"

#include "include/http/response.hpp" // For 'Response'
#include "http/formatters/parser/http_parser.hpp"
#include "http/formatters/serializer/http_serializer.hpp"
#include "http/routing/router.hpp"
#include "shared/apis/master_api.hpp"
#include "utils/backport/string.hpp"
#include "utils/filesystem/filesystem.hpp"
#include "utils/process/process.hpp"

#include <iostream>
#include <string>
#include <thread>

namespace WFX::Core {

Engine::Engine()
    : connHandler_(CreateConnectionHandler())
{
    config_.LoadCoreSettings("wfx.toml");
    config_.LoadToolchainSettings("toolchain.toml");

    // This will be used in both compiling and injecting of dll
    const std::string dllDir  = config_.projectConfig.projectName + "/build/dlls/";
    const std::string dllPath = dllDir + "user_entry.dll";

    // Handle the public/ directory routing automatically
    // To serve stuff like css, js and so on
    HandlePublicRoute();

    // Compile user code (in 'src' directory) into dll which will be loaded below
    HandleUserSrcCompilation(dllDir.c_str(), dllPath.c_str());

    // Load user's DLL file which we compiled above
    HandleUserDLLInjection(dllPath.c_str());

    // Now that user code is available to us, load middleware in proper order
    HandleMiddlewareLoading();
}

void Engine::Listen(const std::string& host, int port)
{
    if(!connHandler_->Initialize(host, port))
        logger_.Fatal("[Engine]: Failed to initialize server");

    logger_.Info("[Engine]: Listening on ", host, ':', port);

    connHandler_->Run([this](WFXSocket data) {
        this->HandleConnection(data);
    });
}

void Engine::Stop()
{
    connHandler_->Stop();

    logger_.Info("[Engine]: Stopped Successfully!");
}

// vvv Internals vvv
void Engine::HandleConnection(WFXSocket socket)
{
    connHandler_->SetReceiveCallback(socket, [this, socket](ConnectionContext& ctx) {
        this->HandleRequest(socket, ctx);
    });
}

void Engine::HandleRequest(WFXSocket socket, ConnectionContext& ctx)
{
    // This will be transmitted through all the layers (from here to middleware to user)
    HttpResponse res;
    Response userRes{&res, WFX::Shared::GetHttpAPIV1(), WFX::Shared::GetConfigAPIV1()};

    // Parse (The most obvious fucking comment i could write, i'm just sleepy rn cut me some slack)
    HttpParseState state = HttpParser::Parse(ctx);

    // Version is important for Serializer to properly create a response
    // HTTP/1.1 and HTTP/2 have different formats duh
    res.version = ctx.requestInfo->version;

    switch(state)
    {        
        case HttpParseState::PARSE_INCOMPLETE_HEADERS:
        case HttpParseState::PARSE_INCOMPLETE_BODY:
        {
            // Set the current tick to ensure timeout handler doesnt kill the context
            ctx.SetState(HttpConnectionState::ACTIVE);
            ctx.timeoutTick = connHandler_->GetCurrentTick();
            connHandler_->ResumeReceive(socket);
            return;
        }
        
        case HttpParseState::PARSE_EXPECT_100:
        {
            // We want to wait for the request so we won't be closing connection
            // also update timeoutTick so timeout handler doesnt kill the context
            ctx.SetState(HttpConnectionState::ACTIVE);
            ctx.timeoutTick = connHandler_->GetCurrentTick();
            connHandler_->Write(socket, "HTTP/1.1 100 Continue\r\n\r\n");
            return;
        }
        
        case HttpParseState::PARSE_EXPECT_417:
        {
            // Close the connection whether client wants to or not
            ctx.SetState(HttpConnectionState::CLOSING_DEFAULT);
            connHandler_->Write(socket, "HTTP/1.1 417 Expectation Failed\r\n\r\n");
            return;
        }

        case HttpParseState::PARSE_SUCCESS:
        {
            // So timeout handler doesn't do some weird stuff
            ctx.parseState  = static_cast<std::uint8_t>(HttpParseState::PARSE_DATA_OCCUPIED);
            
            auto conn        = ctx.requestInfo->headers.GetHeader("Connection");
            bool shouldClose = true;

            if(!conn.empty()) {
                res.Set("Connection", std::string{conn});
                shouldClose = StringGuard::CaseInsensitiveCompare(conn, "close");
            }
            else
                res.Set("Connection", "close");

            // Get the callback for the route we got, if it doesn't exist, we display error
            auto callback = Router::GetInstance().MatchRoute(
                                ctx.requestInfo->method,
                                ctx.requestInfo->path,
                                ctx.requestInfo->pathSegments
                            );

            if(!callback)
                res.Status(HttpStatus::NOT_FOUND)
                    .SendText("404: Route not found :(");
            else {
                // middleware_.ExecuteMiddleware(*ctx.requestInfo, userRes);
                (*callback)(*ctx.requestInfo, userRes);
            }

            // Set the 'connState' to whatever the status of Connection header is
            ctx.SetState(
                shouldClose
                    ? HttpConnectionState::CLOSING_DEFAULT
                    : HttpConnectionState::ACTIVE
                );
            ctx.parseState  = static_cast<std::uint8_t>(HttpParseState::PARSE_IDLE);
            ctx.timeoutTick = connHandler_->GetCurrentTick();
            break;
        }

        case HttpParseState::PARSE_ERROR:
        {   
            res.Status(HttpStatus::BAD_REQUEST)
                .Set("Connection", "close")
                .SendText("Bad Request");
                
            // Mark the connection to be closed after write completes
            ctx.SetState(HttpConnectionState::CLOSING_DEFAULT);
            break;
        }

        case HttpParseState::PARSE_STREAMING_BODY:
        default:
            logger_.Info("[Engine]: No Impl");
            res.Status(HttpStatus::NOT_IMPLEMENTED)
                .Set("Connection", "close")
                .SendText("Not Implemented");
            
            ctx.SetState(HttpConnectionState::CLOSING_DEFAULT);
            break;
    }

    HandleResponse(socket, res, ctx);
}

void Engine::HandleResponse(WFXSocket socket, HttpResponse& res, ConnectionContext& ctx)
{
    auto&& [serializedContent, bodyView] = HttpSerializer::Serialize(res);

    int writeStatus = 0;

    // File operation via TransmitFile / sendfile, res.body contains the file path
    if(res.IsFileOperation())
        writeStatus = connHandler_->WriteFile(socket, std::move(serializedContent), bodyView);
    // Regular WSASend write (text, JSON, etc)
    else
        writeStatus = connHandler_->Write(socket, serializedContent);

    // Status < 0 is an error, no GQCS event is generated in WorkerLoop, Close connections ourselves
    if(writeStatus < 0)
    {
        ctx.SetState(HttpConnectionState::CLOSING_IMMEDIATE);
        connHandler_->Close(socket);
        return;
    }

    // If the connection is marked for closure, we DO NOT touch the context here
    // The backend connection handler will handle the final cleanup after the write completes
    if(ctx.GetState() != HttpConnectionState::CLOSING_DEFAULT) {
        ctx.dataLength         = 0;
        ctx.expectedBodyLength = 0;
        ctx.trackBytes         = 0;
        // The state correctly remains ACTIVE
    }
}

// vvv HELPER STUFF vvv
void Engine::HandlePublicRoute()
{
    Router::GetInstance().RegisterRoute(
        HttpMethod::GET, "/public/*",
        [this](HttpRequest& req, Response& res) {
            // The route is pre normalised before it reaches here, so we can safely use the-
            // -wildcard which we get, no issue of dir traversal attacks and such
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

    if(!fs.DirectoryExists(srcDir))
        logger_.Fatal("[Engine]: Failed to locate 'src' directory inside of '", projName, "/src'.");

    if(!fs.CreateDirectory(objDir))
        logger_.Fatal("[Engine]: Failed to create obj dir: ", objDir, ". Error: ", GetLastError());

    if(!fs.CreateDirectory(dllDir))
        logger_.Fatal("[Engine]: Failed to create dll dir: ", dllDir, ". Error: ", GetLastError());

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
            if(!fs.DirectoryExists(dir) && !fs.CreateDirectory(dir))
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

void Engine::HandleUserDLLInjection(const char* dllDir)
{
    HMODULE userModule = LoadLibraryA(dllDir);
    if(!userModule)
        logger_.Fatal("[Engine]: ", dllDir, " was not found.");

    // Resolve the exported function
    auto registerFn = reinterpret_cast<WFX::Shared::RegisterMasterAPIFn>(
        GetProcAddress(userModule, "RegisterMasterAPI")
    );

    if(!registerFn)
        logger_.Fatal("[Engine]: Failed to find RegisterMasterAPI() in user DLL.");
    
    // Inject API
    registerFn(WFX::Shared::GetMasterAPI());

    logger_.Info("[Engine]: Successfully injected API and initialized user module.");
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