// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_ENDPOINT_POSTGRES_CONNECTION_HPP
#define WFX_INC_WFX_ENDPOINT_POSTGRES_CONNECTION_HPP

// -----------------------------------------------------------------------
// Connection setup: the handshake that runs before a slot joins the pool.
//
//   SSLRequest        one byte back, S to upgrade or N to stay plaintext
//   StartupMessage    protocol version and the session parameters
//   authentication    SCRAM-SHA-256 or cleartext, until AuthenticationOk
//   ReadyForQuery     the slot is usable from here
//
// Everything shared between connections lives in PgOptions as string_views.
// A PostgresEndpoint is declared at namespace scope, so PgOptions is built
// during the user .so's static initialization, before the worker initializes
// the buffer pool. A WFX::String there would allocate from an uninitialized
// pool, so nothing in it may own memory. The handshake itself runs at connect
// time and allocates freely.
//
// The endpoint must be configured PLAINTEXT even when encryption is REQUIRED.
// Postgres negotiates encryption in-band, and the engine refuses to wrap an
// already-secure slot, so auto-wrapping at connect time makes UpgradeToTLS
// fail with EpSlotInvalidState.
// -----------------------------------------------------------------------

#include "auth.hpp"
#include "protocol.hpp"
#include "stmt_cache.hpp"
#include "wfx/endpoint/base.hpp"
#include "wfx/memory.hpp"

#include <charconv>
#include <cstdint>
#include <string_view>

namespace WFX::Postgres::Detail {

// Controls whether the connection is encrypted. Certificate verification is
// handled by the engine's outbound TLS settings, so there is no verify option
// here.
enum class PgEncryption : std::uint8_t {
    NONE,     // never ask
    OPTIONAL, // ask, continue in plaintext if refused
    REQUIRED, // ask, fail if refused
};

enum class PgIsolation : std::uint8_t {
    READ_COMMITTED,
    REPEATABLE_READ,
    SERIALIZABLE,
};

// Sent by the server in response to SSLRequest
inline constexpr char SSL_ACCEPTED = 'S';
inline constexpr char SSL_REJECTED = 'N';

// Enough for any millisecond GUC value
inline constexpr std::size_t TIMEOUT_DIGITS = 16;

// One startup parameter. Empty values are skipped rather than sent, since an
// empty GUC is not the same as an absent one.
struct PgStartupParam {
    std::string_view key;
    std::string_view value;
};

// Shared by every connection to one endpoint, handed over as userCtx. Views
// only, for the static initialization reason above.
struct PgOptions {
    std::string_view user;
    std::string_view password;
    std::string_view database;
    std::string_view applicationName;
    std::string_view searchPath;
    std::string_view timeZone;
    std::uint32_t statementTimeoutMs = 0;
    std::uint32_t lockTimeoutMs = 0;
    std::uint32_t idleInTransactionTimeoutMs = 0;
    std::uint32_t maxMessageBytes = 16u * 1024u * 1024u;

    std::uint32_t statementCacheSize = 64;
    std::uint16_t statementCacheMinUses = 2;

    // Points at the endpoint's own control block, so plan invalidation reaches
    // every connection rather than just the one that saw the rejection
    PgCacheControl* cacheControl = nullptr;

    PgEncryption encryption = PgEncryption::REQUIRED;
    PgAuthPolicy authPolicy = PgAuthPolicy::NO_PLAINTEXT;
    bool preferBinary = true;
};

// -----------------------------------------------------------------------
// Per-connection state. Lives as long as the slot and survives every request
// that runs on it.
// -----------------------------------------------------------------------
struct SlotState {
    const PgOptions* options = nullptr;

    // Prepared statements are session state, so this cannot be shared with
    // any other connection
    PgStatementCache stmtCache;

    // From BackendKeyData, the pair a CancelRequest has to quote
    std::int32_t backendPid = 0;
    std::int32_t secretKey = 0;

    // Last transaction status byte from ReadyForQuery, which is how a pooled
    // slot knows whether it is safe to hand back
    char txStatus = TX_IDLE;

    // How many ReadyForQuery this request will produce. Normally one, but two
    // when a rollback had to be prepended, since releasing a reservation only
    // unpins the connection and leaves an abandoned transaction open on it.
    std::uint8_t expectedReady = 1;

