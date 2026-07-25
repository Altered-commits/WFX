// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_ENDPOINT_HTTP_HPP
#define WFX_INC_WFX_ENDPOINT_HTTP_HPP

// -----------------------------------------------------------------------
// wfx/endpoint/http.hpp
// Outbound HTTP/1.1 client, built on wfx/endpoint/base.hpp's raw Endpoint<>.
//
// Provides:
//   WFX::HttpEndpoint               : the client. One instance per upstream host.
//   WFX::HttpEndpointRequest        : method / path / body / headers (caller-owned views)
//   WFX::HttpEndpointResponse       : status / body / headers (owned)
//   WFX::HttpEndpointRequestHeaders : dynamic request header list
//   WFX::HttpEndpointResponseHeaders: dynamic response header list
//   WFX::HttpEndpointConfig         : connection pool + protocol hardening knobs
//
// Speaks HTTP/1.1 only (no h2, no ALPN). Handles keep-alive, chunked-
// -transfer-encoding, Content-Length and close-delimited bodies,-
// -HEAD/204/304/1xx no-body responses, HTTP/1.0 default-close. Not handled:-
// -CONNECT tunneling, protocol upgrades, trailer headers, Transfer-Encoding-
// -other than plain "chunked".
//
// Request paths/headers are rejected (SerializeResult::ERROR) if they-
// -contain CR/LF/NUL. Response parsing enforces-
// -maxHeaderBytes/maxHeaderCount/maxBodyBytes and rejects conflicting or-
// -ambiguous Content-Length/Transfer-Encoding framing.
//
// -----------------------------------------------------------------------
// Usage
// -----------------------------------------------------------------------
//
//   inline const auto Api = WFX::HttpEndpoint{"api.example.com:443"};
//
//   WFX_GET("/proxy", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
//       auto [status, out] = co_await Api.Get("/users/42");
//       if(status != WFX::EpOk) {
//           res.Status(502).SendText("upstream error");
//           co_return;
//       }
//
//       res.Status(out->status).SendText(out->body);
//       co_return;
//   });
//
//   WFX::HttpEndpointRequestHeaders hdrs;
//   hdrs.Add("Content-Type", "application/json");
//   hdrs.Add("Authorization", authHeaderView); // must outlive the co_await
//
//   auto [status, out] = co_await Api.Post("/users", jsonBody, hdrs);
//
// Coalescing: dedupe concurrent identical requests into one backend call.
// Give HttpEndpointConfig::coalesceKey a function deriving a key from a-
// -request (0 = don't coalesce that request); every waiter still gets its-
// -own HttpEndpointResponse (deep-cloned on completion). Left null (default),-
// -none of this is engaged. Raw ABI shape like every other user-supplied-
// -EndpointDesc callback (cast reqVoid yourself).
//
//   std::uint64_t MyCoalesce(const void* reqVoid) {
//       auto& req = *static_cast<const WFX::HttpEndpointRequest*>(reqVoid);
//       if(req.method != WFX::HttpMethod::GET) return 0;
//       return WFX::Shared::Hasher::Fnv1a(req.path.data(), req.path.size());
//   }
//   inline const auto Api = WFX::HttpEndpoint{"api.example.com:443",
//       WFX::HttpEndpointConfig{.coalesceKey = &MyCoalesce}};
//
// Tuning:
//   inline const auto Api = WFX::HttpEndpoint{"api.example.com:443", WFX::HttpEndpointConfig{
//       .connLimit             = 16,
//       .requestTimeoutSeconds = 5,
//       .maxBodyBytes          = 4 * 1024 * 1024,
//   }};
//
// -----------------------------------------------------------------------

#include "base.hpp"
#include "wfx/memory.hpp"
#include "shared/abis/constants.hpp"
#include <charconv>
#include <cstring>
#include <string_view>

