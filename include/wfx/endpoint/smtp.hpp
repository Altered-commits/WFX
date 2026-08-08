// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_ENDPOINT_SMTP_HPP
#define WFX_INC_WFX_ENDPOINT_SMTP_HPP

// -----------------------------------------------------------------------
// wfx/endpoint/smtp.hpp
// Outbound SMTP client (submission), built on wfx/endpoint/base.hpp's raw Endpoint<>.
//
// Provides:
//   WFX::SmtpEndpoint       : the client. One instance per upstream relay. Also where SendMail
//                             (the one-call wrapper below) lives, as a member function.
//   WFX::SmtpTransaction    : one MAIL FROM / RCPT TO / DATA exchange, pinned to one connection
//   WFX::SmtpEndpointConfig : connection pool + credentials + protocol hardening knobs
//   WFX::SmtpResponse       : status code / text from one transaction command
//   WFX::SmtpSendOutcome    : result of SmtpEndpoint::SendMail
//
// STARTTLS on port 587 only (RFC 3207) (implicit TLS (465) is out
// of scope). Speaks 7-bit ASCII command/response lines (RFC 5321)
// plus AUTH PLAIN / AUTH LOGIN (RFC 4954), whichever the server
// advertises; CRAM-MD5 and other legacy mechanisms are deliberately
// not implemented, they exist only to protect credentials on an
// unencrypted line and this client never sends credentials on one.
//
// Security posture (every one of these is load-bearing, not incidental):
//   - AUTH is refused outright if the server never advertised
//     STARTTLS. No silent plaintext fallback exists in this
//     client, ever.
//   - The pre-TLS EHLO capability list is discarded and re-fetched
//     after the TLS upgrade completes. Trusting the pre-TLS list
//     would let a MITM strip STARTTLS/AUTH capabilities from the
//     wire and force a downgrade (the CVE-2011-0411 /
//     STARTTLS-stripping class).
//   - Anything buffered before the TLS wrap is discarded by the
//     engine's UpgradeToTLS itself (see
//     EpollConnectionHandler::SlotUpgradeTls), so a
//     response-injection attack (bytes appended after the server's
//     "220 Go ahead" before the handshake completes) can't be
//     replayed as though it arrived over the authenticated channel.
//   - Every user-suppliable field (envelope addresses, display
//     names, subject, Reply-To) is scanned for CR/LF/NUL before it
//     reaches the wire; a hit fails the call outright rather than
//     emit a partially-escaped command. This is SMTP's analogue of
//     HTTP header/request-line injection: a Subject or address
//     containing "\r\nBcc: attacker@evil" is exactly the kind of
//     forged-header / smuggled-command attack this exists to stop.
//   - The message body is dot-stuffed (RFC 5321 4.5.2): a body
//     line starting with '.' is escaped to '..' so it can't be
//     mistaken for the DATA terminator, which would otherwise
//     silently truncate the message (a correctness bug as much as
//     a security one).
//   - Server certificate verification reuses the engine's existing
//     outbound TLS trust decision (EndpointConfig::tlsConfig), the
//     same one tls_audit's phase_verify already exhaustively tests.
//     No SMTP-specific certificate logic exists here to get wrong.
//   - Multi-line responses are bounded (maxResponseLineBytes /
//     maxResponseLines) so a hostile or compromised relay can't
//     OOM or hang a worker with an endless "250-..." flood.
//
// -----------------------------------------------------------------------
// Usage
// -----------------------------------------------------------------------
//
//   inline const auto Relay = WFX::SmtpEndpoint{"smtp.zoho.com:587", WFX::SmtpEndpointConfig{
//       .username = WFX::GetEnvString("SMTP_USERNAME"),
//       .password = WFX::GetEnvString("SMTP_PASSWORD"),
//       .heloName = "rearc.example",
//   }};
//
//   WFX_POST("/about-us", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
//       auto tx = Relay.Begin();
//       if(!tx.IsValid()) { res.Status(WFX::HttpStatus::SERVICE_UNAVAILABLE).SendText("busy"); co_return; }
//
//       auto [s1, r1] = co_await tx.MailFrom("noreply@rearc.example");
//       if(s1 != WFX::EpOk || !r1->Success()) { res.Status(WFX::HttpStatus::BAD_GATEWAY).SendText("mail failed");
//       co_return; }
//
//       auto [s2, r2] = co_await tx.RcptTo("contact@rearc.example");
//       if(s2 != WFX::EpOk || !r2->Success()) { res.Status(WFX::HttpStatus::BAD_GATEWAY).SendText("mail failed");
//       co_return; }
//
//       auto [s3, r3] = co_await tx.DataStart();
//       if(s3 != WFX::EpOk || !r3->Continue()) { res.Status(WFX::HttpStatus::BAD_GATEWAY).SendText("mail failed");
//       co_return; }
//
//       auto [s4, r4] = co_await tx.DataBody("noreply@rearc.example", "ReArc", "contact@rearc.example",
//                                            "ReArc Contact", enquirySubject, enquiryBody);
//       if(s4 != WFX::EpOk || !r4->Success()) { res.Status(WFX::HttpStatus::BAD_GATEWAY).SendText("mail failed");
//       co_return; }
//
//       res.SendText("sent");
//       co_return;
//   });
//
// Every step above is written out and awaited by hand, which is what you want
// when you need to react differently depending on which command failed, or
// send to more than one recipient on the same connection with repeated
// RcptTo calls before DataStart. For the common case of "just send this one
// email", SmtpEndpoint::SendMail runs the whole exchange for you:
//
//   WFX::SmtpSendOutcome outcome;
//   co_await Relay.SendMail("noreply@rearc.example", "ReArc", "contact@rearc.example",
//                            "ReArc Contact", enquirySubject, enquiryBody, outcome);
//   if(!outcome.Success()) { res.Status(WFX::HttpStatus::BAD_GATEWAY).SendText("mail failed");
//   co_return; }
//
// This works because WFX::Coro (Async::Task<void>) can now be co_awaited from
// another coroutine, so SendMail can be a coroutine in its own right that a
// route handler just awaits like any single step. See include/async/task.hpp.
//
// -----------------------------------------------------------------------

