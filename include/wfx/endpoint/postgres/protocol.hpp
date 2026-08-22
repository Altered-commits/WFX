// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_ENDPOINT_POSTGRES_PROTOCOL_HPP
#define WFX_INC_WFX_ENDPOINT_POSTGRES_PROTOCOL_HPP

// -----------------------------------------------------------------------
// PostgreSQL frontend/backend protocol v3 framing.
//
// Every message is [1 byte type][int32 length][payload], except the three
// startup-phase messages which omit the type byte. The length field counts
// itself but not the type byte, so a typed message occupies 1 + length bytes.
//
// Everything here is zero-copy on the read side: PgMessage and every
// PgFieldReader result point into the caller's receive buffer, valid only
// until the next parse call.
// -----------------------------------------------------------------------

#include <cstdint>
#include <cstring>
#include <string_view>

namespace WFX::Postgres::Detail {

// -----------------------------------------------------------------------
// Big endian conversion
//
// Written as shift/or rather than a builtin: every compiler folds this into
// a single bswap, and it stays correct on a big endian host without an ifdef.
// -----------------------------------------------------------------------
inline constexpr std::uint16_t ToBe16(std::uint16_t v) noexcept
{
    return static_cast<std::uint16_t>((v >> 8) | (v << 8));
}

inline constexpr std::uint32_t ToBe32(std::uint32_t v) noexcept
{
    return (v >> 24) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) | (v << 24);
}

inline constexpr std::uint64_t ToBe64(std::uint64_t v) noexcept
{
    return (static_cast<std::uint64_t>(ToBe32(static_cast<std::uint32_t>(v))) << 32) |
           ToBe32(static_cast<std::uint32_t>(v >> 32));
}

// Loads are unaligned by nature (payloads are packed), memcpy is the portable
// spelling and every compiler lowers it to a single load
inline std::uint16_t LoadBe16(const char* p) noexcept
{
    std::uint16_t v;
    std::memcpy(&v, p, sizeof(v));
    return ToBe16(v);
}

inline std::uint32_t LoadBe32(const char* p) noexcept
{
    std::uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return ToBe32(v);
}

inline std::uint64_t LoadBe64(const char* p) noexcept
{
    std::uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    return ToBe64(v);
}

// -----------------------------------------------------------------------
// Message type tags, FE_ for frontend and BE_ for backend. A few letters are
// reused in both directions with different meanings, which is why they are
// kept as two sets rather than one.
// -----------------------------------------------------------------------
inline constexpr char FE_BIND = 'B';
inline constexpr char FE_CLOSE = 'C';
inline constexpr char FE_COPY_DATA = 'd';
inline constexpr char FE_COPY_DONE = 'c';
inline constexpr char FE_COPY_FAIL = 'f';
inline constexpr char FE_DESCRIBE = 'D';
inline constexpr char FE_EXECUTE = 'E';
inline constexpr char FE_FLUSH = 'H';
inline constexpr char FE_PARSE = 'P';
inline constexpr char FE_PASSWORD = 'p'; // also SASLInitialResponse and SASLResponse
inline constexpr char FE_QUERY = 'Q';
inline constexpr char FE_SYNC = 'S';
inline constexpr char FE_TERMINATE = 'X';