// Shared by both the public header list below and the wire codec further-
// -down, one definition each, kept in Detail to stay out of the public-
// -WFX:: surface.
namespace WFX::Http::Detail {

constexpr char ToLowerAscii(char c) noexcept
{
    auto uc = static_cast<unsigned char>(c);
    unsigned char isUpper = static_cast<unsigned char>(uc - 'A') < 26;
    return static_cast<char>(uc | static_cast<unsigned char>(isUpper << 5));
}

constexpr bool InsensitiveEqual(std::string_view a, std::string_view b) noexcept
{
    if(a.size() != b.size())
        return false;

    for(std::size_t i = 0; i < a.size(); ++i)
        if(ToLowerAscii(a[i]) != ToLowerAscii(b[i]))
            return false;

    return true;
}

constexpr bool HasInjectionBytes(std::string_view s) noexcept
{
    for(char c : s)
        if(c == '\r' || c == '\n' || c == '\0')
            return true;

    return false;
}

} // namespace WFX::Http::Detail

namespace WFX {

// -----------------------------------------------------------------------
// Case-insensitive header list. Grows as needed, no cap on count (response-
// -side: bounded separately by HttpEndpointConfig::maxHeaderCount). Backed-
// -by WFX::Vector; empty until the first Add()/Set().
// -----------------------------------------------------------------------
template <typename StrT> class HttpHeaderList {
public: // Main Functions
    void Add(StrT name, StrT value)
    {
        entries_.emplace_back(std::move(name), std::move(value));
    }

    void Set(StrT name, StrT value)
    {
        for(auto& e : entries_) {
            if(Http::Detail::InsensitiveEqual(e.name, name)) {
                e.value = std::move(value);
                return;
            }
        }

        Add(std::move(name), std::move(value));
    }

    bool Get(std::string_view name, std::string_view& out) const noexcept
    {
        for(const auto& e : entries_) {
            if(Http::Detail::InsensitiveEqual(e.name, name)) {
                out = std::string_view{e.value};
                return true;
            }
        }

        return false;
    }

    bool Contains(std::string_view name) const noexcept
    {
        std::string_view unused;
        return Get(name, unused);
    }

    void Clear() noexcept
    {
        entries_.clear();
    }
    void Reserve(std::size_t n)
    {
        entries_.reserve(n);
    }

    std::size_t Size() const noexcept
    {
        return entries_.size();
    }
    bool Empty() const noexcept
    {
        return entries_.empty();
    }

    auto begin() const noexcept
    {
        return entries_.begin();
    }
    auto end() const noexcept
    {
        return entries_.end();
    }

private: // Storage
    struct Entry {
        StrT name;
        StrT value;
    };

    WFX::Vector<Entry> entries_;
};

using HttpEndpointRequestHeaders = HttpHeaderList<std::string_view>;
using HttpEndpointResponseHeaders = HttpHeaderList<WFX::String>;

// -----------------------------------------------------------------------
// Outbound request. path/body/headers are caller-owned string_views,-
// -(must stay alive for the duration of the co_await).
// -----------------------------------------------------------------------
struct HttpEndpointRequest {
    HttpMethod method = HttpMethod::GET;
    std::string_view path = "/";
    std::string_view body{};
    HttpEndpointRequestHeaders headers{};
};

// -----------------------------------------------------------------------
// Parsed response. Owned, valid for as long as the enclosing-
// -EndpointOutput<HttpEndpointResponse> is.
// -----------------------------------------------------------------------
struct HttpEndpointResponse {
    std::uint16_t status = 0;
    WFX::String body;
    HttpEndpointResponseHeaders headers{};

    bool GetHeader(std::string_view name, std::string_view& out) const noexcept
    {
        return headers.Get(name, out);
    }
    bool IsSuccess() const noexcept
    {
        return status >= 200 && status < 300;
    }
};

// -----------------------------------------------------------------------
// Connection pool + protocol hardening knobs.
// -----------------------------------------------------------------------
struct HttpEndpointConfig {
    std::uint32_t connLimit = 8;              // Max simultaneous connections to this host
    std::uint16_t connectTimeoutSeconds = 10; // TCP + TLS handshake budget
    std::uint16_t requestTimeoutSeconds = 15; // Full send+receive cycle budget
    std::uint32_t idleTimeoutSeconds = 60;    // Idle pooled connections close after this long
    std::uint16_t maxReconnectAttempts = 5;   // Backoff retries before a slot is marked fatal
    std::uint16_t reconnectBackoffBaseSeconds = 1;
    std::uint16_t reconnectBackoffMaxSeconds = 30;
    Shared::EndpointTLSConfig tlsConfig = EpTlsAuto;
    std::uint32_t prewarm = 0; // Connections to open eagerly on startup