    // Cache entry the in-flight request sent a Parse for, so the reply can
    // record whether the server accepted it. STMT_NONE when it sent none.
    std::uint32_t pendingStmt = STMT_NONE;

    // Rows one Execute may return. Zero means this request is not a stream.
    std::uint32_t streamRows = 0;

    PgStreamPhase streamPhase = PgStreamPhase::NONE;

    // A stream on a session that was already in a transaction must leave that
    // transaction alone, so it closes its portal instead of committing
    bool streamOwnsTx = false;

    // The delivered chunk is still borrowed when the next round goes out, so its
    // rows are cleared as that round's reply arrives rather than when it was
    // handed over
    bool streamRowsStale = false;

    // Set by the first ReadyForQuery of the handshake, which is what ends the
    // connect loop and lets the slot join the pool
    bool ready = false;
};

// Renders a millisecond GUC value into caller storage, empty when unset
inline std::string_view TimeoutValue(std::uint32_t ms, char (&buf)[TIMEOUT_DIGITS]) noexcept
{
    if(ms == 0)
        return {};

    const auto [ptr, ec] = std::to_chars(buf, buf + TIMEOUT_DIGITS, ms);
    if(ec != std::errc{})
        return {};

    return {buf, static_cast<std::size_t>(ptr - buf)};
}

// -----------------------------------------------------------------------
// Message sizing
//
// Every send is built into a buffer sized from its own contents. A fixed
// buffer would fail the handshake on a long search_path or password, and it
// would fail by looking like a connection error.
// -----------------------------------------------------------------------
inline std::size_t StartupSize(const PgOptions& opts, const PgStartupParam* extra, std::uint32_t extraCount) noexcept
{
    // length + protocol version + trailing terminator
    std::size_t n = 4 + 4 + 1;

    n += 4 + 1 + opts.user.size() + 1; // "user"
    if(!opts.database.empty())
        n += 8 + 1 + opts.database.size() + 1;
    if(!opts.applicationName.empty())
        n += 16 + 1 + opts.applicationName.size() + 1;

    for(std::uint32_t i = 0; i < extraCount; ++i)
        if(!extra[i].value.empty())
            n += extra[i].key.size() + 1 + extra[i].value.size() + 1;

    return n;
}

inline void WriteStartup(PgWriter& w, const PgOptions& opts, const PgStartupParam* extra, std::uint32_t extraCount)
{
    w.BeginUntyped();
    w.I32(static_cast<std::int32_t>(PROTOCOL_3_0));

    // user is the only parameter the server requires
    w.CStr("user");
    w.CStr(opts.user);

    if(!opts.database.empty()) {
        w.CStr("database");
        w.CStr(opts.database);
    }

    if(!opts.applicationName.empty()) {
        w.CStr("application_name");
        w.CStr(opts.applicationName);
    }

    // Any GUC can be set here, one round trip cheaper than issuing SET
    // statements once the connection is up
    for(std::uint32_t i = 0; i < extraCount; ++i) {
        if(extra[i].value.empty())
            continue;

        w.CStr(extra[i].key);
        w.CStr(extra[i].value);
    }

    w.U8(0); // end of the parameter list
    w.End();
}

// -----------------------------------------------------------------------
// PgHandshakeReader
//
// Frames messages out of successive Receive() results. Unconsumed bytes are
// retained by the engine and redelivered, so a partial message is left alone
// rather than buffered here.
// -----------------------------------------------------------------------
class PgHandshakeReader {
public:
    explicit PgHandshakeReader(std::uint32_t maxMessageBytes) noexcept : max_(maxMessageBytes)
    {}

public: // Feeding
    void Feed(const char* buf, std::uint32_t len) noexcept
    {
        buf_ = buf;
        len_ = len;
        pos_ = 0;
    }

    // How much of the current buffer has been framed, to hand back to Receive()
    std::uint32_t Consumed() const noexcept
    {
        return pos_;
    }

public: // Framing
    // Pulls the next whole message. False means the buffer is exhausted or
    // ends mid-message, so more bytes are needed.
    bool Next(PgMessage& out) noexcept
    {
        const FrameStatus fs = FrameMessage(buf_, len_, pos_, out, max_);
        if(fs == FrameStatus::MALFORMED)
            failed_ = true;

        return fs == FrameStatus::OK;
    }