inline constexpr char BE_AUTHENTICATION = 'R';
inline constexpr char BE_BACKEND_KEY_DATA = 'K';
inline constexpr char BE_BIND_COMPLETE = '2';
inline constexpr char BE_CLOSE_COMPLETE = '3';
inline constexpr char BE_COMMAND_COMPLETE = 'C';
inline constexpr char BE_COPY_DATA = 'd';
inline constexpr char BE_COPY_DONE = 'c';
inline constexpr char BE_COPY_IN_RESPONSE = 'G';
inline constexpr char BE_COPY_OUT_RESPONSE = 'H';
inline constexpr char BE_COPY_BOTH_RESPONSE = 'W';
inline constexpr char BE_DATA_ROW = 'D';
inline constexpr char BE_EMPTY_QUERY_RESPONSE = 'I';
inline constexpr char BE_ERROR_RESPONSE = 'E';
inline constexpr char BE_NEGOTIATE_PROTOCOL = 'v';
inline constexpr char BE_NO_DATA = 'n';
inline constexpr char BE_NOTICE_RESPONSE = 'N';
inline constexpr char BE_NOTIFICATION_RESPONSE = 'A';
inline constexpr char BE_PARAMETER_DESCRIPTION = 't';
inline constexpr char BE_PARAMETER_STATUS = 'S';
inline constexpr char BE_PARSE_COMPLETE = '1';
inline constexpr char BE_PORTAL_SUSPENDED = 's';
inline constexpr char BE_READY_FOR_QUERY = 'Z';
inline constexpr char BE_ROW_DESCRIPTION = 'T';

// Authentication subtypes, the int32 immediately after an 'R' message header
inline constexpr std::uint32_t AUTH_OK = 0;
inline constexpr std::uint32_t AUTH_KERBEROS_V5 = 2;
inline constexpr std::uint32_t AUTH_CLEARTEXT_PASSWORD = 3;
inline constexpr std::uint32_t AUTH_MD5_PASSWORD = 5;
inline constexpr std::uint32_t AUTH_GSS = 7;
inline constexpr std::uint32_t AUTH_GSS_CONTINUE = 8;
inline constexpr std::uint32_t AUTH_SSPI = 9;
inline constexpr std::uint32_t AUTH_SASL = 10;
inline constexpr std::uint32_t AUTH_SASL_CONTINUE = 11;
inline constexpr std::uint32_t AUTH_SASL_FINAL = 12;

// Transaction status byte carried by every ReadyForQuery
inline constexpr char TX_IDLE = 'I';
inline constexpr char TX_IN_TRANSACTION = 'T';
inline constexpr char TX_FAILED = 'E';

// Where a streamed request has got to. Only Parse advances this: Serialize runs
// again whenever the write buffer was too small, so anything it decided from a
// phase it had moved itself would differ between the two attempts.
enum class PgStreamPhase : std::uint8_t {
    NONE,  // not streaming, or the stream is over
    FETCH, // portal has more rows, the next round asks for them
    CLOSE, // portal is exhausted, the next round ends the transaction
    DONE,  // the closing round came back, the slot is reusable
};

// A slot runs one request at a time, so one portal name serves every stream on
// it. Streams are the only thing that names a portal; every other request binds
// the unnamed one, which Sync destroys for it.
inline constexpr std::string_view STREAM_PORTAL = "wfxp";

// Untyped startup-phase request codes, sent where a protocol version would go
inline constexpr std::uint32_t PROTOCOL_3_0 = 196608; // 0x00030000
inline constexpr std::uint32_t PROTOCOL_3_2 = 196610; // 3.1 was skipped, old pgbouncer mis-negotiated it
inline constexpr std::uint32_t CANCEL_REQUEST_CODE = 80877102;
inline constexpr std::uint32_t SSL_REQUEST_CODE = 80877103;
inline constexpr std::uint32_t GSSENC_REQUEST_CODE = 80877104;

// Header is the type byte plus the int32 length, so nothing can be decoded below this
inline constexpr std::uint32_t MESSAGE_HEADER_SIZE = 5;

// A length field counts itself, so it can never be smaller than its own width
inline constexpr std::uint32_t MIN_LENGTH_FIELD = 4;

