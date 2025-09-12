#include "engine.hpp"

#include "include/http/response.hpp" // For 'Response'
#include "http/formatters/parser/http_parser.hpp"
#include "http/formatters/serializer/http_serializer.hpp"
#include "http/routing/router.hpp"
#include "shared/apis/master_api.hpp"
#include "utils/backport/string.hpp"
#include "utils/filesystem/filesystem.hpp"
#include "utils/process/process.hpp"

#include <string>
#include <thread>

namespace WFX::Core {

Engine::Engine(bool noCache)
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

    // Let's do this cuz its getting annoying for me, if flag isn't there, we compile it
    auto& fs = FileSystem::GetFileSystem();
    
    if(!noCache && !fs.FileExists(dllPath))
        HandleUserSrcCompilation(dllDir.c_str(), dllPath.c_str());
    else
        logger_.Info("[Engine]: --no-cache flag detected, skipping user code compilation");

    // Load user's DLL file which we compiled / is cached
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
        return this->HandleRequest(socket, ctx);
    });
}

ReceiveDirective Engine::HandleRequest(WFXSocket socket, ConnectionContext& ctx)
{
    // This will be transmitted through all the layers (from here to middleware to user)
    HttpResponse res;
    Response userRes{&res, WFX::Shared::GetHttpAPIV1(), WFX::Shared::GetConfigAPIV1()};

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
            ctx.timeoutTick = connHandler_->GetCurrentTick();
            return {
                ReceiveResult::RESUME,
                HttpConnectionState::ACTIVE,
                std::string_view{}
            };
        }
        
        case HttpParseState::PARSE_EXPECT_100:
        {
            // We want to wait for the request so we won't be closing connection
            // also update timeoutTick so timeout handler doesnt kill the context
            ctx.timeoutTick = connHandler_->GetCurrentTick();
            return {
                ReceiveResult::WRITE,
                HttpConnectionState::ACTIVE,
                "HTTP/1.1 100 Continue\r\n\r\n"
            };
        }
        
        case HttpParseState::PARSE_EXPECT_417:
        {
            return {
                ReceiveResult::WRITE,
                HttpConnectionState::CLOSING_DEFAULT,
                "HTTP/1.1 417 Expectation Failed\r\n\r\n"
            };
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

            ctx.parseState  = static_cast<std::uint8_t>(HttpParseState::PARSE_IDLE);
            ctx.timeoutTick = connHandler_->GetCurrentTick();

            return HandleResponse(socket, res, ctx, shouldClose);
        }

        case HttpParseState::PARSE_ERROR:
        {   
            return {
                ReceiveResult::WRITE,
                HttpConnectionState::CLOSING_DEFAULT,
                "HTTP/1.1 400 Bad Request\r\n"
                "Connection: close\r\n"
                "Content-Length: 11\r\n"
                "Content-Type: text/plain\r\n"
                "\r\n"
                "Bad Request"
            };
        }

        case HttpParseState::PARSE_STREAMING_BODY:
        default:
        {
            return {
                ReceiveResult::WRITE,
                HttpConnectionState::CLOSING_DEFAULT,
                "HTTP/1.1 501 Not Implemented\r\n"
                "Connection: close\r\n"
                "Content-Length: 15\r\n"
                "Content-Type: text/plain\r\n"
                "\r\n"
                "Not Implemented"
            };
        }
    }
}

ReceiveDirective Engine::HandleResponse(WFXSocket socket, HttpResponse& res, ConnectionContext& ctx, bool shouldClose)
{
    auto&& [serializeResult, bodyView] = HttpSerializer::SerializeToBuffer(res, ctx.rwBuffer);

    HttpConnectionState afterWriteState = shouldClose ?
        HttpConnectionState::CLOSING_DEFAULT :
        HttpConnectionState::ACTIVE;

    switch(serializeResult)
    {
        case SerializeResult::SERIALIZE_SUCCESS:
        {
            if(res.IsFileOperation())
                return {
                    ReceiveResult::WRITE_FILE,
                    afterWriteState,
                    bodyView
                };
            
            if(shouldClose)
                return {
                    ReceiveResult::WRITE,
                    afterWriteState,
                    std::string_view{}
                };
            
            return {
                ReceiveResult::WRITE_DEFERRED,
                afterWriteState,
                std::string_view{}
            };
        }
        case SerializeResult::SERIALIZE_BUFFER_INSUFFICIENT:
        {
            // Flush current buffer and just ignore the rest of the data for now :)
            return {
                ReceiveResult::WRITE,
                afterWriteState,
                std::string_view{}
            };
        }

        case SerializeResult::SERIALIZE_BUFFER_FAILED:
        case SerializeResult::SERIALIZE_BUFFER_TOO_SMALL:
        default:
        {
            logger_.Error("[Engine]: Failed to serialize response, buffer failed or too small");
            return {
                ReceiveResult::CLOSE,
                HttpConnectionState::CLOSING_IMMEDIATE,
                std::string_view{}
            };
        }
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