    bool Failed() const noexcept
    {
        return failed_;
    }

private:
    const char* buf_ = nullptr;
    std::uint32_t len_ = 0;
    std::uint32_t pos_ = 0;
    std::uint32_t max_ = 0;
    bool failed_ = false;
};

// -----------------------------------------------------------------------
// Connection handshake
// -----------------------------------------------------------------------
inline EpCoro PgOnConnect(SlotHandle h, void* slotStateVoid)
{
    auto* state = static_cast<SlotState*>(slotStateVoid);
    const PgOptions& opts = *state->options;

    if(opts.user.empty())
        co_return EpFatal;

    std::uint32_t pending = 0;
    WFX::String out;

    // vvv 1. TLS negotiation vvv
    if(opts.encryption != PgEncryption::NONE) {
        char req[8];
        PgWriter w(req, sizeof(req));
        WriteSslRequest(w);

        if(!w.Ok() || (co_await h.Send(req, w.Pos())) != EpSlotOk)
            co_return EpFatal;

        // The reply is a bare byte, not a framed message
        char verdict = 0;
        while(verdict == 0) {
            auto recv = co_await h.Receive(pending);
            pending = 0;

            if(recv.status != EpSlotOk)
                co_return EpFatal;

            if(recv.len == 0)
                continue;

            // The verdict is the only thing a server may send here: TLS cannot
            // start until we send ClientHello, so anything trailing it is a
            // broken or hostile peer. The engine drops the tail on upgrade
            // anyway, this refuses to talk to the peer at all.
            if(recv.len > 1)
                co_return EpFatal;

            verdict = recv.buf[0];
            pending = 1;
        }

        if(verdict == SSL_ACCEPTED) {
            if((co_await h.UpgradeToTLS()) != EpSlotOk)
                co_return EpFatal;

            // The upgrade consumed the byte, nothing is left to trim
            pending = 0;
        }
        else if(verdict != SSL_REJECTED || opts.encryption == PgEncryption::REQUIRED)
            co_return EpFatal;
    }

    // vvv 2. StartupMessage vvv
    {
        char stmtBuf[TIMEOUT_DIGITS];
        char lockBuf[TIMEOUT_DIGITS];
        char idleBuf[TIMEOUT_DIGITS];

        const PgStartupParam extra[] = {
            {"search_path", opts.searchPath},
            {"TimeZone", opts.timeZone},
            {"statement_timeout", TimeoutValue(opts.statementTimeoutMs, stmtBuf)},
            {"lock_timeout", TimeoutValue(opts.lockTimeoutMs, lockBuf)},
            {"idle_in_transaction_session_timeout", TimeoutValue(opts.idleInTransactionTimeoutMs, idleBuf)},
        };

        const std::uint32_t count = sizeof(extra) / sizeof(extra[0]);
        out.resize(StartupSize(opts, extra, count));

        PgWriter w(out.data(), static_cast<std::uint32_t>(out.size()));
        WriteStartup(w, opts, extra, count);

        if(!w.Ok() || (co_await h.Send(out.data(), w.Pos())) != EpSlotOk)
            co_return EpFatal;
    }

    // vvv 3. Authentication, then everything up to ReadyForQuery vvv
    ScramSha256 scram;
    PgHandshakeReader reader{opts.maxMessageBytes};

    while(!state->ready) {
        auto recv = co_await h.Receive(pending);

        if(recv.status != EpSlotOk)
            co_return EpFatal;

        reader.Feed(recv.buf, static_cast<std::uint32_t>(recv.len));

        // A reply is built here rather than sent inline, because sending while
        // the reader still points into the receive buffer would leave it
        // framing memory the slot may have reused
        bool replyPending = false;
        PgMessage msg;

        while(!replyPending && !state->ready && reader.Next(msg)) {
            switch(msg.type) {
                case BE_AUTHENTICATION: {
                    PgFieldReader r{msg};
                    const auto kind = static_cast<std::uint32_t>(r.I32());
                    if(!r.Ok())
                        co_return EpFatal;

                    if(kind == AUTH_OK)
                        break;

                    if(kind == AUTH_CLEARTEXT_PASSWORD) {
                        if(opts.authPolicy != PgAuthPolicy::ANY)
                            co_return EpFatal;

                        out.resize(1 + 4 + opts.password.size() + 1);
                        PgWriter w(out.data(), static_cast<std::uint32_t>(out.size()));
                        w.Begin(FE_PASSWORD);
                        w.CStr(opts.password);
                        w.End();

                        if(!w.Ok())
                            co_return EpFatal;

                        out.resize(w.Pos());
                        replyPending = true;
                        break;
                    }

                    if(kind == AUTH_SASL) {
                        if(!ScramSelectMechanism(r.Rest()))
                            co_return EpFatal;

                        WFX::String clientFirst;
                        if(!scram.BuildClientFirst(clientFirst))
                            co_return EpFatal;

                        out.resize(1 + 4 + SCRAM_SHA_256.size() + 1 + 4 + clientFirst.size());
                        PgWriter w(out.data(), static_cast<std::uint32_t>(out.size()));
                        w.Begin(FE_PASSWORD);
                        w.CStr(SCRAM_SHA_256);
                        w.I32(static_cast<std::int32_t>(clientFirst.size()));
                        w.Str(clientFirst);
                        w.End();

                        if(!w.Ok())
                            co_return EpFatal;

                        out.resize(w.Pos());
                        replyPending = true;
                        break;
                    }

                    if(kind == AUTH_SASL_CONTINUE) {
                        WFX::String clientFinal;
                        if(!scram.BuildClientFinal(r.Rest(), opts.password, clientFinal))
                            co_return EpFatal;

                        out.resize(1 + 4 + clientFinal.size());
                        PgWriter w(out.data(), static_cast<std::uint32_t>(out.size()));
                        w.Begin(FE_PASSWORD);
                        w.Str(clientFinal);
                        w.End();

                        if(!w.Ok())
                            co_return EpFatal;

                        out.resize(w.Pos());
                        replyPending = true;
                        break;
                    }

                    if(kind == AUTH_SASL_FINAL) {
                        if(!scram.VerifyServerFinal(r.Rest()))
                            co_return EpFatal;

                        break;
                    }

                    // MD5 lands here: the crypto backend exposes SHA-2 only, so
                    // there is no way to answer it. GSS, SSPI and Kerberos are
                    // equally unsupported.
                    co_return EpFatal;
                }

                case BE_BACKEND_KEY_DATA: {
                    PgFieldReader r{msg};
                    state->backendPid = r.I32();
                    state->secretKey = r.I32();

                    if(!r.Ok())
                        co_return EpFatal;

                    break;
                }

                case BE_READY_FOR_QUERY: {
                    PgFieldReader r{msg};
                    state->txStatus = static_cast<char>(r.U8());
                    state->ready = true;
                    break;
                }

                // Server settings and warnings are informational during setup
                case BE_PARAMETER_STATUS:
                case BE_NOTICE_RESPONSE:
                    break;

                case BE_ERROR_RESPONSE:
                default:
                    co_return EpFatal;
            }
        }

        if(reader.Failed())
            co_return EpFatal;

        pending = reader.Consumed();

        if(replyPending && (co_await h.Send(out.data(), static_cast<std::uint32_t>(out.size()))) != EpSlotOk)
            co_return EpFatal;
    }

    co_return EpReady;
}

// -----------------------------------------------------------------------
// Cancellation
//
// CancelRequest is not an in-band message: it has to arrive on a second
// connection, quoting the key the server handed out at startup.
// -----------------------------------------------------------------------
inline EpAbortCoro PgOnAbort(AbortSlotHandle h, void* slotStateVoid)
{
    const auto* state = static_cast<const SlotState*>(slotStateVoid);

    if(state->backendPid == 0)
        co_return;

    auto [status, side] = co_await h.OpenSideConnection();
    if(status != EpSlotOk)
        co_return;

    char buf[16];
    PgWriter w(buf, sizeof(buf));
    WriteCancelRequest(w, state->backendPid, state->secretKey);

    if(w.Ok())
        (void)co_await side.Send(buf, w.Pos());

    side.Close();
    co_return;
}

} // namespace WFX::Postgres::Detail

#endif // WFX_INC_WFX_ENDPOINT_POSTGRES_CONNECTION_HPP
