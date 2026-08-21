// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_HTTP_RESPONSE_HPP
#define WFX_HTTP_RESPONSE_HPP

#include "config/config.hpp"
#include "shared/json/json_object.hpp"
#include "shared/abis/types.hpp"
#include "shared/abis/constants.hpp"
#include "utils/rw_buffer/rw_buffer.hpp"

namespace WFX::Http {

enum class ResponsePhase : std::uint8_t {
    FRESH = 0,
    STATUS = 1,
    HEADERS = 2,
    BODY = 3,
    COMMITTED = 4,
};

enum class BodyKind : std::uint8_t {
    NONE = 0,
    BUFFERED = 1,     // body written directly to rwBuffer
    GEN_STREAM = 2,   // StreamGenerator, body not in rwBuffer
    AWAIT_STREAM = 3, // Flush()/FlushEnd() driven, body sent incrementally during the handler coroutine
    FILE = 4,         // file path written to rwBuffer as header data, body not in rwBuffer
};

class HttpResponse {
public:
    // Called by engine before each request, resets internal state but keeps 'rwBuffer' allocation
    void Reset();

    // Phase-broken error API
    void AbortWithError(Shared::HttpStatus status, std::string_view message);

public: // Phase-enforced write API
    void WriteStatus(Shared::HttpStatus code);
    void WriteHeader(std::string_view key, std::string_view value);
    void WritePersistentHeader(std::string_view key, std::string_view value);
    void WriteBodyData(std::string_view data);
    void WriteFile(std::string_view path, bool autoHandle404);
    void WriteStream(Shared::StreamGenerator gen, bool chunked);
    void WriteTemplate(std::string&& path, Shared::JsonObject&& ctx); // Impl at end of file
    void Commit();
    void WriteFlushStart();
    bool PrepareFlushChunk(bool isFinal);
    bool FinishFlushRound(bool isFinal);

public: // Sugar syntax essentially
    void SendText(std::string_view data);
    void SendFile(std::string_view path, bool autoHandle404);
    void SendStream(Shared::StreamGenerator gen, bool chunked);

public: // Queries used by CoreEngine / Serializer
    bool IsCommitted() const
    {
        return phase_ == ResponsePhase::COMMITTED;
    }
    bool IsGenStream() const
    {
        return bodyKind_ == BodyKind::GEN_STREAM;
    }
    bool IsAwaitStream() const
    {
        return bodyKind_ == BodyKind::AWAIT_STREAM;
    }
    bool HasPendingFlushData() const noexcept
    {
        return rwBuffer_->GetWriteMeta()->dataLength > bodyStartOffset_;
    }
    bool IsFile() const
    {
        return bodyKind_ == BodyKind::FILE;
    }
    BodyKind GetBodyKind() const
    {
        return bodyKind_;
    }
    ResponsePhase GetPhase() const
    {
        return phase_;
    }
    Shared::HttpStatus GetStatus() const
    {
        return status_;
    }
    std::string TakeFilePath() noexcept
    {
        return std::move(filePath_);
    }
    Shared::StreamGenerator TakeGenerator() noexcept
    {
        Shared::StreamGenerator gen = stream_;
        stream_ = {};
        return gen;
    }

    void SetRWBuffer(Utils::RWBuffer* ptr) noexcept
    {
        rwBuffer_ = ptr;
    }
    void SetVersion(Shared::HttpVersion v) noexcept
    {
        version_ = v;
    }
    void SetShouldClose(bool sc) noexcept
    {
        shouldClose_ = sc;
    }

private:
    void EnsureStatusLineReserved(); // Writes the fixed-width status line placeholder
    void EnsureHeadersOpen();        // Ensures the status line exists, switch phase to HEADERS
    bool EnsureBodyOpen();           // Writes CL slot + \r\n separator, switch phase to BODY. False if rejected
    void InjectContentLength();      // Writes the fixed-width CL header line, saves 'clOffset_'

    // Response-contract enforcement. A handler that violates the build rules (write after commit,
    // body on a bodyless status, header after body, etc.) does not get its bad output forwarded.
    // The response is destroyed and replaced with a 500 naming the violation.
    void AbortContractViolation(const char* what);
    bool RejectIfCommitted(const char* caller);
    bool ReserveNextFlushGap();

private:
    inline bool Append(const char* data, std::uint32_t len)
    {
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): rwBuffer_ always set by CoreEngine first
        return rwBuffer_->AppendWriteData(data, len, networkConfig_.sendBufferIncSize,
                                          networkConfig_.maxSendBufferSize);
    }
    void MarkAwaitStreamDone() noexcept
    {
        phase_ = ResponsePhase::COMMITTED;
    }

private:
    Utils::RWBuffer* rwBuffer_ = nullptr;                         // |
    Shared::HttpVersion version_ = Shared::HttpVersion::HTTP_1_1; // | -> All set by 'CoreEngine', guaranteed
    bool shouldClose_ = false;                                    // |

private:
    ResponsePhase phase_ = ResponsePhase::FRESH;
    BodyKind bodyKind_ = BodyKind::NONE;
    Shared::HttpStatus status_ = Shared::HttpStatus::OK;

    bool clNeeded_ = false;           // Does this response need CL patch?
    bool aborted_ = false;            // A contract violation already replaced this with a 500
    std::size_t clOffset_ = 0;        // Offset of CL value field in rwBuffer
    std::size_t bodyStartOffset_ = 0; // Offset where body data begins

    // End of the status line + any WritePersistentHeader calls so far. AbortWithError truncates
    // back to here instead of a full Reset(), so persistent headers survive a forced-error rebuild
    std::size_t persistentHeadersEndOffset_ = 0;

    std::string filePath_;           // When bodyKind_ == FILE
    Shared::StreamGenerator stream_; // When bodyKind_ == STREAM

    // Used in 'Append' alot
    Core::NetworkConfig& networkConfig_ = Core::GetConfig().networkConfig;
};

} // namespace WFX::Http

#endif // WFX_HTTP_RESPONSE_HPP