    std::uint32_t maxHeaderBytes = 16 * 1024;      // Status line + header block size cap
    std::uint32_t maxBodyBytes = 16 * 1024 * 1024; // Response body size cap
    std::uint16_t maxHeaderCount = 100;            // Header-count cap (amplification defense)

    // Dedup key from req, or 0 to skip coalescing; cast reqVoid to `const HttpEndpointRequest*` yourself
    Shared::EndpointCoalesceKeyFn coalesceKey = nullptr;
};

// -----------------------------------------------------------------------
// Implementation detail: EndpointDesc callbacks + HTTP/1.1 wire codec.
// -----------------------------------------------------------------------
namespace Http::Detail {

struct HttpEndpointLimits {
    std::uint32_t maxHeaderBytes;
    std::uint32_t maxBodyBytes;
    std::uint16_t maxHeaderCount;
};

// Shared by every connection to one HttpEndpoint; lives as long as the-
// -owning HttpEndpoint. Handed to the engine as EndpointDesc::userCtx.
struct HttpEndpointOptions {
    // A VIEW into the caller's hostPort string, NOT a copy and NOT a WFX::String.
    // Two reasons it must be a plain string_view here:
    //   1. A HttpEndpoint is declared at namespace scope (`inline const auto Api =
    //      -WFX::HttpEndpoint{"host:443"}`), so this is populated during the user-
    //      -.so's STATIC INITIALIZATION, before the worker runs GetBufferPool().Init().
    //      A pool-backed WFX::String whose value exceeds the small-string buffer (a-
    //      -long hostname) would allocate from an uninitialized pool and SIGSEGV.
    //   2. hostPort is required to be a static / long-lived string anyway (the-
    //      -deferred AllocateEndpoint captures the same pointer), so a view is safe and-
    //      -avoids the allocation + copy entirely.
    std::string_view hostHeaderValue;
    HttpEndpointLimits limits;
};

// Per-connection; survives keep-alive requests. lastMethod is written by-
// -Serialize() and read by Parse() for the same request (safe: slots here-
// -are exclusive, one request in flight at a time).
struct SlotState {
    const HttpEndpointOptions* options = nullptr;
    HttpMethod lastMethod = HttpMethod::GET;
};

inline void* CreateSlotState(void* userCtx) noexcept
{
    auto* s = New<SlotState>();
    if(s)
        s->options = static_cast<const HttpEndpointOptions*>(userCtx);

    return s;
}

inline void DestroySlotState(void* state) noexcept
{
    Delete(static_cast<SlotState*>(state));
}

// Incremental response parser state; reset (not recreated) between-
// -keep-alive requests on the same slot.
enum class ParsePhase : std::uint8_t {
    StatusLine,
    Headers,
    BodyContentLength,
    BodyChunkSize,
    BodyChunkData,
    BodyChunkLineEnd,
    BodyChunkTrailer,
    BodyUntilClose,
};

// Chained 1xx responses before a final status line are rejected past this count,-
// -backstopped anyway by requestTimeoutSeconds, but no reason to let a chatty/malicious-
// -upstream spin the parser indefinitely within that window
inline constexpr std::uint8_t kMaxInformationalResponses = 8;

struct ParseState {
    WFX::String lineAcc; // a line that spanned multiple parse() calls
    std::uint64_t bodyRemaining = 0;
    std::uint32_t headerBytes = 0; // cumulative status line + header block size
    std::uint16_t headerCount = 0;
    std::uint8_t informationalCount = 0; // number of 1xx responses discarded so far
    ParsePhase phase = ParsePhase::StatusLine;
    bool chunked = false;
    bool hasContentLength = false;
    bool closeAfter = false; // "Connection: close" seen
    bool httpMinor0 = false; // response declared HTTP/1.0
};

inline void ResetParseStateFields(ParseState& s) noexcept
{
    s.phase = ParsePhase::StatusLine;
    s.lineAcc.clear();
    s.bodyRemaining = 0;
    s.headerBytes = 0;
    s.headerCount = 0;
    s.informationalCount = 0;
    s.chunked = false;
    s.hasContentLength = false;
    s.closeAfter = false;
    s.httpMinor0 = false;
}

inline void* CreateParseState(void*) noexcept
{
    return New<ParseState>();
}

inline void DestroyParseState(void* p) noexcept
{
    Delete(static_cast<ParseState*>(p));
}

inline void ResetParseStateCb(void* p) noexcept
{
    ResetParseStateFields(*static_cast<ParseState*>(p));
}

inline void* CreateOutput(void*) noexcept
{
    return New<HttpEndpointResponse>();
}

inline void DestroyOutput(void* p) noexcept
{
    Delete(static_cast<HttpEndpointResponse*>(p));
}

inline void* CloneOutput(void* /*slotState*/, const void* srcOutputVoid) noexcept
{
    const auto& src = *static_cast<const HttpEndpointResponse*>(srcOutputVoid);

    auto* clone = New<HttpEndpointResponse>();
    if(!clone)
        return nullptr;

    *clone = src;
    return clone;
}

// Bounds-checked append-only cursor over the serialize() buffer. Append-
// -fails cleanly (false) on overflow -> caller returns EpSerBufferTooSmall-
// -so the engine retries with a larger buffer.
class BufWriter {
public:
    BufWriter(char* buf, std::uint32_t cap) noexcept : buf_(buf), cap_(cap)
    {}

public: // Main Functions
    bool Append(std::string_view s) noexcept
    {
        if(s.size() > cap_ - pos_)
            return false;

        std::memcpy(buf_ + pos_, s.data(), s.size());
        pos_ += static_cast<std::uint32_t>(s.size());
        return true;
    }