// ErrorResponse and NoticeResponse share one layout: a sequence of
// [1 byte field code][null terminated value], terminated by a zero byte
inline constexpr char ERRF_SEVERITY = 'S';
inline constexpr char ERRF_SEVERITY_NONLOCALIZED = 'V';
inline constexpr char ERRF_SQLSTATE = 'C';
inline constexpr char ERRF_MESSAGE = 'M';
inline constexpr char ERRF_DETAIL = 'D';
inline constexpr char ERRF_HINT = 'H';
inline constexpr char ERRF_POSITION = 'P';
inline constexpr char ERRF_INTERNAL_POSITION = 'p';
inline constexpr char ERRF_INTERNAL_QUERY = 'q';
inline constexpr char ERRF_WHERE = 'W';
inline constexpr char ERRF_SCHEMA_NAME = 's';
inline constexpr char ERRF_TABLE_NAME = 't';
inline constexpr char ERRF_COLUMN_NAME = 'c';
inline constexpr char ERRF_DATA_TYPE_NAME = 'd';
inline constexpr char ERRF_CONSTRAINT_NAME = 'n';
inline constexpr char ERRF_FILE = 'F';
inline constexpr char ERRF_LINE = 'L';
inline constexpr char ERRF_ROUTINE = 'R';

// -----------------------------------------------------------------------
// PgWriter
//
// Append-only cursor over a serialize() buffer. Overflow is sticky rather
// than fatal, so a caller writes a whole message optimistically and tests
// Ok() once at the end, then returns EpSerBufferTooSmall.
//
// Begin()/End() backpatch the length field, which avoids walking a message
// twice just to size it.
// -----------------------------------------------------------------------
class PgWriter {
public:
    PgWriter(char* buf, std::uint32_t cap) noexcept : buf_(buf), cap_(cap)
    {}

    bool Ok() const noexcept
    {
        return ok_;
    }

    std::uint32_t Pos() const noexcept
    {
        return pos_;
    }

    // Typed message: type byte, then a length placeholder patched by End()
    void Begin(char type) noexcept
    {
        U8(static_cast<std::uint8_t>(type));
        msgLenPos_ = pos_;
        I32(0);
    }

    // StartupMessage, SSLRequest, CancelRequest and GSSENCRequest carry no type byte
    void BeginUntyped() noexcept
    {
        msgLenPos_ = pos_;
        I32(0);
    }

    // Length covers itself and the payload, never the type byte, so it is simply
    // the distance from where the placeholder went in to wherever we are now
    void End() noexcept
    {
        if(!ok_)
            return;

        Patch32(msgLenPos_, pos_ - msgLenPos_);
    }

    void U8(std::uint8_t v) noexcept
    {
        if(!Reserve(1))
            return;

        buf_[pos_++] = static_cast<char>(v);
    }

    void I16(std::int16_t v) noexcept
    {
        if(!Reserve(2))
            return;

        const std::uint16_t be = ToBe16(static_cast<std::uint16_t>(v));
        std::memcpy(buf_ + pos_, &be, 2);
        pos_ += 2;
    }

    void I32(std::int32_t v) noexcept
    {
        if(!Reserve(4))
            return;

        const std::uint32_t be = ToBe32(static_cast<std::uint32_t>(v));
        std::memcpy(buf_ + pos_, &be, 4);
        pos_ += 4;
    }

    void I64(std::int64_t v) noexcept
    {
        if(!Reserve(8))
            return;

        const std::uint64_t be = ToBe64(static_cast<std::uint64_t>(v));
        std::memcpy(buf_ + pos_, &be, 8);
        pos_ += 8;
    }

    void Bytes(const void* p, std::uint32_t n) noexcept
    {
        if(!Reserve(n))
            return;

        std::memcpy(buf_ + pos_, p, n);
        pos_ += n;
    }

    void Str(std::string_view s) noexcept
    {
        Bytes(s.data(), static_cast<std::uint32_t>(s.size()));
    }

    // Protocol strings are null terminated, not length prefixed
    void CStr(std::string_view s) noexcept
    {
        Str(s);
        U8(0);
    }

    // Length-prefixed value, where -1 means SQL NULL as Bind expects
    void Field(const void* p, std::int32_t n) noexcept
    {
        I32(n);
        if(n > 0)
            Bytes(p, static_cast<std::uint32_t>(n));
    }

    // Fixed-width length-prefixed values, the shape every scalar parameter takes.
    // The length and the payload share one bounds check instead of two, which
    // halves the checks on the parameter binding path.
    void FieldI16(std::int16_t v) noexcept
    {
        if(!Reserve(6))
            return;

        Put32(4);
        Put16(static_cast<std::uint16_t>(v));
    }

