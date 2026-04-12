#include "http_response.hpp"

#include "http/common/http_detector.hpp"
#include "utils/fileops/filesystem.hpp"
#include "utils/logger/logger.hpp"
#include "utils/crypt/string.hpp"

#include <cstring>

namespace WFX::Http {

using namespace WFX::Shared;
using namespace WFX::Utils;

// vvv Constants vvv
static constexpr std::uint32_t CL_FIELD_WIDTH       = 10;
static constexpr const char*   CL_HEADER_PREFIX     = "Content-Length: ";
static constexpr std::uint32_t CL_HEADER_PREFIX_LEN = 16;
static constexpr const char*   CL_PLACEHOLDER       = "0000000000";  // 10 digits
static constexpr const char*   CL_ZERO              = "Content-Length: 0\r\n";
static constexpr std::uint32_t CL_ZERO_LEN          = 19;

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

    phase_           = ResponsePhase::FRESH;
    bodyKind_        = BodyKind::NONE;
    status_          = HttpStatus::OK;
    clOffset_        = 0;
    bodyStartOffset_ = 0;
    clNeeded_        = false;

    if(rwBuffer_)
        rwBuffer_->ClearWriteBuffer();
}

// vvv Private helpers vvv
void HttpResponse::FatalIfCommitted(const char* caller)
{
    if(phase_ == ResponsePhase::COMMITTED)
        Logger::GetInstance().Fatal("[HttpResponse]: '", caller, "()' called after Commit()");
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
        Logger::GetInstance().Fatal(
            "[HttpResponse]: Body not allowed for this status code [1xx, 204 and 304]"
        );

    InjectContentLength();
    Append("\r\n", 2);

    // Record where body bytes start
    bodyStartOffset_ = rwBuffer_->GetWriteMeta()->dataLength;
    bodyKind_        = BodyKind::BUFFERED;
    phase_           = ResponsePhase::BODY;
}

// vvv Public write API vvv
void HttpResponse::WriteStatus(HttpStatus code)
{
    FatalIfCommitted("WriteStatus");

    if(phase_ != ResponsePhase::FRESH)
        Logger::GetInstance().Fatal("[HttpResponse]: 'WriteStatus()' called after headers already written");

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

    auto& logger = Logger::GetInstance();

    if(phase_ == ResponsePhase::BODY)
        logger.Fatal("[HttpResponse]: 'WriteHeader()' called after body already started");

    // Engine-owned header, user must not set this
    // TODO: Think if we should also block Content-Length and Transfer-Encoding key as well
    if(Utils::StringCanonical::InsensitiveStringCompare(key, "connection"))
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

    auto& logger = Logger::GetInstance();

    if(phase_ == ResponsePhase::BODY)
        logger.Fatal("[HttpResponse]: 'WriteFile()' called after body already started");

    if(bodyKind_ != BodyKind::NONE)
        logger.Fatal("[HttpResponse]: 'WriteFile()' called after body kind already set");

    if(StatusForbidsBody(status_))
        logger.Fatal("[HttpResponse]: File body not allowed for this status code [1xx, 204 and 304]");

    if(autoHandle404 && !FileSystem::FileExists(path.data())) {
        WriteStatus(HttpStatus::NOT_FOUND);
        SendText(std::string_view{"File not found", 14});
        return;
    }

    EnsureHeadersOpen();

    std::uint64_t    fileSize = FileSystem::GetFileSize(path.data());
    std::string_view mime     = MimeDetector::DetectMimeFromExt({path.data(), path.size()});

    char          clVal[20];
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
    phase_    = ResponsePhase::COMMITTED;
}

void HttpResponse::WriteStream(StreamGenerator gen, bool chunked)
{
    FatalIfCommitted("WriteStream");

    auto& logger = Logger::GetInstance();

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

    stream_   = gen;
    bodyKind_ = BodyKind::STREAM;
    phase_    = ResponsePhase::COMMITTED;
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
        phase_    = ResponsePhase::COMMITTED;
        return;
    }

    // Patch Content-Length value in place using only 'GetWriteData()' + memcpy
    if(clNeeded_) {
        std::size_t bodySize = rwBuffer_->GetWriteMeta()->dataLength - bodyStartOffset_;
        char        tmp[CL_FIELD_WIDTH];
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

} // namespace WFX::Http