    bool Append(char c) noexcept
    {
        if(pos_ >= cap_)
            return false;

        buf_[pos_++] = c;
        return true;
    }

    std::uint32_t Pos() const noexcept
    {
        return pos_;
    }

private: // Storage
    char* buf_;
    std::uint32_t cap_;
    std::uint32_t pos_ = 0;
};

inline Shared::SerializeResult Serialize(void* slotStateVoid, const void* reqVoid, char* buf, std::uint32_t bufLen,
                                         std::uint32_t* written, std::uint64_t* /*streamKey*/) noexcept
{
    auto* state = static_cast<SlotState*>(slotStateVoid);
    const auto& req = *static_cast<const HttpEndpointRequest*>(reqVoid);

    if(HasInjectionBytes(req.path)) [[unlikely]]
        return EpSerError;

    state->lastMethod = req.method;

    bool bodyBearing =
        req.method == HttpMethod::POST || req.method == HttpMethod::PUT || req.method == HttpMethod::PATCH;

    char lenBuf[20];
    std::string_view lenSv{};

    if(!req.body.empty() || bodyBearing) {
        auto [end, ec] = std::to_chars(lenBuf, lenBuf + sizeof(lenBuf), req.body.size());
        lenSv = std::string_view{lenBuf, static_cast<std::size_t>(end - lenBuf)};
    }

    BufWriter w{buf, bufLen};

    auto sv = Shared::HttpMethodToStringView(req.method);
    bool ok = w.Append(std::string_view{sv.Data(), sv.Size()}) && w.Append(' ') && w.Append(req.path) &&
              w.Append(" HTTP/1.1\r\nHost: ") && w.Append(state->options->hostHeaderValue) && w.Append("\r\n");

    if(ok && !lenSv.empty())
        ok = w.Append("Content-Length: ") && w.Append(lenSv) && w.Append("\r\n");

    if(ok) {
        for(const auto& h : req.headers) {
            if(HasInjectionBytes(h.name) || HasInjectionBytes(h.value)) [[unlikely]]
                return EpSerError;

            // We own Host/Content-Length/Transfer-Encoding; drop caller duplicates
            if(InsensitiveEqual(h.name, "host") || InsensitiveEqual(h.name, "content-length") ||
               InsensitiveEqual(h.name, "transfer-encoding"))
                continue;

            if(!(w.Append(h.name) && w.Append(": ") && w.Append(h.value) && w.Append("\r\n"))) {
                ok = false;
                break;
            }
        }
    }

    ok = ok && w.Append("\r\n");
    ok = ok && (req.body.empty() || w.Append(req.body));

    if(!ok) [[unlikely]]
        return EpSerBufferTooSmall;

    *written = w.Pos();
    return EpSerOk;
}

// "HTTP/1.x SSS <reason>" (only HTTP/1.0 and 1.1 accepted)
inline bool ParseStatusLine(std::string_view line, HttpEndpointResponse& res, ParseState& st) noexcept
{
    if(line.size() < 12 || line.compare(0, 5, "HTTP/") != 0 || line[6] != '.')
        return false;

    char major = line[5], minor = line[7];
    if(major != '1' || (minor != '0' && minor != '1') || line[8] != ' ')
        return false;

    std::uint16_t code = 0;
    auto [ptr, ec] = std::from_chars(line.data() + 9, line.data() + 12, code);
    if(ec != std::errc{} || ptr != line.data() + 12)
        return false;

    res.status = code;
    st.httpMinor0 = (minor == '0');
    return true;
}

inline bool ParseHeaderLine(std::string_view line, HttpEndpointResponse& res, ParseState& st,
                            const HttpEndpointLimits& lim) noexcept
{
    // RFC 7230 3.2.4: obs-fold (a continuation line starting with SP/HTAB) is deprecated -> reject
    if(line.front() == ' ' || line.front() == '\t')
        return false;

    auto colon = line.find(':');
    if(colon == std::string_view::npos)
        return false;

    std::string_view name = line.substr(0, colon);
    std::string_view value = line.substr(colon + 1);

    while(!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while(!value.empty() && (value.back() == ' ' || value.back() == '\t'))
        value.remove_suffix(1);

    if(name.empty() || name.back() == ' ' || name.back() == '\t')
        return false;

    if(InsensitiveEqual(name, "content-length")) {
        // Content-Length + Transfer-Encoding together -> reject
        if(st.chunked)
            return false;

        std::uint64_t val = 0;
        auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), val);
        if(ec != std::errc{} || ptr != value.data() + value.size())
            return false;
        if(val > lim.maxBodyBytes)
            return false;
        if(st.hasContentLength && st.bodyRemaining != val) // differing duplicate -> reject
            return false;

        st.hasContentLength = true;
        st.bodyRemaining = val;
    }
    else if(InsensitiveEqual(name, "transfer-encoding")) {
        if(st.hasContentLength)
            return false;
        if(!InsensitiveEqual(value, "chunked")) // only a bare "chunked" coding is understood
            return false;

        st.chunked = true;
    }
    else if(InsensitiveEqual(name, "connection")) {
        std::size_t pos = 0;
        while(pos <= value.size()) {
            auto comma = value.find(',', pos);
            std::string_view tok =
                value.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);

            while(!tok.empty() && (tok.front() == ' ' || tok.front() == '\t'))
                tok.remove_prefix(1);
            while(!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
                tok.remove_suffix(1);

            if(InsensitiveEqual(tok, "close"))
                st.closeAfter = true;

            if(comma == std::string_view::npos)
                break;

            pos = comma + 1;
        }
    }