    void FieldI32(std::int32_t v) noexcept
    {
        if(!Reserve(8))
            return;

        Put32(4);
        Put32(static_cast<std::uint32_t>(v));
    }

    void FieldI64(std::int64_t v) noexcept
    {
        if(!Reserve(12))
            return;

        Put32(8);
        Put64(static_cast<std::uint64_t>(v));
    }

    void FieldU8(std::uint8_t v) noexcept
    {
        if(!Reserve(5))
            return;

        Put32(1);
        buf_[pos_++] = static_cast<char>(v);
    }

    void FieldNull() noexcept
    {
        I32(-1);
    }

private:
    bool Reserve(std::uint32_t n) noexcept
    {
        if(!ok_ || n > cap_ - pos_) {
            ok_ = false;
            return false;
        }

        return true;
    }

    void Patch32(std::uint32_t at, std::uint32_t v) noexcept
    {
        const std::uint32_t be = ToBe32(v);
        std::memcpy(buf_ + at, &be, 4);
    }

    // Unchecked stores, only ever reached after a Reserve() covering the whole write
    void Put16(std::uint16_t v) noexcept
    {
        const std::uint16_t be = ToBe16(v);
        std::memcpy(buf_ + pos_, &be, 2);
        pos_ += 2;
    }

    void Put32(std::uint32_t v) noexcept
    {
        const std::uint32_t be = ToBe32(v);
        std::memcpy(buf_ + pos_, &be, 4);
        pos_ += 4;
    }

    void Put64(std::uint64_t v) noexcept
    {
        const std::uint64_t be = ToBe64(v);
        std::memcpy(buf_ + pos_, &be, 8);
        pos_ += 8;
    }

private:
    char* buf_;
    std::uint32_t cap_;
    std::uint32_t pos_ = 0;
    std::uint32_t msgLenPos_ = 0;
    bool ok_ = true;
};

// -----------------------------------------------------------------------
// PgMessage
//
// One framed backend message. payload points into the receive buffer and
// excludes the header, so it is exactly what the per-type readers expect.
// -----------------------------------------------------------------------
struct PgMessage {
    char type = 0;
    const char* payload = nullptr;
    std::uint32_t len = 0;
};

enum class FrameStatus : std::uint8_t {
    OK,        // A complete message was framed
    NEED_MORE, // Header or payload still incomplete, ask for more bytes
    MALFORMED, // Length field is nonsense, the slot must be closed
};

// -----------------------------------------------------------------------
// Frames one message at buf + pos without copying. On OK, pos advances past
// the whole message, so a caller loops until NEED_MORE and then reports pos
// as 'consumed', leaving any partial tail for the next parse call.
//
// maxMessageBytes bounds a hostile or corrupt length field before it is ever
// used to index anything.
// -----------------------------------------------------------------------
inline FrameStatus FrameMessage(const char* buf, std::uint32_t len, std::uint32_t& pos, PgMessage& out,
                                std::uint32_t maxMessageBytes) noexcept
{
    if(len - pos < MESSAGE_HEADER_SIZE)
        return FrameStatus::NEED_MORE;

    const std::uint32_t declared = LoadBe32(buf + pos + 1);
    if(declared < MIN_LENGTH_FIELD || declared > maxMessageBytes)
        return FrameStatus::MALFORMED;

    // Total on the wire is the type byte plus everything the length field covers
    const std::uint32_t total = declared + 1;
    if(len - pos < total)
        return FrameStatus::NEED_MORE;

    out.type = buf[pos];
    out.payload = buf + pos + MESSAGE_HEADER_SIZE;
    out.len = declared - MIN_LENGTH_FIELD;

    pos += total;
    return FrameStatus::OK;
}