#include "base.hpp"
#include "helper.hpp"
#include "wfx/memory.hpp"
#include "wfx/utils/encoding.hpp"
#include <cstring>
#include <ctime>
#include <random>
#include <string_view>

namespace WFX {

// -----------------------------------------------------------------------
// SMTP status codes this client checks for (RFC 5321 4.2.3), named
// instead of left as bare literals at every comparison site.
// -----------------------------------------------------------------------
inline constexpr std::uint16_t SMTP_READY = 220;            // greeting, and "220" to STARTTLS
inline constexpr std::uint16_t SMTP_OK = 250;               // EHLO / MAIL FROM / RCPT TO / QUIT success
inline constexpr std::uint16_t SMTP_START_MAIL_INPUT = 354; // DATA's go-ahead to send the message
inline constexpr std::uint16_t SMTP_AUTH_CONTINUE = 334;    // AUTH LOGIN's Username:/Password: prompts
inline constexpr std::uint16_t SMTP_AUTH_SUCCESS = 235;     // AUTH completed

// -----------------------------------------------------------------------
// One transaction command. Built by SmtpTransaction's typed methods
// below, never construct this directly from unescaped input, the
// injection screen lives in Serialize(), keyed off these raw fields,
// exactly once, in one auditable place (mirrors
// HttpEndpointRequest/HasInjectionBytes).
// -----------------------------------------------------------------------
enum class SmtpCmdKind : std::uint8_t { MAIL_FROM, RCPT_TO, DATA_START, DATA_BODY, RSET, QUIT };

struct SmtpCmd {
    SmtpCmdKind kind = SmtpCmdKind::QUIT;
    std::string_view addr;     // MAIL_FROM / RCPT_TO: envelope address
    std::string_view fromAddr; // DATA_BODY only, from here down
    std::string_view fromName;
    std::string_view toAddr;
    std::string_view toName;
    std::string_view subject;
    std::string_view body;
    std::string_view replyTo;
};

// Response to one transaction command. text is the final response line only (multi-line EHLO
// style capability scanning is a separate, onConnect-only concern, see Detail::LineResponse)
struct SmtpResponse {
    std::uint16_t code = 0;
    WFX::String text;

    bool Success() const noexcept
    {
        return code >= 200 && code < 300;
    }
    bool Continue() const noexcept
    {
        return code == SMTP_START_MAIL_INPUT;
    }
};

// -----------------------------------------------------------------------
// Connection pool + credentials + protocol hardening knobs.
// -----------------------------------------------------------------------
struct SmtpEndpointConfig {
    std::uint32_t connLimit = 4;
    std::uint16_t connectTimeoutSeconds = 20; // TCP + STARTTLS + AUTH handshake budget
    std::uint16_t requestTimeoutSeconds = 20; // each MAIL/RCPT/DATA command budget
    std::uint32_t idleTimeoutSeconds = 60;
    std::uint16_t maxReconnectAttempts = 3;
    std::uint16_t reconnectBackoffBaseSeconds = 2;
    std::uint16_t reconnectBackoffMaxSeconds = 30;
    std::uint32_t prewarm = 0;

    std::uint32_t maxResponseLineBytes = 2048; // single response line cap
    std::uint16_t maxResponseLines = 64;       // multi-line response cap, DoS defense