    if(++st.headerCount > lim.maxHeaderCount)
        return false;

    res.headers.Add(WFX::String(name), WFX::String(value));
    return true;
}

// "<hex-size>[;ext...]"
inline bool ParseChunkSizeLine(std::string_view line, std::uint64_t& sizeOut) noexcept
{
    auto semi = line.find(';');
    std::string_view hexPart = semi == std::string_view::npos ? line : line.substr(0, semi);
    if(hexPart.empty())
        return false;

    auto [ptr, ec] = std::from_chars(hexPart.data(), hexPart.data() + hexPart.size(), sizeOut, 16);
    return ec == std::errc{} && ptr == hexPart.data() + hexPart.size();
}

inline Shared::ParseResult Parse(void* slotStateVoid, void* parseStateVoid, const char* buf, std::uint32_t len,
                                 std::uint32_t* consumed, void* outObjVoid, bool isEof,
                                 std::uint64_t* /*completedKey*/) noexcept
{
    auto* state = static_cast<SlotState*>(slotStateVoid);
    auto* st = static_cast<ParseState*>(parseStateVoid);
    auto& res = *static_cast<HttpEndpointResponse*>(outObjVoid);
    const auto& lim = state->options->limits;

    std::uint32_t pos = 0;
    auto finish = [&](Shared::ParseResult r) noexcept {
        *consumed = pos;
        return r;
    };

    while(true) {
        switch(st->phase) {
            case ParsePhase::StatusLine:
            case ParsePhase::Headers:
            case ParsePhase::BodyChunkSize:
            case ParsePhase::BodyChunkLineEnd:
            case ParsePhase::BodyChunkTrailer: {
                const void* nl = std::memchr(buf + pos, '\n', len - pos);
                if(!nl) {
                    std::uint32_t remaining = len - pos;
                    if(st->lineAcc.size() + remaining > lim.maxHeaderBytes)
                        return finish(EpParseError);

                    st->lineAcc.append(buf + pos, remaining);
                    pos = len;
                    return finish(isEof ? EpParseError : EpParseIncomplete);
                }

                auto lineLenInBuf = static_cast<std::uint32_t>(static_cast<const char*>(nl) - (buf + pos));
                std::string_view line;
                if(!st->lineAcc.empty()) {
                    if(st->lineAcc.size() + lineLenInBuf > lim.maxHeaderBytes)
                        return finish(EpParseError);

                    st->lineAcc.append(buf + pos, lineLenInBuf);
                    line = st->lineAcc;
                }
                else
                    line = std::string_view{buf + pos, lineLenInBuf};

                if(!line.empty() && line.back() == '\r')
                    line.remove_suffix(1);

                pos += lineLenInBuf + 1;

                ParsePhase currentPhase = st->phase;
                if(currentPhase == ParsePhase::StatusLine || currentPhase == ParsePhase::Headers) {
                    st->headerBytes += lineLenInBuf + 1;
                    if(st->headerBytes > lim.maxHeaderBytes)
                        return finish(EpParseError);
                }

                bool ok = true;

                if(currentPhase == ParsePhase::StatusLine) {
                    ok = ParseStatusLine(line, res, *st);
                    if(ok) {
                        res.headers.Clear();
                        st->headerCount = 0;
                        st->phase = ParsePhase::Headers;
                    }
                }
                else if(currentPhase == ParsePhase::Headers) {
                    if(line.empty()) {
                        bool informational = res.status >= 100 && res.status < 200;
                        bool noBody = state->lastMethod == HttpMethod::HEAD ||
                                      res.status == static_cast<std::uint16_t>(HttpStatus::NO_CONTENT) ||
                                      res.status == static_cast<std::uint16_t>(HttpStatus::NOT_MODIFIED) ||
                                      informational;

                        if(informational) {
                            // RFC 7230 3.3.3: a 1xx is its own complete message, reset the-
                            // -flags its header block set so they can't leak into the response-
                            // -that follows on the same connection
                            if(++st->informationalCount > kMaxInformationalResponses)
                                ok = false;
                            else {
                                st->chunked = false;
                                st->hasContentLength = false;
                                st->closeAfter = false;
                                st->phase = ParsePhase::StatusLine;
                            }
                        }
                        else if(noBody) {
                            bool close = st->closeAfter || st->httpMinor0;
                            return finish(close ? EpParseClose : EpParseDone);
                        }
                        else if(st->chunked)
                            st->phase = ParsePhase::BodyChunkSize;
                        else if(st->hasContentLength) {
                            if(st->bodyRemaining == 0) {
                                bool close = st->closeAfter || st->httpMinor0;
                                return finish(close ? EpParseClose : EpParseDone);
                            }

                            std::uint64_t reserveSize =
                                st->bodyRemaining < lim.maxBodyBytes ? st->bodyRemaining : lim.maxBodyBytes;
                            res.body.reserve(static_cast<std::size_t>(reserveSize));
                            st->phase = ParsePhase::BodyContentLength;
                        }
                        else
                            st->phase = ParsePhase::BodyUntilClose;
                    }
                    else
                        ok = ParseHeaderLine(line, res, *st, lim);
                }
                else if(currentPhase == ParsePhase::BodyChunkSize) {
                    std::uint64_t chunkSize = 0;
                    ok = ParseChunkSizeLine(line, chunkSize);
                    if(ok) {
                        if(chunkSize == 0)
                            st->phase = ParsePhase::BodyChunkTrailer;
                        else if(chunkSize > lim.maxBodyBytes - res.body.size()) // overflow-safe vs. huge chunkSize
                            ok = false;
                        else {
                            st->bodyRemaining = chunkSize;
                            st->phase = ParsePhase::BodyChunkData;
                        }
                    }
                }
                else if(currentPhase == ParsePhase::BodyChunkLineEnd) {
                    if(!line.empty())
                        ok = false; // malformed chunk terminator
                    else
                        st->phase = ParsePhase::BodyChunkSize;
                }
                else { // BodyChunkTrailer: discard trailer lines until the blank line
                    if(line.empty()) {
                        bool close = st->closeAfter || st->httpMinor0;
                        return finish(close ? EpParseClose : EpParseDone);
                    }

                    if(++st->headerCount > lim.maxHeaderCount)
                        ok = false;
                }

                st->lineAcc.clear();
                if(!ok) [[unlikely]]
                    return finish(EpParseError);

                continue;
            }

            case ParsePhase::BodyContentLength:
            case ParsePhase::BodyChunkData: {
                std::uint32_t avail = len - pos;
                std::uint64_t take = st->bodyRemaining < avail ? st->bodyRemaining : avail;

                if(res.body.size() + take > lim.maxBodyBytes)
                    return finish(EpParseError);

                res.body.append(buf + pos, static_cast<std::size_t>(take));
                pos += static_cast<std::uint32_t>(take);
                st->bodyRemaining -= take;

                if(st->bodyRemaining == 0) {
                    if(st->phase == ParsePhase::BodyChunkData) {
                        st->phase = ParsePhase::BodyChunkLineEnd;
                        continue;
                    }

                    bool close = st->closeAfter || st->httpMinor0;
                    return finish(close ? EpParseClose : EpParseDone);
                }

                return finish(isEof ? EpParseError : EpParseIncomplete);
            }

            case ParsePhase::BodyUntilClose: {
                std::uint32_t avail = len - pos;
                if(avail > 0) {
                    if(res.body.size() + avail > lim.maxBodyBytes)
                        return finish(EpParseError);

                    res.body.append(buf + pos, avail);
                    pos += avail;
                }

                return finish(isEof ? EpParseClose : EpParseIncomplete);
            }
        }
    }
}

inline HttpEndpointOptions BuildEndpointOptions(const char* hostPort, const HttpEndpointConfig& cfg)
{
    std::string_view hp{hostPort};
    auto colon = hp.rfind(':');
    std::string_view hostOnly = colon == std::string_view::npos ? hp : hp.substr(0, colon);

    std::uint16_t port = 0;
    if(colon != std::string_view::npos) {
        auto portSv = hp.substr(colon + 1);
        std::from_chars(portSv.data(), portSv.data() + portSv.size(), port);
    }

    bool isSecure;
    if(cfg.tlsConfig == EpTlsRequire)
        isSecure = true;
    else if(cfg.tlsConfig == EpTlsInsecure)
        isSecure = false;
    else
        isSecure = (port == 443); // EpTlsAuto

    // Omit the port from the Host header only when it's the scheme's default
    bool defaultPort = isSecure ? (port == 443) : (port == 80);

    HttpEndpointOptions opts{};
    // Full "host:port" when the port is non-default, host-only when it's the scheme default (80/443)
    opts.hostHeaderValue = defaultPort ? hostOnly : hp;
    opts.limits = HttpEndpointLimits{cfg.maxHeaderBytes, cfg.maxBodyBytes, cfg.maxHeaderCount};
    return opts;
}

inline EndpointDesc BuildDesc(HttpEndpointOptions* opts, Shared::EndpointCoalesceKeyFn coalesceKey) noexcept
{
    EndpointDesc d{};
    d.serialize = &Serialize;
    d.parse = &Parse;
    d.onConnect = nullptr;
    d.onDisconnect = nullptr;
    d.createSlotState = &CreateSlotState;
    d.destroySlotState = &DestroySlotState;
    d.createParseState = &CreateParseState;
    d.destroyParseState = &DestroyParseState;
    d.resetParseState = &ResetParseStateCb;
    d.createOutput = &CreateOutput;
    d.destroyOutput = &DestroyOutput;
    d.coalesceKey = coalesceKey;
    d.cloneOutput = coalesceKey ? &CloneOutput : nullptr;
    d.hasCapacity = nullptr; // HTTP/1.1 only: no multiplexing
    d.takeStreamOutput = nullptr;
    d.statusCode = [](const void* out) -> std::uint16_t {
        return static_cast<const HttpEndpointResponse*>(out)->status;
    };
    d.userCtx = opts;
    return d;
}

inline EndpointConfig BuildEndpointConfig(const HttpEndpointConfig& cfg) noexcept
{
    return EndpointConfig{
        .connLimit = cfg.connLimit,
        .dnsRefreshSeconds = 0,
        .connectTimeoutSeconds = cfg.connectTimeoutSeconds,
        .requestTimeoutSeconds = cfg.requestTimeoutSeconds,
        .idleTimeoutSeconds = cfg.idleTimeoutSeconds,
        .maxReconnectAttempts = cfg.maxReconnectAttempts,
        .reconnectBackoffBase = cfg.reconnectBackoffBaseSeconds,
        .reconnectBackoffMax = cfg.reconnectBackoffMaxSeconds,
        .tlsConfig = cfg.tlsConfig,
        .prewarm = cfg.prewarm,
        .maxConcurrentStreams = 0,
        .alpnProtocols = {},
    };
}

} // namespace Http::Detail