// -----------------------------------------------------------------------
// PgFieldReader
//
// Bounds-checked cursor over one message payload. Overreads are sticky and
// leave the reader not Ok() rather than trapping, so a malformed message is
// caught with one test after decoding instead of a branch per field.
// -----------------------------------------------------------------------
class PgFieldReader {
public:
    PgFieldReader(const char* p, std::uint32_t len) noexcept : p_(p), end_(p + len)
    {}

    explicit PgFieldReader(const PgMessage& m) noexcept : PgFieldReader(m.payload, m.len)
    {}

    bool Ok() const noexcept
    {
        return ok_;
    }

    bool Empty() const noexcept
    {
        return p_ >= end_;
    }

    std::uint32_t Remaining() const noexcept
    {
        return ok_ ? static_cast<std::uint32_t>(end_ - p_) : 0;
    }

    std::uint8_t U8() noexcept
    {
        if(!Take(1))
            return 0;

        return static_cast<std::uint8_t>(*(p_ - 1));
    }

    std::int16_t I16() noexcept
    {
        if(!Take(2))
            return 0;

        return static_cast<std::int16_t>(LoadBe16(p_ - 2));
    }

    std::int32_t I32() noexcept
    {
        if(!Take(4))
            return 0;

        return static_cast<std::int32_t>(LoadBe32(p_ - 4));
    }

    std::int64_t I64() noexcept
    {
        if(!Take(8))
            return 0;

        return static_cast<std::int64_t>(LoadBe64(p_ - 8));
    }

    // Null terminated protocol string, returned without its terminator
    std::string_view CStr() noexcept
    {
        if(!ok_)
            return {};

        const void* nul = std::memchr(p_, '\0', static_cast<std::size_t>(end_ - p_));
        if(!nul) {
            ok_ = false;
            return {};
        }

        const char* s = p_;
        const auto n = static_cast<std::size_t>(static_cast<const char*>(nul) - p_);
        p_ += n + 1;
        return {s, n};
    }

    std::string_view Bytes(std::uint32_t n) noexcept
    {
        if(!Take(n))
            return {};

        return {p_ - n, n};
    }

    // Everything left in the message, for payloads ending in a trailing blob
    std::string_view Rest() noexcept
    {
        if(!ok_)
            return {};

        const char* s = p_;
        const auto n = static_cast<std::size_t>(end_ - p_);
        p_ = end_;
        return {s, n};
    }

private:
    bool Take(std::uint32_t n) noexcept
    {
        if(!ok_ || static_cast<std::uint32_t>(end_ - p_) < n) {
            ok_ = false;
            return false;
        }

        p_ += n;
        return true;
    }

private:
    const char* p_;
    const char* end_;
    bool ok_ = true;
};

// -----------------------------------------------------------------------
// Startup-phase message builders
//
// These run before any request exists, straight into an onConnect send
// buffer, so they take a PgWriter rather than going through serialize().
// -----------------------------------------------------------------------
inline void WriteSslRequest(PgWriter& w) noexcept
{
    w.BeginUntyped();
    w.I32(static_cast<std::int32_t>(SSL_REQUEST_CODE));
    w.End();
}

// CancelRequest travels on its own fresh connection, never the one being
// cancelled. The 3.0 shape carries a fixed 4 byte key; 3.2 made it variable
// length, which is the only wire change between the two versions.
inline void WriteCancelRequest(PgWriter& w, std::int32_t backendPid, std::int32_t secretKey) noexcept
{
    w.BeginUntyped();
    w.I32(static_cast<std::int32_t>(CANCEL_REQUEST_CODE));
    w.I32(backendPid);
    w.I32(secretKey);
    w.End();
}

inline void WriteCancelRequestVariable(PgWriter& w, std::int32_t backendPid, std::string_view secretKey) noexcept
{
    w.BeginUntyped();
    w.I32(static_cast<std::int32_t>(CANCEL_REQUEST_CODE));
    w.I32(backendPid);
    w.Str(secretKey);
    w.End();
}

} // namespace WFX::Postgres::Detail

#endif // WFX_INC_WFX_ENDPOINT_POSTGRES_PROTOCOL_HPP