    std::string_view username; // e.g. WFX::GetEnvString("SMTP_USERNAME") (never logged)
    std::string_view password; // e.g. WFX::GetEnvString("SMTP_PASSWORD") (never logged)
    std::string_view heloName; // EHLO identity, e.g. "mail.example.com"
};

// -----------------------------------------------------------------------
// Implementation detail: EndpointDesc callbacks + SMTP wire codec.
// -----------------------------------------------------------------------
namespace Smtp::Detail {

using namespace WFX::EndpointDetail;

// Shared by every connection to one SmtpEndpoint; lives as long as the owning SmtpEndpoint
struct SmtpOptions {
    std::string_view heloName;
    std::string_view username;
    std::string_view password;
    std::uint32_t maxResponseLineBytes;
    std::uint16_t maxResponseLines;
};

struct SlotState {
    const SmtpOptions* options = nullptr;
};

inline void* CreateSlotState(void* userCtx) noexcept
{
    auto* s = New<SlotState>();
    if(s)
        s->options = static_cast<const SmtpOptions*>(userCtx);

    return s;
}

inline void DestroySlotState(void* state) noexcept
{
    Delete(static_cast<SlotState*>(state));
}

// Writes "Name <addr>\r\n"-shaped header value, or just "<addr>" when no display name was given
inline bool AppendAddrHeader(BufWriter& w, std::string_view name, std::string_view addr) noexcept
{
    if(!name.empty()) {
        if(!(w.Append(name) && w.Append(" <") && w.Append(addr) && w.Append('>')))
            return false;
    }
    else if(!w.Append(addr))
        return false;

    return w.Append("\r\n");
}

// RFC 5321 4.5.2 dot-stuffing: any body line starting with '.' gets a second '.' prepended, so it
// can never be mistaken for the "\r\n.\r\n" terminator this function's caller appends afterward.
// Normalizes bare '\n' to '\r\n' along the way, since caller-supplied bodies aren't guaranteed to
// already use wire line endings
inline bool AppendDotStuffed(BufWriter& w, std::string_view body) noexcept
{
    if(body.empty())
        return true;

    std::size_t lineStart = 0;
    while(lineStart <= body.size()) {
        auto nl = body.find('\n', lineStart);
        std::string_view line =
            (nl == std::string_view::npos) ? body.substr(lineStart) : body.substr(lineStart, nl - lineStart);

        if(!line.empty() && line.back() == '\r')
            line.remove_suffix(1);

        if(!line.empty() && line.front() == '.') {
            if(!w.Append('.'))
                return false;
        }
        if(!(w.Append(line) && w.Append("\r\n")))
            return false;

        if(nl == std::string_view::npos)
            break;

        lineStart = nl + 1;
    }

    return true;
}

// Date + Message-ID + MIME headers. None of this is caller-influenced (Date comes from the system
// clock, Message-ID from a random source, and the Message-ID domain part from operator-configured
// heloName rather than any per-message field), so none of it needs the injection screen that
// Serialize() runs over the caller-supplied fields. heloName itself is still screened here too,
// same reasoning as SmtpOnConnect's check: cheap insurance against the bpo-30585 CVE class
// regardless of who ends up wiring this value in
inline bool AppendGeneratedHeaders(BufWriter& w, std::string_view heloName) noexcept
{
    if(HasInjectionBytes(heloName)) [[unlikely]]
        return false;

    // Date: always rendered in GMT so this never depends on the host's local timezone setting.
    // std::strftime's signature requires a char* output buffer; there's no String-native way to
    // call it. The format is fixed-width ("Wed, 02 Aug 2026 14:23:01 +0000", 31 chars), so a
    // small stack buffer sized for exactly that is the correct tool here, unlike a buffer sized
    // for arbitrary caller-controlled input
    {
        const std::time_t now = std::time(nullptr);
        std::tm tmVal{};
        gmtime_r(&now, &tmVal);

        char dateBuf[40];
        const std::size_t n = std::strftime(dateBuf, sizeof(dateBuf), "%a, %d %b %Y %H:%M:%S +0000", &tmVal);
        if(n == 0)
            return false;

        if(!(w.Append("Date: ") && w.Append(std::string_view(dateBuf, n)) && w.Append("\r\n")))
            return false;
    }

    // Message-ID: RFC 5322 doesn't mandate one, but every real receiving MTA and spam filter
    // expects one. Not a security-sensitive value (it's an opaque tracking id, not a credential
    // or a capability). It only has to avoid colliding with another message's id, not resist
    // prediction, so a fast non-cryptographic PRNG is the right tool. std::random_device seeds
    // it exactly once per worker (a real syscall, done once); every message after that just
    // steps the already-seeded generator in memory, no syscall at all. Calling random_device
    // itself per email would mean 16+ syscalls per send, real overhead under concurrent load
    {
        thread_local std::mt19937_64 GlobalRng{[] {
            std::random_device rd;
            return (static_cast<std::uint64_t>(rd()) << 32) | rd();
        }()};

        unsigned char idBytes[16];
        std::uint64_t r0 = GlobalRng();
        std::uint64_t r1 = GlobalRng();
        std::memcpy(idBytes, &r0, 8);
        std::memcpy(idBytes + 8, &r1, 8);

        const WFX::String idHex = HexEncode(std::string_view(reinterpret_cast<const char*>(idBytes), sizeof(idBytes)));
        if(!(w.Append("Message-ID: <") && w.Append(idHex) && w.Append('@') && w.Append(heloName) && w.Append(">\r\n")))
            return false;
    }

    return w.Append("MIME-Version: 1.0\r\n") && w.Append("Content-Type: text/plain; charset=UTF-8\r\n");
}

inline Shared::SerializeResult Serialize(void* slotStateVoid, const void* reqVoid, char* buf, std::uint32_t bufLen,
                                         std::uint32_t* written, std::uint64_t* /*streamKey*/) noexcept
{
    const auto& req = *static_cast<const SmtpCmd*>(reqVoid);

    BufWriter w{buf, bufLen};
    bool ok = true;

    switch(req.kind) {
        case SmtpCmdKind::MAIL_FROM:
            if(HasInjectionBytes(req.addr)) [[unlikely]]
                return Shared::SerializeResult::ERROR;

            ok = w.Append("MAIL FROM:<") && w.Append(req.addr) && w.Append(">\r\n");
            break;

        case SmtpCmdKind::RCPT_TO:
            if(HasInjectionBytes(req.addr)) [[unlikely]]
                return Shared::SerializeResult::ERROR;

            ok = w.Append("RCPT TO:<") && w.Append(req.addr) && w.Append(">\r\n");
            break;

        case SmtpCmdKind::DATA_START:
            ok = w.Append("DATA\r\n");
            break;

        case SmtpCmdKind::DATA_BODY:
            if(HasInjectionBytes(req.fromAddr) || HasInjectionBytes(req.fromName) || HasInjectionBytes(req.toAddr) ||
               HasInjectionBytes(req.toName) || HasInjectionBytes(req.subject) || HasInjectionBytes(req.replyTo))
                [[unlikely]]
                return Shared::SerializeResult::ERROR;

            // SMTP is not 8-bit/NUL-clean and there is no legitimate reason a text body needs a
            // NUL byte; CR/LF in the body is fine, that's just line structure, handled by dot-stuffing
            if(req.body.find('\0') != std::string_view::npos) [[unlikely]]
                return Shared::SerializeResult::ERROR;

            ok = w.Append("From: ") && AppendAddrHeader(w, req.fromName, req.fromAddr);
            ok = ok && w.Append("To: ") && AppendAddrHeader(w, req.toName, req.toAddr);

            if(ok && !req.replyTo.empty())
                ok = w.Append("Reply-To: ") && w.Append(req.replyTo) && w.Append("\r\n");

            ok = ok && w.Append("Subject: ") && w.Append(req.subject) && w.Append("\r\n");
            ok = ok && AppendGeneratedHeaders(w, static_cast<SlotState*>(slotStateVoid)->options->heloName);
            ok = ok && w.Append("\r\n"); // end of headers
            ok = ok && AppendDotStuffed(w, req.body);
            ok = ok && w.Append(".\r\n"); // DATA terminator (the blank line before it is the last body line's own \r\n)
            break;

        case SmtpCmdKind::RSET:
            ok = w.Append("RSET\r\n");
            break;

        case SmtpCmdKind::QUIT:
            ok = w.Append("QUIT\r\n");
            break;
    }

    if(!ok) [[unlikely]]
        return Shared::SerializeResult::BUFFER_TOO_SMALL;

    *written = w.Pos();
    return Shared::SerializeResult::OK;
}

// Incremental parser for one (possibly multi-line) "CODE-text\r\n"..."CODE text\r\n" response,
// used for the transaction phase (MAIL FROM / RCPT TO / DATA / body). onConnect's handshake reads
// responses through a separate, non-incremental path (LineResponse below) since it talks to the
// raw SlotHandle directly rather than through this Parse()/Serialize() pair
struct ParseState {
    WFX::String lineAcc;
    std::uint16_t code = 0;
    std::uint16_t lineCount = 0;
};

inline void ResetParseStateFields(ParseState& s) noexcept
{
    s.lineAcc.clear();
    s.code = 0;
    s.lineCount = 0;
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
    return New<SmtpResponse>();
}
inline void DestroyOutput(void* p) noexcept
{
    Delete(static_cast<SmtpResponse*>(p));
}

// "CODE-text" or "CODE text", returns false on malformed input. finalLine is true for the ' '
// separator (RFC 5321 4.2.1): that's what actually ends a multi-line response
inline bool ParseResponseLine(std::string_view line, std::uint16_t& code, bool& finalLine,
                              std::string_view& text) noexcept
{
    if(line.size() < 3)
        return false;

    for(int i = 0; i < 3; ++i)
        if(line[i] < '0' || line[i] > '9')
            return false;

    const std::uint16_t parsed =
        static_cast<std::uint16_t>((line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0'));

    if(line.size() == 3) {
        // Bare 3-digit line with nothing after it: some servers do this for the final line
        code = parsed;
        finalLine = true;
        text = {};
        return true;
    }

    const char sep = line[3];
    if(sep != '-' && sep != ' ')
        return false;

    code = parsed;
    finalLine = (sep == ' ');
    text = line.substr(4);
    return true;
}

inline Shared::ParseResult Parse(void* slotStateVoid, void* parseStateVoid, const char* buf, std::uint32_t len,
                                 std::uint32_t* consumed, void* outObjVoid, bool isEof,
                                 std::uint64_t* /*completedKey*/) noexcept
{
    auto* state = static_cast<SlotState*>(slotStateVoid);
    auto* st = static_cast<ParseState*>(parseStateVoid);
    auto& res = *static_cast<SmtpResponse*>(outObjVoid);
    const auto& lim = *state->options;

    std::uint32_t pos = 0;
    auto finish = [&](Shared::ParseResult r) noexcept {
        *consumed = pos;
        return r;
    };

    while(true) {
        std::string_view line;
        auto status = ReadLine(buf, len, pos, st->lineAcc, lim.maxResponseLineBytes, line);
        if(status == LineReadStatus::TOO_LONG)
            return finish(Shared::ParseResult::ERROR);
        if(status == LineReadStatus::NEED_MORE)
            return finish(isEof ? Shared::ParseResult::ERROR : Shared::ParseResult::INCOMPLETE);

        st->lineAcc.clear();

        std::uint16_t lineCode = 0;
        bool finalLine = false;
        std::string_view text;

        if(!ParseResponseLine(line, lineCode, finalLine, text)) [[unlikely]]
            return finish(Shared::ParseResult::ERROR);

        if(++st->lineCount > lim.maxResponseLines) [[unlikely]]
            return finish(Shared::ParseResult::ERROR);

        // Every continuation line must carry the same code as the first: a server (or a MITM)
        // splicing two different responses together is a protocol violation, not tolerated
        if(st->code != 0 && lineCode != st->code) [[unlikely]]
            return finish(Shared::ParseResult::ERROR);

        st->code = lineCode;

        if(finalLine) {
            res.code = st->code;
            res.text = WFX::String(text);
            return finish(Shared::ParseResult::COMPLETE_KEEP_ALIVE);
        }
    }
}

inline SmtpOptions BuildOptions(const SmtpEndpointConfig& cfg) noexcept
{
    return SmtpOptions{cfg.heloName, cfg.username, cfg.password, cfg.maxResponseLineBytes, cfg.maxResponseLines};
}

inline EndpointDesc BuildDesc(SmtpOptions* opts) noexcept
{
    EndpointDesc d{};
    d.serialize = &Serialize;
    d.parse = &Parse;
    d.createSlotState = &CreateSlotState;
    d.destroySlotState = &DestroySlotState;
    d.createParseState = &CreateParseState;
    d.destroyParseState = &DestroyParseState;
    d.resetParseState = &ResetParseStateCb;
    d.createOutput = &CreateOutput;
    d.destroyOutput = &DestroyOutput;
    d.statusCode = [](const void* out) -> std::uint16_t { return static_cast<const SmtpResponse*>(out)->code; };
    d.userCtx = opts;
    return d;
}

// -----------------------------------------------------------------------
// onConnect: EHLO -> STARTTLS -> re-EHLO -> AUTH. Talks to the raw
// SlotHandle directly (Send/Receive), not through Serialize()/Parse()
// above. Those exist for the repeatable transaction phase, this runs
// exactly once per connection before it ever enters the pool
// -----------------------------------------------------------------------

// Accumulates one multi-line response from raw Receive() calls. Pure/non-async on purpose: the
// actual co_await loop has to live inline in the onConnect coroutine (see the big comment at the
// top of this file. Promise<T> is a closed set, so a reusable "co_await in a loop, return a
// value" helper coroutine has nowhere to live), this is the part that CAN be factored out
struct LineResponse {
    std::uint16_t code = 0;
    bool hasStartTls = false;
    bool hasAuthPlain = false;
    bool hasAuthLogin = false;
    bool ok = false; // false on malformed input or a limit violation

public:
    explicit LineResponse(const SmtpOptions* lim) noexcept : lim_(lim)
    {}

    bool Done() const noexcept
    {
        return done_;
    }

    // Returns false on a protocol violation or a bound exceeded, caller must treat that as fatal.
    // consumed is always set to how many of 'len' bytes were used, so the caller trims exactly
    // that much before its next Receive() call
    bool Feed(const char* buf, std::uint32_t len, std::uint32_t& consumed) noexcept
    {
        auto fail = [&](std::uint32_t p) noexcept {
            consumed = p;
            return false;
        };

        std::uint32_t pos = 0;
        while(pos < len) {
            std::string_view line;
            auto status = ReadLine(buf, len, pos, lineAcc_, lim_->maxResponseLineBytes, line);
            if(status == LineReadStatus::TOO_LONG)
                return fail(pos);
            if(status == LineReadStatus::NEED_MORE) {
                consumed = len;
                return true;
            }

            lineAcc_.clear();

            std::uint16_t lineCode = 0;
            bool finalLine = false;
            std::string_view text;

            if(!ParseResponseLine(line, lineCode, finalLine, text))
                return fail(pos);

            if(++lineCount_ > lim_->maxResponseLines)
                return fail(pos);

            if(code != 0 && lineCode != code)
                return fail(pos);

            code = lineCode;

            // Capability lines only matter on an EHLO response; harmless to scan unconditionally
            // elsewhere, nothing else this client sends produces a line spelled "STARTTLS"/"AUTH"
            if(InsensitiveEqual(text, "STARTTLS"))
                hasStartTls = true;
            else if(InsensitiveStartsWith(text, "AUTH ") || InsensitiveStartsWith(text, "AUTH=")) {
                const std::string_view mechList = text.substr(5);
                std::size_t p = 0;
                while(p < mechList.size()) {
                    while(p < mechList.size() && mechList[p] == ' ')
                        ++p;

                    const std::size_t start = p;
                    while(p < mechList.size() && mechList[p] != ' ')
                        ++p;

                    const std::string_view mech = mechList.substr(start, p - start);

                    if(InsensitiveEqual(mech, "PLAIN"))
                        hasAuthPlain = true;
                    else if(InsensitiveEqual(mech, "LOGIN"))
                        hasAuthLogin = true;
                }
            }

            if(finalLine) {
                done_ = true;
                ok = true;
                consumed = pos;
                return true;
            }
        }

        consumed = pos;
        return true;
    }

private: // Storage
    std::uint16_t lineCount_ = 0;
    bool done_ = false;
    WFX::String lineAcc_;
    const SmtpOptions* lim_ = nullptr;
};

inline EpCoro SmtpOnConnect(SlotHandle h, void* slotStateVoid)
{
    const auto* opts = static_cast<const SlotState*>(slotStateVoid)->options;

    // heloName is operator config today, not per-request input, but CPython's smtplib had a real
    // CRLF-injection CVE through this exact parameter (local_hostname, bpo-30585). Screening it
    // here costs nothing and removes the assumption entirely rather than relying on callers never
    // wiring it from anything less trusted
    if(HasInjectionBytes(opts->heloName)) [[unlikely]]
        co_return EpFatal;

    // How much of the last Receive() result to trim next time, see SlotHandle::Receive
    std::uint32_t pending = 0;

    // Every response read below follows the same shape: feed Receive() results into a
    // LineResponse until it says Done(). This can't be factored into a shared helper function.
    // See the file-level comment on why a reusable "co_await in a loop, return a value" coroutine
    // has nowhere to live in this codebase's closed Promise<T> set, so it's inlined per read,
    // seven times, deliberately
#define WFX_SMTP_READ_RESPONSE(varName)                                                                                \
    LineResponse varName{opts};                                                                                        \
    while(!(varName).Done()) {                                                                                         \
        auto recv = co_await h.Receive(pending);                                                                       \
        pending = 0;                                                                                                   \
        if(recv.status != EpSlotOk)                                                                                    \
            co_return EpFatal;                                                                                         \
        if(!(varName).Feed(recv.buf, static_cast<std::uint32_t>(recv.len), pending))                                   \
            co_return EpFatal;                                                                                         \
    }

    // 1. Unsolicited greeting banner
    {
        WFX_SMTP_READ_RESPONSE(greet)
        if(greet.code != SMTP_READY)
            co_return EpFatal;
    }

    // Sent twice further down (once now, once again post-TLS since the pre-TLS capability list
    // is never trusted), built once here since heloName never changes between the two sends
    WFX::String ehloLine;
    ehloLine.reserve(5 + opts->heloName.size() + 2); // "EHLO " + heloName + "\r\n"
    ehloLine.append("EHLO ");
    ehloLine.append(opts->heloName.data(), opts->heloName.size());
    ehloLine.append("\r\n");

    // 2. EHLO (pre-TLS), only consulted for STARTTLS capability. Its AUTH line, if any, is
    // deliberately never trusted: see the re-EHLO after the TLS upgrade below
    if((co_await h.Send(ehloLine.data(), static_cast<std::uint32_t>(ehloLine.size()))) != EpSlotOk)
        co_return EpFatal;

    {
        WFX_SMTP_READ_RESPONSE(ehlo1)
        if(ehlo1.code != SMTP_OK)
            co_return EpFatal;

        // 3. STARTTLS must be advertised. Refuse outright rather than ever consider a plaintext
        // fallback. This is the entire point of the client existing: no path here ever
        // authenticates without a confirmed TLS upgrade first
        if(!ehlo1.hasStartTls)
            co_return EpFatal;
    }

    if((co_await h.Send("STARTTLS\r\n", 10)) != EpSlotOk)
        co_return EpFatal;

    {
        WFX_SMTP_READ_RESPONSE(startTlsResp)
        if(startTlsResp.code != SMTP_READY)
            co_return EpFatal;
    }

    // Anything buffered at this point is discarded by the engine before the TLS wrap (see
    // EpollConnectionHandler::SlotUpgradeTls's ClearReadBuffer() call). A response-injection
    // attempt (extra plaintext appended right after the "220") can't survive to be read as though
    // it arrived over the now-authenticated channel
    if((co_await h.UpgradeToTLS()) != EpSlotOk)
        co_return EpFatal;

    // ClearReadBuffer() already wiped the buffer above, so any pending trim from startTlsResp
    // (e.g. an injection attempt's extra bytes) is stale now, not real leftover data to trim
    pending = 0;

    // 4. Re-EHLO over the encrypted channel, reusing the exact same line built above. The pre-TLS
    // capability list is never reused past this point: a MITM that stripped STARTTLS/AUTH from it
    // already failed at step 3, and one that let STARTTLS through but lied about AUTH mechanisms
    // gets caught here instead
    if((co_await h.Send(ehloLine.data(), static_cast<std::uint32_t>(ehloLine.size()))) != EpSlotOk)
        co_return EpFatal;

    // 5. AUTH: PLAIN preferred (single round trip), LOGIN as fallback for relays that don't offer
    // PLAIN. Picked from what the server just advertised POST-TLS, never assumed
    bool authPlain, authLogin;
    {
        WFX_SMTP_READ_RESPONSE(ehlo2)
        if(ehlo2.code != SMTP_OK)
            co_return EpFatal;

        authPlain = ehlo2.hasAuthPlain;
        authLogin = ehlo2.hasAuthLogin;
    }

    if(authPlain) {
        // "AUTH PLAIN <base64(\0username\0password)>" (RFC 4954 initial-response form)
        WFX::String blob;
        blob.push_back('\0');
        blob.append(opts->username.data(), opts->username.size());
        blob.push_back('\0');
        blob.append(opts->password.data(), opts->password.size());

        WFX::String encoded = Base64Encode(std::string_view(blob.data(), blob.size()));

        WFX::String line;
        line.reserve(11 + encoded.size() + 2); // "AUTH PLAIN " + encoded + "\r\n"
        line.append("AUTH PLAIN ");
        line.append(encoded);
        line.append("\r\n");

        if((co_await h.Send(line.data(), static_cast<std::uint32_t>(line.size()))) != EpSlotOk)
            co_return EpFatal;
    }
    else if(authLogin) {
        if((co_await h.Send("AUTH LOGIN\r\n", 12)) != EpSlotOk)
            co_return EpFatal;

        {
            WFX_SMTP_READ_RESPONSE(loginPrompt)
            if(loginPrompt.code != SMTP_AUTH_CONTINUE)
                co_return EpFatal;
        }

        WFX::String userLine = Base64Encode(opts->username) + "\r\n";
        if((co_await h.Send(userLine.data(), static_cast<std::uint32_t>(userLine.size()))) != EpSlotOk)
            co_return EpFatal;

        {
            WFX_SMTP_READ_RESPONSE(passPrompt)
            if(passPrompt.code != SMTP_AUTH_CONTINUE)
                co_return EpFatal;
        }

        WFX::String passLine = Base64Encode(opts->password) + "\r\n";
        if((co_await h.Send(passLine.data(), static_cast<std::uint32_t>(passLine.size()))) != EpSlotOk)
            co_return EpFatal;
    }
    // Neither mechanism this client implements was offered. Refuse rather than guess
    // CRAM-MD5 and friends are deliberately unimplemented, see the file-level comment
    else
        co_return EpFatal;

    {
        WFX_SMTP_READ_RESPONSE(authResp)
        // Wrong credentials or a refusal both land here; no retry-with-the-same-credentials loop.
        // Reconnects go through the engine's own backoff (maxReconnectAttempts), not a tight
        // local one
        if(authResp.code != SMTP_AUTH_SUCCESS)
            co_return EpFatal;
    }

#undef WFX_SMTP_READ_RESPONSE

    co_return EpReady;
}

} // namespace Smtp::Detail

// -----------------------------------------------------------------------
// One MAIL FROM / RCPT TO / DATA exchange, pinned to a single
// reserved connection. See Async::Resolve::Reserve()'s doc comment
// ("SQL transactions, LISTEN/NOTIFY, any sticky session"). Always
// check IsValid() before use: the pool can be exhausted.
// -----------------------------------------------------------------------
class SmtpTransaction {
public:
    explicit SmtpTransaction(ReservedSlot<SmtpCmd, SmtpResponse> slot) noexcept : slot_(std::move(slot))
    {}

public:
    bool IsValid() const noexcept
    {
        return slot_.IsValid();
    }

    auto MailFrom(std::string_view addr) const noexcept
    {
        return slot_.SendPayload(SmtpCmd{SmtpCmdKind::MAIL_FROM, addr});
    }
    auto RcptTo(std::string_view addr) const noexcept
    {
        return slot_.SendPayload(SmtpCmd{SmtpCmdKind::RCPT_TO, addr});
    }
    auto DataStart() const noexcept
    {
        return slot_.SendPayload(SmtpCmd{SmtpCmdKind::DATA_START});
    }
    // Builds the From/To/Reply-To/Subject headers and a dot-stuffed body internally. Every field
    // is CR/LF/NUL-screened before it reaches the wire (see Serialize()'s DATA_BODY case)
    auto DataBody(std::string_view fromAddr, std::string_view fromName, std::string_view toAddr,
                  std::string_view toName, std::string_view subject, std::string_view body,
                  std::string_view replyTo = {}) const noexcept
    {
        SmtpCmd cmd{};
        cmd.kind = SmtpCmdKind::DATA_BODY;
        cmd.fromAddr = fromAddr;
        cmd.fromName = fromName;
        cmd.toAddr = toAddr;
        cmd.toName = toName;
        cmd.subject = subject;
        cmd.body = body;
        cmd.replyTo = replyTo;
        return slot_.SendPayload(cmd);
    }
    auto Reset() const noexcept
    {
        return slot_.SendPayload(SmtpCmd{SmtpCmdKind::RSET});
    }
    auto Quit() const noexcept
    {
        return slot_.SendPayload(SmtpCmd{SmtpCmdKind::QUIT});
    }

private:
    ReservedSlot<SmtpCmd, SmtpResponse> slot_;
};

// Result of SmtpEndpoint::SendMail below.
//   status   EpOk once DATA actually goes through, otherwise whatever stopped it, including
//            failures that happen before any SMTP response exists at all (pool exhausted,
//            connect failure, timeout)
//   response whichever step's response came back last, empty if 'status' never got that far
struct SmtpSendOutcome {
    Shared::EndpointStatus status = EpInternalError;
    EndpointOutput<SmtpResponse> response;

    bool Success() const noexcept
    {
        return status == EpOk && response && response->Success();
    }
};

// -----------------------------------------------------------------------
// The client. One instance per upstream relay, declared at namespace scope before Run():
//
//   inline const auto Relay = WFX::SmtpEndpoint{"smtp.example.com:587", WFX::SmtpEndpointConfig{...}};
// -----------------------------------------------------------------------
class SmtpEndpoint {
public:
    explicit SmtpEndpoint(const char* hostPort, SmtpEndpointConfig config)
        : options_(Smtp::Detail::BuildOptions(config)),
          ep_(hostPort, Smtp::Detail::BuildDesc(&options_),
              EndpointConfig{
                  .connLimit = config.connLimit,
                  .dnsRefreshSeconds = 0,
                  .connectTimeoutSeconds = config.connectTimeoutSeconds,
                  .requestTimeoutSeconds = config.requestTimeoutSeconds,
                  .idleTimeoutSeconds = config.idleTimeoutSeconds,
                  .maxReconnectAttempts = config.maxReconnectAttempts,
                  .reconnectBackoffBase = config.reconnectBackoffBaseSeconds,
                  .reconnectBackoffMax = config.reconnectBackoffMaxSeconds,
                  .tlsConfig = EpTlsInsecure, // STARTTLS is negotiated in-band by onConnect
                  .prewarm = config.prewarm,
                  .maxConcurrentStreams = 0,
                  .alpnProtocols = {},
              })
    {}

    // Never copied or moved: the engine holds the address of options_
    SmtpEndpoint(const SmtpEndpoint&) = delete;
    SmtpEndpoint& operator=(const SmtpEndpoint&) = delete;
    SmtpEndpoint(SmtpEndpoint&&) = delete;
    SmtpEndpoint& operator=(SmtpEndpoint&&) = delete;

public:
    // Pins one connection for a whole MAIL FROM/RCPT TO/DATA exchange. Check IsValid() before use
    SmtpTransaction Begin() const noexcept
    {
        return SmtpTransaction{ep_.Reserve()};
    }

    // One-call convenience wrapper around a full MAIL FROM -> RCPT TO -> DATA exchange, instead
    // of writing the sequence out by hand like SmtpTransaction's own doc comment shows.
    //
    //   WFX::SmtpSendOutcome outcome;
    //   co_await Relay.SendMail("noreply@example.com", "Example", "contact@example.com",
    //                            "Contact Form", "New enquiry", "message body", outcome);
    //   if(!outcome.Success()) { /* outcome.status says why, outcome.response the last reply */ }
    WFX::Coro SendMail(std::string_view fromAddr, std::string_view fromName, std::string_view toAddr,
                       std::string_view toName, std::string_view subject, std::string_view body, SmtpSendOutcome& out,
                       std::string_view replyTo = {}) const
    {
        auto tx = Begin();
        if(!tx.IsValid()) {
            out.status = EpPoolExhausted;
            co_return;
        }

        // Moves whatever came back into 'out' and reports whether step actually succeeded,
        // so the four steps below don't repeat this every time
        auto record = [&out](Shared::EndpointStatus s, EndpointOutput<SmtpResponse>& r, bool responseOk) {
            out.status = s;
            out.response = std::move(r);
            return s == EpOk && responseOk;
        };

        auto [s1, r1] = co_await tx.MailFrom(fromAddr);
        if(!record(s1, r1, r1 && r1->Success()))
            co_return;

        auto [s2, r2] = co_await tx.RcptTo(toAddr);
        if(!record(s2, r2, r2 && r2->Success()))
            co_return;

        auto [s3, r3] = co_await tx.DataStart();
        if(!record(s3, r3, r3 && r3->Continue()))
            co_return;

        auto [s4, r4] = co_await tx.DataBody(fromAddr, fromName, toAddr, toName, subject, body, replyTo);
        record(s4, r4, r4 && r4->Success());
    }

private:
    Smtp::Detail::SmtpOptions options_;
    Endpoint<SmtpCmd, SmtpResponse, &Smtp::Detail::SmtpOnConnect> ep_;
};

} // namespace WFX

#endif // WFX_INC_WFX_ENDPOINT_SMTP_HPP