// -----------------------------------------------------------------------
// The client. One instance per upstream host, declared at namespace scope-
// -before Run():
//
//   inline const auto Api = WFX::HttpEndpoint{"api.example.com:443"};
//
// co_await any call for std::pair<EndpointStatus, EndpointOutput<HttpEndpointResponse>>.
// -----------------------------------------------------------------------
class HttpEndpoint {
public:
    explicit HttpEndpoint(const char* hostPort, HttpEndpointConfig config = {})
        : options_(Http::Detail::BuildEndpointOptions(hostPort, config)),
          ep_(hostPort, Http::Detail::BuildDesc(&options_, config.coalesceKey),
              Http::Detail::BuildEndpointConfig(config))
    {}

    // Never copied or moved: the engine holds the address of options_
    HttpEndpoint(const HttpEndpoint&) = delete;
    HttpEndpoint& operator=(const HttpEndpoint&) = delete;
    HttpEndpoint(HttpEndpoint&&) = delete;
    HttpEndpoint& operator=(HttpEndpoint&&) = delete;

public: // Main Functions
    auto Send(HttpEndpointRequest req) const noexcept
    {
        return ep_.SendPayload(std::move(req));
    }

    auto Get(std::string_view path, HttpEndpointRequestHeaders headers = {}) const noexcept
    {
        return Send(HttpEndpointRequest{HttpMethod::GET, path, {}, std::move(headers)});
    }
    auto Head(std::string_view path, HttpEndpointRequestHeaders headers = {}) const noexcept
    {
        return Send(HttpEndpointRequest{HttpMethod::HEAD, path, {}, std::move(headers)});
    }
    auto Options(std::string_view path, HttpEndpointRequestHeaders headers = {}) const noexcept
    {
        return Send(HttpEndpointRequest{HttpMethod::OPTIONS, path, {}, std::move(headers)});
    }
    auto Delete(std::string_view path, HttpEndpointRequestHeaders headers = {}) const noexcept
    {
        return Send(HttpEndpointRequest{HttpMethod::DELETE, path, {}, std::move(headers)});
    }
    auto Post(std::string_view path, std::string_view body, HttpEndpointRequestHeaders headers = {}) const noexcept
    {
        return Send(HttpEndpointRequest{HttpMethod::POST, path, body, std::move(headers)});
    }
    auto Put(std::string_view path, std::string_view body, HttpEndpointRequestHeaders headers = {}) const noexcept
    {
        return Send(HttpEndpointRequest{HttpMethod::PUT, path, body, std::move(headers)});
    }
    auto Patch(std::string_view path, std::string_view body, HttpEndpointRequestHeaders headers = {}) const noexcept
    {
        return Send(HttpEndpointRequest{HttpMethod::PATCH, path, body, std::move(headers)});
    }

private: // Storage
    Http::Detail::HttpEndpointOptions options_;
    Endpoint<HttpEndpointRequest, HttpEndpointResponse> ep_;
};

} // namespace WFX

#endif // WFX_INC_WFX_ENDPOINT_HTTP_HPP
