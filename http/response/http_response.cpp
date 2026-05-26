#include "http_response.hpp"

#include "config/config.hpp"
#include "engine/template_engine.hpp"
#include "http/common/http_detector.hpp"
#include "utils/pool/buffer_pool.hpp"
#include "utils/fileops/filecache.hpp"
#include "utils/fileops/filesystem.hpp"
#include "utils/diagnostics/logger.hpp"
#include "utils/string/string.hpp"

#include <cstring>

namespace WFX::Http {

using namespace WFX::Core;
using namespace WFX::Shared;
using namespace WFX::Utils;

// vvv Constants vvv
static constexpr std::uint32_t CL_FIELD_WIDTH = 10;
static constexpr const char* CL_HEADER_PREFIX = "Content-Length: ";
static constexpr std::uint32_t CL_HEADER_PREFIX_LEN = 16;
static constexpr const char* CL_PLACEHOLDER = "0000000000"; // 10 digits
static constexpr const char* CL_ZERO = "Content-Length: 0\r\n";
static constexpr std::uint32_t CL_ZERO_LEN = 19;

// vvv Static helpers vvv
static void FormatFixed10(std::uint32_t value, char out[CL_FIELD_WIDTH])
{
    for(int i = CL_FIELD_WIDTH - 1; i >= 0; --i) {
        out[i] = '0' + (value % 10);
        value /= 10;
    }
}

static std::uint32_t FormatUInt64(std::uint64_t value, char out[20])
{
    // Formats uint64 into caller-supplied buffer, returns char count written
    if(value == 0) {
        out[0] = '0';
        return 1;
    }

    char tmp[20];
    int n = 0;

    while(value > 0) {
        tmp[n++] = '0' + static_cast<char>(value % 10);
        value /= 10;
    }

    for(int i = 0; i < n; ++i)
        out[i] = tmp[n - 1 - i];

    return static_cast<std::uint32_t>(n);
}

static bool StatusForbidsBody(HttpStatus s)
{
    std::uint16_t code = static_cast<std::uint16_t>(s);
    return (code >= 100 && code < 200) || code == 204 || code == 304;
}

// vvv Engine-facing vvv
void HttpResponse::Reset()
{
    if(bodyKind_ == BodyKind::STREAM) {
        if(stream_.ctx && stream_.Destroy)
            stream_.Destroy(stream_.ctx);

        stream_ = {};
    }

    phase_ = ResponsePhase::FRESH;
    bodyKind_ = BodyKind::NONE;
    status_ = HttpStatus::OK;
    clOffset_ = 0;
    bodyStartOffset_ = 0;
    clNeeded_ = false;

    if(rwBuffer_)
        rwBuffer_->ClearWriteBuffer();
}

// vvv Public Error API vvv
void HttpResponse::AbortWithError(HttpStatus status, std::string_view message)
{
    Reset();
    WriteStatus(status);
    SendText(message);
}

// vvv Private helpers vvv
void HttpResponse::FatalIfCommitted(const char* caller)
{
    if(phase_ == ResponsePhase::COMMITTED)
        GetLogger().Fatal("[HttpResponse]: '", caller, "()' called after Commit()");
}

void HttpResponse::EnsureStatusWritten()
{
    if(phase_ != ResponsePhase::FRESH)
        return;

    WriteStatus(status_);
}

void HttpResponse::EnsureHeadersOpen()
{
    EnsureStatusWritten();

    // Inject Connection header exactly once, right after status line
    if(phase_ == ResponsePhase::STATUS) {
        Append("Connection: ", 12);
        Append(shouldClose_ ? "close\r\n" : "keep-alive\r\n", shouldClose_ ? 7 : 12);

        phase_ = ResponsePhase::HEADERS;
    }
}

void HttpResponse::InjectContentLength()
{
    Append(CL_HEADER_PREFIX, CL_HEADER_PREFIX_LEN);

    // Record where value field starts, we will use this to patch length
    clOffset_ = rwBuffer_->GetWriteMeta()->dataLength;

    Append(CL_PLACEHOLDER, CL_FIELD_WIDTH);
    Append("\r\n", 2);

    clNeeded_ = true;
}

void HttpResponse::EnsureBodyOpen()
{
    EnsureHeadersOpen();

    if(phase_ >= ResponsePhase::BODY)
        return;

    if(StatusForbidsBody(status_))
        GetLogger().Fatal("[HttpResponse]: Body not allowed for this status code [1xx, 204 and 304]");

    InjectContentLength();
    Append("\r\n", 2);

    // Record where body bytes start
    bodyStartOffset_ = rwBuffer_->GetWriteMeta()->dataLength;
    bodyKind_ = BodyKind::BUFFERED;
    phase_ = ResponsePhase::BODY;
}

// vvv Public write API vvv
void HttpResponse::WriteStatus(HttpStatus code)
{
    FatalIfCommitted("WriteStatus");

    if(phase_ != ResponsePhase::FRESH)
        GetLogger().Fatal("[HttpResponse]: 'WriteStatus()' called after headers already written");

    status_ = code;

    Append("HTTP/1.", 7);
    Append(version_ == HttpVersion::HTTP_1_1 ? "1 " : "0 ", 2);

    std::uint16_t c = static_cast<std::uint16_t>(code);
    char cs[4];
    cs[0] = static_cast<char>('0' + c / 100);
    cs[1] = static_cast<char>('0' + (c / 10) % 10);
    cs[2] = static_cast<char>('0' + c % 10);
    cs[3] = ' ';
    Append(cs, 4);

    StringView reason = StringView::FromCString(HttpStatusToReason(code));
    Append(reason.data, static_cast<std::uint32_t>(reason.length));
    Append("\r\n", 2);

    phase_ = ResponsePhase::STATUS;
}

void HttpResponse::WriteHeader(std::string_view key, std::string_view value)
{
    FatalIfCommitted("WriteHeader");

    auto& logger = GetLogger();

    if(phase_ == ResponsePhase::BODY)
        logger.Fatal("[HttpResponse]: 'WriteHeader()' called after body already started");

    // Engine-owned header, user must not set this
    // TODO: Think if we should also block Content-Length and Transfer-Encoding key as well
    if(Utils::StringUtils::InsensitiveStringCompare(key, "connection"))
        logger.Fatal("[HttpResponse]: 'Connection' header is engine-owned, do not set it manually");

    EnsureHeadersOpen();

    Append(key.data(), static_cast<std::uint32_t>(key.size()));
    Append(": ", 2);
    Append(value.data(), static_cast<std::uint32_t>(value.size()));
    Append("\r\n", 2);

    phase_ = ResponsePhase::HEADERS;
}

void HttpResponse::WriteBodyData(std::string_view data)
{
    FatalIfCommitted("WriteBodyData");
    EnsureBodyOpen();

    if(data.size() == 0)
        return;

    Append(data.data(), static_cast<std::uint32_t>(data.size()));
}

void HttpResponse::WriteFile(std::string_view path, bool autoHandle404)
{
    FatalIfCommitted("WriteFile");

    auto& logger = GetLogger();

    if(phase_ == ResponsePhase::BODY)
        logger.Fatal("[HttpResponse]: 'WriteFile()' called after body already started");

    if(bodyKind_ != BodyKind::NONE)
        logger.Fatal("[HttpResponse]: 'WriteFile()' called after body kind already set");

    if(StatusForbidsBody(status_))
        logger.Fatal("[HttpResponse]: File body not allowed for this status code [1xx, 204 and 304]");

    if(autoHandle404 && !FileSystem::FileExists(path.data())) {
        AbortWithError(HttpStatus::NOT_FOUND, "File not found");
        return;
    }

    EnsureHeadersOpen();

    std::uint64_t fileSize = FileSystem::GetFileSize(path.data());
    std::string_view mime = MimeDetector::DetectMimeFromExt({path.data(), path.size()});

    char clVal[20];
    std::uint32_t clLen = FormatUInt64(fileSize, clVal);

    Append("Content-Length: ", 16);
    Append(clVal, clLen);
    Append("\r\n", 2);
    Append("Content-Type: ", 14);
    Append(mime.data(), static_cast<std::uint32_t>(mime.size()));
    Append("\r\n", 2);

    // Close header section, no body in 'rwBuffer' for file responses
    Append("\r\n", 2);

    // Own the path, engine retrieves it later via GetFilePath()
    filePath_.assign(path.data(), path.size());

    bodyKind_ = BodyKind::FILE;
    phase_ = ResponsePhase::COMMITTED;
}

void HttpResponse::WriteStream(StreamGenerator gen, bool chunked)
{
    FatalIfCommitted("WriteStream");

    auto& logger = GetLogger();

    if(bodyKind_ != BodyKind::NONE)
        logger.Fatal("[HttpResponse]: 'WriteStream()' called after body kind already set");

    if(StatusForbidsBody(status_))
        logger.Fatal("[HttpResponse]: Stream body not allowed for this status code [1xx, 204 and 304]");

    EnsureHeadersOpen();

    if(chunked)
        Append("Transfer-Encoding: chunked\r\n", 28);

    // Close header section
    Append("\r\n", 2);

    if(stream_.ctx && stream_.Destroy)
        stream_.Destroy(stream_.ctx);

    stream_ = gen;
    bodyKind_ = BodyKind::STREAM;
    phase_ = ResponsePhase::COMMITTED;
}

void HttpResponse::Commit()
{
    FatalIfCommitted("Commit");

    // No body written, zero-body response (e.g. 204, HEAD, error with no body)
    if(phase_ < ResponsePhase::BODY) {
        EnsureHeadersOpen();

        if(!StatusForbidsBody(status_))
            Append(CL_ZERO, CL_ZERO_LEN);

        Append("\r\n", 2);

        bodyKind_ = BodyKind::BUFFERED;
        phase_ = ResponsePhase::COMMITTED;
        return;
    }

    // Patch Content-Length value in place using only 'GetWriteData()' + memcpy
    if(clNeeded_) {
        std::size_t bodySize = rwBuffer_->GetWriteMeta()->dataLength - bodyStartOffset_;
        char tmp[CL_FIELD_WIDTH];
        FormatFixed10(bodySize, tmp);

        // PatchAt via existing API: 'GetWriteData()' gives base, clOffset_ is the byte offset
        char* base = rwBuffer_->GetWriteData();
        if(base)
            std::memcpy(base + clOffset_, tmp, CL_FIELD_WIDTH);
    }

    phase_ = ResponsePhase::COMMITTED;
}

// vvv Sugar Syntax vvv
void HttpResponse::SendText(std::string_view data)
{
    EnsureHeadersOpen();
    Append("Content-Type: text/plain\r\n", 26);
    phase_ = ResponsePhase::HEADERS;

    WriteBodyData(data);
    Commit();
}

void HttpResponse::SendFile(std::string_view path, bool autoHandle404)
{
    WriteFile(path, autoHandle404);
}

void HttpResponse::SendStream(StreamGenerator gen, bool chunked)
{
    WriteStream(gen, chunked);
}

// vvv Putting 'WriteTemplate' here because it was tooo messy :) vvv
static bool DrainCarry(const std::string& carry, char* bufBase, std::uint64_t bufSize, std::uint64_t& bufferOffset,
                       std::uint64_t& currentOffset, std::uint64_t maxSize)
{
    std::uint64_t remaining = maxSize - currentOffset;
    std::uint64_t toRead = std::min(remaining, bufSize - bufferOffset);

    std::memcpy(bufBase + bufferOffset, carry.c_str() + currentOffset, toRead);

    currentOffset += toRead;
    bufferOffset += toRead;

    return currentOffset >= maxSize;
}

static int DrainFile(BaseFilePtr& inFile, char* bufBase, std::uint64_t bufSize, std::uint64_t& bufferOffset,
                     std::uint64_t& currentOffset, std::uint64_t maxSize)
{
    std::uint64_t remaining = maxSize - currentOffset;
    std::uint64_t toRead = std::min(remaining, bufSize - bufferOffset);
    std::int64_t writtenBytes = inFile->ReadAt(bufBase + bufferOffset, toRead, currentOffset);

    if(writtenBytes < 0)
        return -1;

    currentOffset += writtenBytes;
    bufferOffset += writtenBytes;

    return currentOffset >= maxSize ? 1 : 0;
}

static bool SerializeVal(Shared::JsonRef val, std::string& carry)
{
    if(!val.Valid())
        return false;

    if(val.IsNull())
        carry.assign("null");

    else if(val.IsString()) {
        auto sv = val.AsString();
        carry.assign(sv.data(), sv.size());
    }

    else if(val.IsBool())
        carry = val.AsBool() ? "true" : "false";

    else if(val.IsInt()) {
        char tmp[20];
        auto [e, _] = std::to_chars(tmp, tmp + 20, val.AsInt());
        carry.assign(tmp, e - tmp);
    }

    else if(val.IsUInt()) {
        char tmp[20];
        auto [e, _] = std::to_chars(tmp, tmp + 20, val.AsUInt());
        carry.assign(tmp, e - tmp);
    }

    else if(val.IsDouble()) {
        char tmp[32];
        auto [e, _] = std::to_chars(tmp, tmp + 32, val.AsDouble());
        carry.assign(tmp, e - tmp);
    }

    // Array or object, just say 'object' and move on, however
    // TODO: Actually serialize it in future
    else
        carry.assign("object");

    return !carry.empty();
}

void HttpResponse::WriteTemplate(std::string&& path, Shared::JsonObject&& ctx)
{
    FatalIfCommitted("SendTemplate");

    auto meta = GetTemplateEngine().GetTemplate(std::move(path));
    if(!meta) {
        AbortWithError(HttpStatus::NOT_FOUND, "Template not found :(");
        return;
    }

    // Static template, just a file send with html content type
    if(meta->type == TemplateType::STATIC) {
        WriteHeader("Content-Type", "text/html");
        WriteFile(meta->filePath, false);
        return;
    }

    // Dynamic template
    if(!meta->gen) {
        AbortWithError(HttpStatus::INTERNAL_SERVER_ERROR, "Template missing generator");
        return;
    }

    auto [fd, size] = GetFileCache().GetFileDesc(meta->filePath);
    if(fd == WFX_INVALID_FILE) {
        AbortWithError(HttpStatus::INTERNAL_SERVER_ERROR, "Template file descriptor failed");
        return;
    }

    auto inFile = FileSystem::OpenFileExisting(fd, static_cast<std::size_t>(size));
    if(!inFile) {
        AbortWithError(HttpStatus::INTERNAL_SERVER_ERROR, "Template file operation failed");
        return;
    }

    // All good, write headers before handing off to stream
    WriteHeader("Content-Type", "text/html");

    using State = struct {
        BaseFilePtr inFile;
        Shared::JsonObject ctx;
        decltype(meta->gen.get()) gen;
        TemplateChunkType currentType;
        std::uint32_t currentState;
        std::uint64_t currentOffset;
        std::uint64_t maxSize;
        std::string carry;
    };

    auto* s = GetBufferPool().Alloc(sizeof(State));
    if(!s) {
        AbortWithError(HttpStatus::INTERNAL_SERVER_ERROR, "Template allocation failed");
        return;
    }

    // Construct object inplace
    new (s) State{std::move(inFile), std::move(ctx), meta->gen.get(), TemplateChunkType::MONOSTATE, 0, 0, 0, {}};

    WriteStream(StreamGenerator{s,

                                // Next
                                [](void* c, StreamBuffer buffer) -> StreamResult {
                                    auto& [inFile, ctx, gen, currentType, currentState, currentOffset, maxSize, carry] =
                                        *static_cast<State*>(c);

                                    // So the way we will implement this is simple
                                    // We will infinite loop and keep calling 'GetState', we will only break out if-
                                    // -we reached end of state (checked by 'GetState' returning std::monostate) or-
                                    // -buffer is full, we need to continue it in next loop

                                    std::uint64_t bufferOffset = 0;
                                    char* bufBase = buffer.buffer;
                                    std::uint64_t bufSize = buffer.size;

                                    // But before we do all the shit i said above, check if we have data remaining from-
                                    // -previous call, if yes, complete it before moving to the actual 'GetState' stuff
                                    if(currentType != TemplateChunkType::MONOSTATE) {
                                        if(currentType == TemplateChunkType::FILE) {
                                            int r = DrainFile(inFile, bufBase, bufSize, bufferOffset, currentOffset,
                                                              maxSize);

                                            if(r < 0)
                                                return {0, StreamAction::STOP_AND_CLOSE_CONN};
                                            if(r == 0)
                                                return {bufferOffset, StreamAction::CONTINUE}; // chunk unfinished
                                        }
                                        else {
                                            bool done = DrainCarry(carry, bufBase, bufSize, bufferOffset, currentOffset,
                                                                   maxSize);
                                            if(!done)
                                                return {bufferOffset, StreamAction::CONTINUE}; // chunk unfinished
                                        }

                                        currentType = TemplateChunkType::MONOSTATE;

                                        // Buffer full, yield before processing new states
                                        if(bufferOffset >= bufSize)
                                            return {bufferOffset, StreamAction::CONTINUE};
                                    }

                                    // Process new states
                                    while(true) {
                                        auto stateResult = gen->GetState(currentState, ctx);
                                        auto& chunk = stateResult.chunk;
                                        currentState = stateResult.newState;

                                        // Monostate, we reached the end of template, exit and keep-alive the connection
                                        if(std::holds_alternative<std::monostate>(chunk)) {
                                            // But before we exit, check if we have any data remaining to send
                                            // If we do, send it and in the next call, we will close
                                            if(bufferOffset > 0)
                                                return {bufferOffset, StreamAction::CONTINUE};

                                            return {0, StreamAction::STOP_AND_ALIVE_CONN};
                                        }

                                        // File chunk, read file to buffer
                                        if(auto* fc = std::get_if<FileChunk>(&chunk)) {
                                            currentType = TemplateChunkType::FILE;
                                            currentOffset = fc->offset;
                                            maxSize = fc->offset + fc->length;

                                            int r = DrainFile(inFile, bufBase, bufSize, bufferOffset, currentOffset,
                                                              maxSize);

                                            if(r < 0)
                                                return {0, StreamAction::STOP_AND_CLOSE_CONN};
                                            if(r == 0)
                                                return {bufferOffset, StreamAction::CONTINUE};

                                            currentType = TemplateChunkType::MONOSTATE;
                                            continue;
                                        }

                                        if(auto* vc = std::get_if<VariableChunk>(&chunk)) {
                                            if(!SerializeVal(vc->value, carry)) {
                                                currentType = TemplateChunkType::MONOSTATE;
                                                continue;
                                            }

                                            currentType = TemplateChunkType::VARIABLE;
                                            currentOffset = 0;
                                            maxSize = carry.size();

                                            bool done = DrainCarry(carry, bufBase, bufSize, bufferOffset, currentOffset,
                                                                   maxSize);
                                            if(!done)
                                                return {bufferOffset, StreamAction::CONTINUE};

                                            currentType = TemplateChunkType::MONOSTATE;
                                            continue;
                                        }

                                        return {0, StreamAction::STOP_AND_CLOSE_CONN};
                                    }
                                },

                                // Destroy
                                [](void* c) {
                                    static_cast<State*>(c)->~State();
                                    GetBufferPool().Free(c);
                                }

                },
                true);
}

} // namespace WFX::Http