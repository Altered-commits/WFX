#include "http_response.hpp"

#include "engine/template_engine.hpp"
#include "http/common/http_detector.hpp"
#include "http/connection/http_connection.hpp"
#include "http/common/http_global_state.hpp"
#include "include/third_party/json/json.hpp"
#include "utils/filecache/filecache.hpp"
#include "utils/filesystem/filesystem.hpp"
#include "utils/backport/string.hpp"
#include "utils/logger/logger.hpp"

namespace WFX::Http {

using namespace WFX::Utils; // For 'Logger', 'FileSystem', ...
using namespace WFX::Core;  // For 'TemplateEngine'

constexpr const char* CONTENT_TYPE_PLAIN = "text/plain";
constexpr const char* CONTENT_TYPE_JSON  = "application/json";

HttpResponse& HttpResponse::Status(HttpStatus code)
{
    status = code;
    return *this;
}

HttpResponse& HttpResponse::Set(std::string&& key, std::string&& value)
{
    headers.SetHeader(std::move(key), std::move(value));
    return *this;
}

bool HttpResponse::IsFileOperation()   const { return operationType_ == OperationType::FILE; }
bool HttpResponse::IsStreamOperation() const { return operationType_ == OperationType::STREAM_CHUNKED
                                                    || operationType_ == OperationType::STREAM_FIXED; }

OperationType HttpResponse::GetOperation() const { return operationType_; }

// vvv MAIN SHIT BELOW vvv
// vvv TEXT vvv
void HttpResponse::SendText(const char* cstr)
{
    auto& logger = Logger::GetInstance();

    // Shouldn't happen btw
    if(!cstr)
        logger.Fatal("[HttpResponse]: SendText(const char*) received nullptr");
    
    if(!std::holds_alternative<std::monostate>(body))
        logger.Fatal("[HttpResponse]: SendText() called after response body already set");
    
    if(IsFileOperation())
        logger.Fatal("[HttpResponse]: Cannot call SendText() after SendFile()");

    auto view = std::string_view{cstr};

    body = view;
    headers.SetHeader("Content-Length", UInt64ToStr(view.size()));
    headers.SetHeader("Content-Type", CONTENT_TYPE_PLAIN);
}

void HttpResponse::SendText(std::string&& str)
{
    SetTextBody(std::move(str), CONTENT_TYPE_PLAIN);
}

// vvv JSON vvv
void HttpResponse::SendJson(const Json& j)
{
    SetTextBody(std::move(j.dump()), CONTENT_TYPE_JSON);
}

void HttpResponse::SendJson(Json&& j)
{
    SetTextBody(std::move(j).dump(), CONTENT_TYPE_JSON);
}

// vvv FILE vvv
void HttpResponse::SendFile(const char* cstr, bool autoHandle404)
{
    // Shouldn't happen btw
    if(!cstr)
        Logger::GetInstance().Fatal("[HttpResponse]: SendFile(const char*) received nullptr");

    auto view = std::string_view{cstr};

    if(!ValidateFileSend(view, autoHandle404))
        return;

    body = view;
    PrepareFileHeaders(view);
}

void HttpResponse::SendFile(std::string&& path, bool autoHandle404)
{
    if(!ValidateFileSend(path, autoHandle404))
        return;

    body = std::move(path);
    PrepareFileHeaders(std::get<std::string>(body));
}

void HttpResponse::SendTemplate(const char* cstr, Json&& ctx)
{
    // Shouldn't happen btw
    if(!cstr)
        Logger::GetInstance().Fatal("[HttpResponse]: SendTemplate(const char*) received nullptr");

    SendTemplate(std::string(cstr), std::move(ctx));
}

void HttpResponse::SendTemplate(std::string&& path, Json&& ctx)
{
    if(!std::holds_alternative<std::monostate>(body))
        Logger::GetInstance()
            .Fatal("[HttpResponse]: SendTemplate() called after body already set");

    auto meta = TemplateEngine::GetInstance().GetTemplate(std::move(path));
    if(!meta) {
        Status(HttpStatus::NOT_FOUND)
            .SendText("Template not found");
        return;
    }

    // Common headers
    headers.SetHeader("Content-Type", "text/html");

    // TemplateType::STATIC is just sendfile, not that big of an issue
    if(meta->type == TemplateType::STATIC) {
        operationType_ = OperationType::FILE;
    
        // Template can be served as is, 'filePath' contains the path to template
        body = std::string_view{meta->filePath};
    
        // Set remaining headers
        headers.SetHeader("Content-Length", UInt64ToStr(meta->size));
    }
    // TemplateType::DYNAMIC needs streaming with the help of stateless generator in meta.gen
    else {
        // A bit of sanity check, this shouldn't happen btw but still
        if(!meta->gen) {
            Status(HttpStatus::INTERNAL_SERVER_ERROR)
                .SendText("[ST]_1Internal Error");
            return;
        }

        // Get the actual fd for us to perform operations on it, and while we are at it-
        // -open existing fd for reading (wrap it in common interface)
        auto [fd, size] = GetGlobalState().fileCache->GetFileDesc(meta->filePath);
        if(fd == WFX_INVALID_FILE) {
            Status(HttpStatus::INTERNAL_SERVER_ERROR)
                .SendText("[ST]_2Internal Error");
            return;
        }
        auto inFile = FileSystem::GetFileSystem().OpenFileExisting(fd, size);
        if(!inFile) {
            Status(HttpStatus::INTERNAL_SERVER_ERROR)
                .SendText("[ST]_3Internal Error");
            return;
        }

        // We will stream out the stuff pretty much, chunked encoded template
        // For generator, we can confidently take the raw pointer as template metadata will exist-
        // -till the end of life (..hopefully)
        Stream([
            inFile        = std::move(inFile),
            ctx           = std::move(ctx),
            gen           = meta->gen.get(),
            currentType   = TemplateChunkType::MONOSTATE,
            currentState  = std::uint32_t{0},
            currentOffset = std::uint64_t{0},
            maxSize       = std::uint64_t{0},
            carry         = std::string{}
        ](StreamBuffer buffer) mutable -> StreamResult {
            // So the way we will implement this is simple
            // We will infinite loop and keep calling 'GetState', we will only break out if-
            // -we reached end of state (checked by 'GetState' returning std::monostate) or-
            // -buffer is full, we need to continue it in next loop

            // Total bytes filled this call
            std::uint64_t bufferOffset = 0;

            // But before we do all the shit i said above, check if we have data remaining from-
            // -previous call, if yes, complete it before moving to the actual 'GetState' stuff
            if(currentType != TemplateChunkType::MONOSTATE) {
                switch(currentType) {
                    case TemplateChunkType::FILE:
                    {
                        // Calculate the amount of data we can safely read rn
                        std::uint64_t remaining = maxSize - currentOffset;
                        std::uint64_t freeSpace = buffer.size - bufferOffset;
                        std::uint64_t toRead    = std::min<std::uint64_t>(remaining, freeSpace);

                        std::int64_t writtenBytes = inFile->ReadAt(buffer.buffer + bufferOffset, toRead, currentOffset);
                        // Error, no point in moving forwards
                        if(writtenBytes < 0)
                            return {0, StreamAction::STOP_AND_CLOSE_CONN};

                        currentOffset += writtenBytes;
                        bufferOffset  += writtenBytes;

                        // Not finished with this chunk
                        if(currentOffset < maxSize)
                            return { bufferOffset, StreamAction::CONTINUE };

                        // Finished the chunk, reset
                        currentType = TemplateChunkType::MONOSTATE;

                        // Buffer full, yield control
                        if(bufferOffset >= buffer.size)
                            return { bufferOffset, StreamAction::CONTINUE };

                        // Otherwise continue to next state within this call
                    } break;

                    case TemplateChunkType::VARIABLE:
                    {
                        // Calculate the amount of data we can safely read rn
                        std::uint64_t remaining = maxSize - currentOffset;
                        std::uint64_t freeSpace = buffer.size - bufferOffset;
                        std::uint64_t toRead    = std::min<std::uint64_t>(remaining, freeSpace);

                        std::memcpy(buffer.buffer + bufferOffset, carry.c_str() + currentOffset, toRead);

                        currentOffset += toRead;
                        bufferOffset  += toRead;

                        // If remaining data cant fit, continue in next call
                        if(remaining > freeSpace)
                            return { bufferOffset, StreamAction::CONTINUE };

                        // Finished the chunk, reset
                        currentType = TemplateChunkType::MONOSTATE;

                        // Buffer full, yield control
                        if(bufferOffset >= buffer.size)
                            return { bufferOffset, StreamAction::CONTINUE };

                        // Otherwise continue to next state within this call
                    } break;
                }
            }

            // Begin processing new states
            while(true) {
                auto  stateResult = gen->GetState(currentState, ctx);
                auto& chunk       = stateResult.chunk;
                currentState      = stateResult.newState; // Prepare for next state

                // Monostate, we reached the end of template, exit and keep-alive the connection
                if(std::holds_alternative<std::monostate>(chunk)) {
                    // But before we exit, check if we have any data remaining to send
                    // If we do, send it and in the next call, we will close
                    if(bufferOffset > 0) {
                        currentType = TemplateChunkType::MONOSTATE;
                        return { bufferOffset, StreamAction::CONTINUE };
                    }

                    return { 0, StreamAction::STOP_AND_ALIVE_CONN };
                }

                // File chunk, read file to buffer
                if(auto* fc = std::get_if<FileChunk>(&chunk)) {
                    currentType   = TemplateChunkType::FILE;
                    currentOffset = fc->offset;
                    maxSize       = fc->offset + fc->length;

                    std::uint64_t remaining = maxSize - currentOffset;
                    std::uint64_t freeSpace = buffer.size - bufferOffset;
                    std::uint64_t toRead    = std::min<std::uint64_t>(remaining, freeSpace);

                    std::int64_t writtenBytes = inFile->ReadAt(buffer.buffer + bufferOffset, toRead, currentOffset);
                    if(writtenBytes < 0)
                        return { 0, StreamAction::STOP_AND_CLOSE_CONN };

                    currentOffset += writtenBytes;
                    bufferOffset  += writtenBytes;

                    // If chunk unfinished or buffer is full, continue in next call
                    if(currentOffset < maxSize || bufferOffset >= buffer.size)
                        return { bufferOffset, StreamAction::CONTINUE };

                    currentType = TemplateChunkType::MONOSTATE;
                    continue;
                }

                // Variable chunk, serialize JSON to string
                if(auto* vc = std::get_if<VariableChunk>(&chunk)) {
                    // Sanity check 'vc->value', if it doesn't exist or its null, render nothing
                    if(!vc->value || vc->value->is_null()) {
                        carry.clear();
                        currentType = TemplateChunkType::MONOSTATE;
                        continue;
                    }

                    // Get the string representation of json value
                    carry = vc->value->dump();

                    currentType   = TemplateChunkType::VARIABLE;
                    currentOffset = 0;
                    maxSize       = carry.size();

                    std::uint64_t remaining = maxSize - currentOffset;
                    std::uint64_t freeSpace = buffer.size - bufferOffset;
                    std::uint64_t toRead    = std::min<std::uint64_t>(remaining, freeSpace);

                    std::memcpy(buffer.buffer + bufferOffset, carry.c_str(), toRead);

                    currentOffset += toRead;
                    bufferOffset  += toRead;

                    // If remaining data cant fit or buffer is full, continue in next call
                    if(remaining > freeSpace || bufferOffset >= buffer.size)
                        return { bufferOffset, StreamAction::CONTINUE };

                    // We can fit more data in buffer
                    continue;
                }

                // Yeah uhh shouldn't happen
                return { 0, StreamAction::STOP_AND_CLOSE_CONN };
            }
        }, true, true);
    }
}

void HttpResponse::Stream(StreamGenerator generator, bool streamChunked, bool skipChecks)
{
    if(!skipChecks && !std::holds_alternative<std::monostate>(body))
        Logger::GetInstance().Fatal("[HttpResponse]: Stream() called after body already set");

    // Set the streaming-specific header
    if(streamChunked)
        headers.SetHeader("Transfer-Encoding", "chunked");

    operationType_ = streamChunked ? OperationType::STREAM_CHUNKED : OperationType::STREAM_FIXED;
    body = std::move(generator);
}

// vvv HELPER FUNCTIONS vvv
void HttpResponse::SetTextBody(std::string&& text, const char* contentType)
{
    auto& logger = Logger::GetInstance();

    if(!std::holds_alternative<std::monostate>(body))
        logger.Fatal("[HttpResponse]: Text body already set");

    if(IsFileOperation())
        logger.Fatal("[HttpResponse]: Cannot mix text and file responses");

    body = std::move(text);
    headers.SetHeader("Content-Length", UInt64ToStr(std::get<std::string>(body).size()));
    headers.SetHeader("Content-Type", contentType);
}

bool HttpResponse::ValidateFileSend(std::string_view path, bool autoHandle404, const char* funcName)
{
    auto& logger = Logger::GetInstance();
    auto& fs     = FileSystem::GetFileSystem();

    if(!std::holds_alternative<std::monostate>(body))
        logger.Fatal("[HttpResponse]: ", funcName, " called after body already set");

    if(autoHandle404 && !fs.FileExists(path.data())) {
        Status(HttpStatus::NOT_FOUND)
            .SendText("File not found");
        return false;
    }

    return true;
}

void HttpResponse::PrepareFileHeaders(std::string_view path)
{
    auto& fs = FileSystem::GetFileSystem();

    operationType_ = OperationType::FILE;

    std::uint64_t    fileSize = fs.GetFileSize(path.data());
    std::string_view mime     = MimeDetector::DetectMimeFromExt(path);

    headers.SetHeader("Content-Length", UInt64ToStr(fileSize));
    headers.SetHeader("Content-Type", std::string(mime));
}

} // namespace WFX::Http