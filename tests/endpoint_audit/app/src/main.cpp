// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits
//
// Endpoint audit target. Unlike the inbound torture app, every route here turns
// an *inbound* request into an *outbound* WFX::HttpEndpoint call against the
// scriptable mock upstream (http_upstream.py, pinned at 127.0.0.1:UPSTREAM_PORT) and
// reflects the result back as JSON the harness can assert on:
//
//   { "ep": <EndpointStatus int>,          // 0 == SUCCESS, see shared/abis/types.hpp
//     "status": <upstream HTTP status>,     // present only when ep == 0
//     "bodylen": <response body length>,    //   "
//     "body": "<response body>",            //   "  (mock bodies are small ASCII)
//     "hdr": "<value of X-Want header>" }   //   "  (only if X-Want was sent)
//
// The inbound request selects what to do via headers (kept header-only so the
// harness never has to encode anything into the path):
//
//   X-Ep      default | small | fast | coalesce   (which endpoint instance)
//   X-Method  GET | HEAD | OPTIONS | DELETE | POST | PUT | PATCH   (default GET)
//   X-Path    upstream request target                             (default "/ok")
//   X-Body    request body for POST/PUT/PATCH                      (optional)
//   X-Fwd     a header to forward upstream, "Name: Value"          (optional)
//   X-Want    upstream response header to echo back into "hdr"     (optional)
//
// The four endpoint instances differ only in the knobs each test needs:
//   default  -> roomy limits, the everyday client
//   small    -> tiny header/body caps so limit enforcement is cheap to trigger
//   fast     -> 1s request timeout so slow-upstream cases fail fast
//   coalesce -> coalesceKey set, to prove concurrent-request dedup
//
// Further down (search "Raw-protocol endpoint") this file also drives the RAW
// WFX::Endpoint<> primitive directly against a tiny hand-rolled text protocol
// (http_upstream.py's second listener, PROTO_UPSTREAM) to cover onConnect,
// onDisconnect, and multiplexing, three things HttpEndpoint structurally
// cannot exercise, since HTTP/1.1 has no connection handshake and no concept
// of concurrent requests sharing one connection.
//
// This file also drives WFX::SmtpEndpoint against a second, hostile mock (smtp_upstream.py,
// one persona per fixed port) via /smtp/send and /smtp/inject. See the comment above each
// route for its request/response shape.

#include <wfx/http.hpp>
#include <wfx/memory.hpp>
#include <wfx/telemetry.hpp>
#include <wfx/utils/hash.hpp>
#include <wfx/endpoint/http.hpp>
#include <wfx/endpoint/smtp.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// The mock upstream is pinned here at COMPILE time (HttpEndpoint bakes host:port
// into the instance). The harness MUST launch http_upstream.py on this exact port; it
// is also mirrored in endpoint_audit.py as UPSTREAM_PORT.
#define UPSTREAM "127.0.0.1:8091"

// Second mock listener, a raw hand-rolled protocol (see "Raw-protocol endpoint"
// below), mirrored in endpoint_audit.py as PROTO_UPSTREAM_PORT.
#define PROTO_UPSTREAM "127.0.0.1:8092"


// Endpoint instances

// Everyday client: roomy limits, plaintext (the mock speaks cleartext HTTP/1.1).
inline const auto EpDefault = WFX::HttpEndpoint{UPSTREAM, WFX::HttpEndpointConfig{
    .connLimit             = 4,
    .requestTimeoutSeconds = 10,
    .tlsConfig             = WFX::EpTlsInsecure,
}};

// Deliberately tiny caps so the harness can trip maxHeaderBytes / maxHeaderCount /
// maxBodyBytes with small, fast upstream responses instead of multi-MB payloads.
inline const auto EpSmall = WFX::HttpEndpoint{UPSTREAM, WFX::HttpEndpointConfig{
    .connLimit             = 2,
    .requestTimeoutSeconds = 10,
    .tlsConfig             = WFX::EpTlsInsecure,
    .maxHeaderBytes        = 256,
    .maxBodyBytes          = 1024,
    .maxHeaderCount        = 8,
}};

// Minimum request budget the engine allows (timeouts must be >= the 5s timer-tick
// cooldown, INVOKE_TIMEOUT_COOLDOWN). slow-header / slow-body upstreams stall well
// past this, so they surface as EpRequestTimeout.
inline const auto EpFast = WFX::HttpEndpoint{UPSTREAM, WFX::HttpEndpointConfig{
    .connLimit             = 2,
    .connectTimeoutSeconds = 5,
    .requestTimeoutSeconds = 5,
    .tlsConfig             = WFX::EpTlsInsecure,
}};

// Coalesce identical concurrent GETs into one backend call. Key = FNV-1a(path);
// non-GET returns 0 (never coalesced), matching the header's documented pattern.
inline std::uint64_t CoalesceByPath(const void* reqVoid) noexcept
{
    const auto& r = *static_cast<const WFX::HttpEndpointRequest*>(reqVoid);
    if(r.method != WFX::HttpMethod::GET)
        return 0;

    const std::uint64_t h = WFX::Fnv1a(r.path);

    return h ? h : 1; // 0 is reserved for "don't coalesce"
}

inline const auto EpCoalesce = WFX::HttpEndpoint{UPSTREAM, WFX::HttpEndpointConfig{
    .connLimit             = 4,
    .requestTimeoutSeconds = 10,
    .tlsConfig             = WFX::EpTlsInsecure,
    .coalesceKey           = &CoalesceByPath,
}};

// Single-slot pool: forces every sequential request onto the SAME pooled
// connection + slot state, so the harness can prove there is no cross-request
// bleed of body / headers / status across keep-alive reuse.
inline const auto EpReuse = WFX::HttpEndpoint{UPSTREAM, WFX::HttpEndpointConfig{
    .connLimit             = 1,
    .requestTimeoutSeconds = 10,
    .tlsConfig             = WFX::EpTlsInsecure,
}};

// Connection-lifecycle probes
// Points at a port with nothing listening: connect() is refused (RST) -> the
// request must fail cleanly and quickly, never hang the worker.
inline const auto EpDead = WFX::HttpEndpoint{"127.0.0.1:9", WFX::HttpEndpointConfig{
    .connLimit                   = 1,
    .connectTimeoutSeconds       = 5,
    .requestTimeoutSeconds       = 5,
    .maxReconnectAttempts        = 1,
    .reconnectBackoffBaseSeconds = 1,
    .reconnectBackoffMaxSeconds  = 1,
    .tlsConfig                   = WFX::EpTlsInsecure,
}};

// Points at an unrouteable TEST-NET-1 address (RFC 5737): the SYN is black-holed,
// so the connect attempt must surface as an error within the connect/request budget.
inline const auto EpUnreach = WFX::HttpEndpoint{"192.0.2.1:80", WFX::HttpEndpointConfig{
    .connLimit                   = 1,
    .connectTimeoutSeconds       = 5,
    .requestTimeoutSeconds       = 5,
    .maxReconnectAttempts        = 1,
    .reconnectBackoffBaseSeconds = 1,
    .reconnectBackoffMaxSeconds  = 1,
    .tlsConfig                   = WFX::EpTlsInsecure,
}};

// Smallest idle timeout the engine allows (>= 5s tick cooldown) so a pooled
// keep-alive connection is recycled within the test window.
inline const auto EpIdle = WFX::HttpEndpoint{UPSTREAM, WFX::HttpEndpointConfig{
    .connLimit             = 1,
    .requestTimeoutSeconds = 5,
    .idleTimeoutSeconds    = 5,
    .tlsConfig             = WFX::EpTlsInsecure,
}};

// Eagerly opens 3 connections at startup (before any request is driven to it).
inline const auto EpPrewarm = WFX::HttpEndpoint{UPSTREAM, WFX::HttpEndpointConfig{
    .connLimit             = 4,
    .requestTimeoutSeconds = 5,
    .tlsConfig             = WFX::EpTlsInsecure,
    .prewarm               = 3,
}};


// Raw-protocol endpoint: onConnect / onDisconnect / multiplexing coverage
//
// HttpEndpoint can't exercise any of this: HTTP/1.1 has no connection handshake
// and no concept of concurrent requests sharing one connection. This section
// drives the RAW WFX::Endpoint<> primitive directly against a tiny hand-rolled
// text protocol spoken by http_upstream.py's second listener (PROTO_UPSTREAM):
//
//   - onConnect: a one-line "AUTH <token>\n" handshake, once per physical
//     connection. "good" succeeds, "bad" is rejected, "slow" stalls past
//     connectTimeoutSeconds, "reset" drops the connection mid-handshake.
//   - onDisconnect: counted per DisconnectReason (idle timeout / handshake
//     timeout / anything else) so the harness can assert the right reason
//     fired for the right scenario.
//   - Multiplexing: every request is tagged with a stream id (REQ <id> <key>),
//     and the mock deliberately answers out of order (RES <id> <value>, after
//     a per-request delay baked into the key) so the harness can prove
//     responses are matched back to the right caller by id, never by arrival
//     order, all sharing the SAME pooled connection (connLimit = 1).
//
// Routes (both header-only, like /call above):
//   GET /proto/call
//     X-Proto  good | bad | slow | reset   (which endpoint instance, default good)
//     X-Key    key to echo back; "sleep:<secs>:<value>" delays the mock's reply
//   GET  /proto/disconnects        -> {"idle", "handshake", "error"} counters
//   POST /proto/disconnects/reset  -> zero all three counters

namespace {

struct ProtoReq {
    WFX::String key;
};

struct ProtoRes {
    WFX::String value;
};

struct PendingReply {
    std::uint64_t id;
    ProtoRes* res; // owned until takeStreamOutput hands it off
};

// One per physical connection. token is which handshake this slot sends,
// injected via userCtx so every instance can share the same onConnect function.
struct ProtoSlotState {
    const char* token;
    std::uint64_t nextId = 1;
    std::uint32_t inFlight = 0;
    WFX::Vector<PendingReply> finished;
};

// Single worker (see wfx.toml, worker_processes = 1), so these are the engine's
// own event-loop thread only, no locking needed.
std::uint64_t g_idleDisconnects = 0;
std::uint64_t g_handshakeTimeouts = 0;
std::uint64_t g_errorDisconnects = 0;

void* ProtoCreateSlotState(void* userCtx)
{
    auto* state = WFX::New<ProtoSlotState>();
    state->token = static_cast<const char*>(userCtx);
    return state;
}

void ProtoDestroySlotState(void* slotState)
{
    WFX::Delete(static_cast<ProtoSlotState*>(slotState));
}

void ProtoOnDisconnect(void* /*slotState*/, WFX::DisconnectReason reason)
{
    if(reason == WFX::EpIdleTimeout)
        g_idleDisconnects++;
    else if(reason == WFX::EpHandshakeTimeoutReason)
        g_handshakeTimeouts++;
    else
        g_errorDisconnects++;
}

WFX::EpCoro ProtoAuthenticate(WFX::SlotHandle h, void* slotStateVoid)
{
    auto* state = static_cast<ProtoSlotState*>(slotStateVoid);

    WFX::String line = "AUTH ";
    line += state->token;
    line += "\n";

    if(co_await h.Send(line.data(), static_cast<std::uint32_t>(line.size())) != WFX::EpSlotOk)
        co_return WFX::EpFatal;

    auto recv = co_await h.Receive();
    if(recv.status != WFX::EpSlotOk)
        co_return WFX::EpFatal;

    std::string_view reply{recv.buf, recv.len};
    co_return reply.starts_with("OK") ? WFX::EpReady : WFX::EpFatal;
}

WFX::Shared::SerializeResult ProtoSerialize(void* slotStateVoid, const void* reqVoid, char* buf,
                                            std::uint32_t bufLen, std::uint32_t* written,
                                            std::uint64_t* streamKey)
{
    auto* state = static_cast<ProtoSlotState*>(slotStateVoid);
    auto& req = *static_cast<const ProtoReq*>(reqVoid);

    std::uint64_t id = state->nextId++;
    int n = std::snprintf(buf, bufLen, "REQ %llu %s\n", static_cast<unsigned long long>(id), req.key.c_str());
    if(n < 0 || static_cast<std::uint32_t>(n) >= bufLen)
        return WFX::EpSerBufferTooSmall;

    *written = static_cast<std::uint32_t>(n);
    *streamKey = id;
    state->inFlight++;
    return WFX::EpSerOk;
}

WFX::Shared::ParseResult ProtoParse(void* slotStateVoid, void* /*parseState, unused*/, const char* buf,
                                    std::uint32_t len, std::uint32_t* consumed, void* /*outObj, unused here*/,
                                    bool isEof, std::uint64_t* completedKey)
{
    auto* state = static_cast<ProtoSlotState*>(slotStateVoid);
    std::string_view view{buf, len};

    auto nl = view.find('\n');
    if(nl == std::string_view::npos) {
        *consumed = 0;
        return isEof ? WFX::EpParseError : WFX::EpParseIncomplete;
    }

    std::string_view line = view.substr(0, nl);
    *consumed = static_cast<std::uint32_t>(nl + 1);

    // "RES <id> <value>"
    if(!line.starts_with("RES "))
        return WFX::EpParseError;

    line.remove_prefix(4);
    auto sp = line.find(' ');
    if(sp == std::string_view::npos)
        return WFX::EpParseError;

    std::uint64_t id = 0;
    for(char c : line.substr(0, sp)) {
        if(c < '0' || c > '9')
            return WFX::EpParseError;
        id = id * 10 + static_cast<std::uint64_t>(c - '0');
    }

    auto* res = WFX::New<ProtoRes>();
    const auto val = line.substr(sp + 1);
    res->value.assign(val.data(), val.size());

    state->finished.push_back({id, res});
    state->inFlight--;
    *completedKey = id;

    // The connection stays open for other in-flight streams, so this stream
    // finishing does not mean there is nothing left to read
    return WFX::EpParseIncomplete;
}

// Never actually called on the multiplexed path (the engine only calls
// destroyOutput here, to free a finished-but-abandoned or delivered response),
// but createOutput/destroyOutput are validated as a pair at startup regardless
// of hasCapacity, so this has to exist even though it's dead weight at runtime.
void* ProtoCreateOutput(void*)
{
    return nullptr;
}

void ProtoDestroyOutput(void* outputPtr)
{
    WFX::Delete(static_cast<ProtoRes*>(outputPtr));
}

bool ProtoHasCapacity(void* slotStateVoid)
{
    return static_cast<ProtoSlotState*>(slotStateVoid)->inFlight < 32;
}

void* ProtoTakeStreamOutput(void* slotStateVoid, std::uint64_t key)
{
    auto* state = static_cast<ProtoSlotState*>(slotStateVoid);
    for(auto it = state->finished.begin(); it != state->finished.end(); ++it) {
        if(it->id == key) {
            void* res = it->res;
            state->finished.erase(it);
            return res;
        }
    }

    return nullptr; // not finished yet
}

WFX::EndpointDesc ProtoDesc(const char* token)
{
    return WFX::EndpointDesc{
        .serialize        = ProtoSerialize,
        .parse            = ProtoParse,
        .onDisconnect     = ProtoOnDisconnect,
        .createSlotState  = ProtoCreateSlotState,
        .destroySlotState = ProtoDestroySlotState,
        .createOutput     = ProtoCreateOutput,
        .destroyOutput    = ProtoDestroyOutput,
        .hasCapacity      = ProtoHasCapacity,
        .takeStreamOutput = ProtoTakeStreamOutput,
        .userCtx          = const_cast<void*>(static_cast<const void*>(token)),
    };
}

using ProtoEp = WFX::Endpoint<ProtoReq, ProtoRes, &ProtoAuthenticate>;

} // namespace

// Handshake succeeds, single slot (connLimit=1) so every concurrent /proto/call
// against this instance is forced to multiplex onto the SAME connection.
// idleTimeoutSeconds at the engine's 5s floor to make idle-disconnect observable
// inside a reasonable test window.
inline const ProtoEp ProtoGood{
    PROTO_UPSTREAM, ProtoDesc("good"),
    WFX::EndpointConfig{
        .connLimit             = 1,
        .connectTimeoutSeconds = 5,
        .requestTimeoutSeconds = 10,
        .idleTimeoutSeconds    = 5,
        .maxReconnectAttempts  = 1,
        .tlsConfig             = WFX::EpTlsInsecure,
        .maxConcurrentStreams  = 32,
    }
};

// Mock replies ERR: onConnect returns EpFatal, the request must fail cleanly.
inline const ProtoEp ProtoBad{
    PROTO_UPSTREAM, ProtoDesc("bad"),
    WFX::EndpointConfig{
        .connLimit             = 1,
        .connectTimeoutSeconds = 5,
        .requestTimeoutSeconds = 5,
        .idleTimeoutSeconds    = 5,
        .maxReconnectAttempts  = 1,
        .reconnectBackoffBase  = 1,
        .reconnectBackoffMax   = 1,
        .tlsConfig             = WFX::EpTlsInsecure,
    }
};

// Mock stalls the AUTH reply well past connectTimeoutSeconds (the engine's 5s
// floor): the connect-phase timeout must surface as EpHandshakeTimeout.
inline const ProtoEp ProtoSlow{
    PROTO_UPSTREAM, ProtoDesc("slow"),
    WFX::EndpointConfig{
        .connLimit             = 1,
        .connectTimeoutSeconds = 5,
        .requestTimeoutSeconds = 5,
        .idleTimeoutSeconds    = 5,
        .maxReconnectAttempts  = 1,
        .reconnectBackoffBase  = 1,
        .reconnectBackoffMax   = 1,
        .tlsConfig             = WFX::EpTlsInsecure,
    }
};

// Mock drops the connection mid-handshake without replying: co_await h.Receive()
// must fail, onConnect returns EpFatal from its own error path.
inline const ProtoEp ProtoReset{
    PROTO_UPSTREAM, ProtoDesc("reset"),
    WFX::EndpointConfig{
        .connLimit             = 1,
        .connectTimeoutSeconds = 5,
        .requestTimeoutSeconds = 5,
        .idleTimeoutSeconds    = 5,
        .maxReconnectAttempts  = 1,
        .reconnectBackoffBase  = 1,
        .reconnectBackoffMax   = 1,
        .tlsConfig             = WFX::EpTlsInsecure,
    }
};

namespace {
const ProtoEp* ProtoEndpointOf(std::string_view name) noexcept
{
    if(name == "bad")   return &ProtoBad;
    if(name == "slow")  return &ProtoSlow;
    if(name == "reset") return &ProtoReset;
    return &ProtoGood;
}
} // namespace

// ---------------------------------------------------------------------------
// Third protocol ("SP"), spoken to http_upstream.py's sp listener on SP_UPSTREAM.
//
// Proto above sets hasCapacity, so it is permanently on the multiplexed path.
// Slot pinning and streaming are single-slot-only by construction, and onPush
// only fires for a slot with nothing in flight, so none of the three can be
// reached through Proto at all. SP is therefore deliberately NOT multiplexed.
//
// Wire format, line oriented, '\n' terminated:
//   handshake  -> "STARTTLS\n"          <- "S\n" (upgrade) | "N\n" (stay plain)
//                 "AUTH <token>\n"      <- "OK <connId>\n" | "ERR\n"
//   request    -> "GET <key>\n"         <- "VAL <value>\n"
//              -> "STREAM <n> <sz>\n"   <- "CHUNK <payload>\n" * n, "END\n"
//              -> "PAGE <n> <sz>\n"     <- "CHUNK <payload>\n", then the engine
//                                          re-serializes and we send "MORE\n"
//                                          for each subsequent chunk, "END\n"
//   push       ->                       <- "PUSH <text>\n" at any idle moment
//
// STREAM exercises CHUNK_READY (server keeps sending), PAGE exercises
// CHUNK_READY_FETCH (server sends nothing until asked). connId is echoed in
// every reply so the harness can prove which physical connection served a
// request, which is what makes the pinning isolation assertions possible.
// ---------------------------------------------------------------------------
#define SP_UPSTREAM "127.0.0.1:8093"

namespace {

enum class SpMode : std::uint8_t { GET, STREAM_SERVER, STREAM_FETCH };

struct SpReq {
    SpMode mode = SpMode::GET;
    WFX::String key;
    std::uint32_t count = 0;
    std::uint32_t size = 0;
    std::uint32_t stallAfter = 0; // STREAM_SERVER only: stall before this chunk index, 0 = never
    std::uint32_t stallMs = 0;    // how long that stall lasts
    bool leakAux = false;         // GET only: onAbort skips side.Close(), proves the timeout reclaims it
};

// One instance per slot, reused for every chunk of a stream. value is ASSIGNED
// per chunk, never appended to: that reuse is exactly what the engine's
// bounded-memory guarantee rests on, and /sp/rss is what proves it holds.
struct SpRes {
    WFX::String value;
    std::uint64_t connId = 0;
    bool ended = false;
};

struct SpSlotState {
    const char* token;
    bool tryTls;
    bool requireTls;
    std::uint64_t connId = 0;
    SpMode mode = SpMode::GET;
    std::uint32_t pagesSent = 0;
    bool leakAux = false; // stashed by SpSerialize, read by SpOnAbort
};

// Single worker (wfx.toml, worker_processes = 1), so plain globals are fine
std::uint64_t g_spPushCount = 0;
std::uint64_t g_spPushBytes = 0;
std::uint64_t g_spPushRejects = 0;
std::uint64_t g_spDisconnects = 0;

// onAbort observability: the app's own view, independent of what the mock recorded,
// so the audit can tell "OpenSideConnection failed" apart from "cancel never arrived"
std::uint64_t g_spAbortRuns = 0;
std::uint64_t g_spAbortSideOpens = 0;
std::uint64_t g_spAbortSideFailures = 0;

void* SpCreateSlotState(void* userCtx)
{
    // userCtx packs the three per-endpoint knobs as "token:tryTls:requireTls"
    auto* state = WFX::New<SpSlotState>();
    std::string_view cfg{static_cast<const char*>(userCtx)};

    const auto c1 = cfg.find(':');
    const auto c2 = cfg.find(':', c1 + 1);

    state->token = static_cast<const char*>(userCtx); // token is the leading field
    state->tryTls = cfg.substr(c1 + 1, c2 - c1 - 1) == "1";
    state->requireTls = cfg.substr(c2 + 1) == "1";

    return state;
}

void SpDestroySlotState(void* slotState)
{
    WFX::Delete(static_cast<SpSlotState*>(slotState));
}

void SpOnDisconnect(void* /*slotState*/, WFX::DisconnectReason)
{
    g_spDisconnects++;
}

void* SpCreateOutput(void*)
{
    return WFX::New<SpRes>();
}

void SpDestroyOutput(void* out)
{
    WFX::Delete(static_cast<SpRes*>(out));
}

WFX::EpCoro SpConnect(WFX::SlotHandle h, void* slotStateVoid)
{
    auto* st = static_cast<SpSlotState*>(slotStateVoid);

    // The token rides along on the probe: STARTTLS precedes AUTH, so it is the
    // only way the mock can tell which endpoint instance is connecting and
    // therefore which upgrade behavior to act out
    const std::string_view token{st->token, std::string_view{st->token}.find(':')};

    if(st->tryTls) {
        WFX::String probe = "STARTTLS ";
        probe.append(token.data(), token.size());
        probe += "\n";

        if(co_await h.Send(probe.data(), static_cast<std::uint32_t>(probe.size())) != WFX::EpSlotOk)
            co_return WFX::EpFatal;

        auto answer = co_await h.Receive();
        if(answer.status != WFX::EpSlotOk)
            co_return WFX::EpFatal;

        const std::string_view reply{answer.buf, answer.len};

        if(reply.starts_with("S")) {
            if(co_await h.UpgradeToTLS() != WFX::EpSlotOk)
                co_return WFX::EpFatal;
        }

        // Server declined. Failing closed here is the whole point: MySQL's --ssl
        // (CVE-2015-3152) and pgJDBC (CVE-2025-49146) both silently continued in
        // plaintext instead, which is what made them MITM-able
        else if(st->requireTls)
            co_return WFX::EpFatal;
    }

    WFX::String line = "AUTH ";
    line.append(token.data(), token.size());
    line += "\n";

    if(co_await h.Send(line.data(), static_cast<std::uint32_t>(line.size())) != WFX::EpSlotOk)
        co_return WFX::EpFatal;

    auto recv = co_await h.Receive();
    if(recv.status != WFX::EpSlotOk)
        co_return WFX::EpFatal;

    std::string_view ok{recv.buf, recv.len};
    if(!ok.starts_with("OK"))
        co_return WFX::EpFatal;

    // "OK <connId>"; remembered so every response can report which physical
    // connection produced it
    ok.remove_prefix(2);
    while(!ok.empty() && (ok.front() == ' '))
        ok.remove_prefix(1);

    std::uint64_t id = 0;
    for(char c : ok) {
        if(c < '0' || c > '9')
            break;
        id = id * 10 + static_cast<std::uint64_t>(c - '0');
    }
    st->connId = id;

    co_return WFX::EpReady;
}

// Postgres-style graceful cancel: opens a throwaway connection to the same upstream and
// sends "CANCEL <connId>\n", no auth (mirrors a real Postgres CancelRequest, which also
// rides a brand-new connection with no prior handshake). connId identifies which physical
// primary connection is being aborted, exactly the way a real PID+secret key would
//
// Deliberately does NOT touch 'h' beyond OpenSideConnection()/NegotiatedProtocol(): the
// primary slot is still mid-request, and AbortSlotHandle doesn't even expose Send/Receive
// on it for that reason (see the plan doc / base.hpp doc comment for why that's unsafe)
WFX::EpAbortCoro SpOnAbort(WFX::AbortSlotHandle h, void* slotStateVoid)
{
    auto* st = static_cast<SpSlotState*>(slotStateVoid);
    g_spAbortRuns++;

    auto [status, side] = co_await h.OpenSideConnection();
    if(status != WFX::EpSlotOk) {
        // auxConnLimit exhausted (or 0/disabled), or the dial itself failed. Either way,
        // there's nothing to send the cancel on: give up quietly. The primary slot is
        // unaffected either way, its own response/timeout cycle doesn't depend on this
        g_spAbortSideFailures++;
        co_return;
    }

    g_spAbortSideOpens++;

    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "CANCEL %llu\n", static_cast<unsigned long long>(st->connId));
    if(n > 0)
        co_await side.Send(buf, static_cast<std::uint32_t>(n));

    // leakAux deliberately skips this, to prove connectTimeoutSeconds reclaims a side
    // connection whose caller forgot (or, here, chose not) to Close() it.
    if(!st->leakAux)
        side.Close();

    co_return;
}

// Hostile/buggy protocol simulation: a real onAbort author will get this wrong at least
// once. Closes the side connection TWICE and keeps using the handle afterward. The engine
// must survive this (bump generationId before freeing, same discipline every other
// pool-recycle path already uses), never double-free the aux slot or corrupt a DIFFERENT
// side connection that gets handed that same slot in the meantime.
WFX::EpAbortCoro SpOnAbortDoubleClose(WFX::AbortSlotHandle h, void* slotStateVoid)
{
    auto* st = static_cast<SpSlotState*>(slotStateVoid);
    g_spAbortRuns++;

    auto [status, side] = co_await h.OpenSideConnection();
    if(status != WFX::EpSlotOk) {
        g_spAbortSideFailures++;
        co_return;
    }

    g_spAbortSideOpens++;

    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "CANCEL %llu\n", static_cast<unsigned long long>(st->connId));
    if(n > 0)
        co_await side.Send(buf, static_cast<std::uint32_t>(n));

    side.Close();
    side.Close(); // deliberate misuse: must be a harmless no-op, not a double-free
    side.Close(); // ...and a third time for good measure

    co_return;
}

WFX::Shared::SerializeResult SpSerialize(void* slotStateVoid, const void* reqVoid, char* buf, std::uint32_t bufLen,
                                         std::uint32_t* written, std::uint64_t* /*streamKey, not multiplexed*/)
{
    auto* st = static_cast<SpSlotState*>(slotStateVoid);
    auto& req = *static_cast<const SpReq*>(reqVoid);

    int n = 0;

    // A re-serialize on a fetch stream is the engine asking for the next page.
    // The cursor lives in slot state, exactly where a real protocol would keep
    // its portal name / paging_state / SCAN cursor
    if(req.mode == SpMode::STREAM_FETCH && st->pagesSent > 0)
        n = std::snprintf(buf, bufLen, "MORE\n");
    else if(req.mode == SpMode::STREAM_FETCH)
        n = std::snprintf(buf, bufLen, "PAGE %u %u\n", req.count, req.size);
    else if(req.mode == SpMode::STREAM_SERVER)
        n = std::snprintf(buf, bufLen, "STREAM %u %u %u %u\n", req.count, req.size, req.stallAfter, req.stallMs);
    else
        n = std::snprintf(buf, bufLen, "GET %s\n", req.key.c_str());

    if(n < 0 || static_cast<std::uint32_t>(n) >= bufLen)
        return WFX::EpSerBufferTooSmall;

    st->mode = req.mode;
    st->leakAux = req.leakAux;
    *written = static_cast<std::uint32_t>(n);

    return WFX::EpSerOk;
}

WFX::Shared::ParseResult SpParse(void* slotStateVoid, void* /*parseState*/, const char* buf, std::uint32_t len,
                                 std::uint32_t* consumed, void* outObj, bool isEof, std::uint64_t* /*completedKey*/)
{
    auto* st = static_cast<SpSlotState*>(slotStateVoid);
    auto* out = static_cast<SpRes*>(outObj);

    std::string_view view{buf, len};

    const auto nl = view.find('\n');
    if(nl == std::string_view::npos) {
        *consumed = 0;
        return isEof ? WFX::EpParseError : WFX::EpParseIncomplete;
    }

    std::string_view line = view.substr(0, nl);
    *consumed = static_cast<std::uint32_t>(nl + 1);

    out->connId = st->connId;

    if(line.starts_with("VAL ")) {
        out->value.assign(line.substr(4));
        out->ended = true;
        st->pagesSent = 0;
        return WFX::EpParseDone;
    }

    if(line.starts_with("CHUNK ")) {
        // assign, never append: peak memory stays at one chunk no matter how
        // many chunks the whole response turns out to be
        out->value.assign(line.substr(6));
        out->ended = false;
        st->pagesSent++;

        return st->mode == SpMode::STREAM_FETCH ? WFX::EpParseChunkFetch : WFX::EpParseChunk;
    }

    if(line.starts_with("END")) {
        out->value.clear();
        out->ended = true;
        st->pagesSent = 0;
        return WFX::EpParseDone;
    }

    return WFX::EpParseError;
}

// Server-initiated "PUSH <text>". Only ever reached on a slot with no request
// in flight; anything arriving mid-request must go through SpParse instead,
// and the harness asserts that split explicitly
bool SpOnPush(void* /*slotState*/, const char* buf, std::uint32_t len, std::uint32_t* consumed)
{
    std::string_view view{buf, len};

    const auto nl = view.find('\n');
    if(nl == std::string_view::npos) {
        *consumed = 0; // partial line, wait for the rest
        return true;
    }

    const std::string_view line = view.substr(0, nl);
    *consumed = static_cast<std::uint32_t>(nl + 1);

    if(!line.starts_with("PUSH ")) {
        g_spPushRejects++;
        return false; // undecodable -> engine closes the slot
    }

    g_spPushCount++;
    g_spPushBytes += line.size() - 5;

    return true;
}

WFX::EndpointDesc SpDesc(const char* cfg)
{
    return WFX::EndpointDesc{
        .serialize        = SpSerialize,
        .parse            = SpParse,
        .onDisconnect     = SpOnDisconnect,
        .createSlotState  = SpCreateSlotState,
        .destroySlotState = SpDestroySlotState,
        .createOutput     = SpCreateOutput,
        .destroyOutput    = SpDestroyOutput,
        .onPush           = SpOnPush,
        .userCtx          = const_cast<void*>(static_cast<const void*>(cfg)),
    };
}

using SpEp = WFX::Endpoint<SpReq, SpRes, &SpConnect>;
using SpAbortEp = WFX::Endpoint<SpReq, SpRes, &SpConnect, &SpOnAbort>;
using SpAbortBadCloseEp = WFX::Endpoint<SpReq, SpRes, &SpConnect, &SpOnAbortDoubleClose>;

} // namespace

// Plain, no TLS probe. connLimit 4 so pinning can hold one slot while other
// callers still get served, and pool exhaustion is reachable in a small loop.
inline const SpEp SpGood{
    SP_UPSTREAM, SpDesc("good:0:0"),
    WFX::EndpointConfig{
        .connLimit             = 4,
        .connectTimeoutSeconds = 5,
        .requestTimeoutSeconds = 10,
        .idleTimeoutSeconds    = 5,
        .maxReconnectAttempts  = 1,
        .tlsConfig             = WFX::EpTlsInsecure,
    }
};

// Probes STARTTLS and REQUIRES it. The mock answers "N", so onConnect must fail
// closed rather than continuing in plaintext (CVE-2015-3152 / CVE-2025-49146).
inline const SpEp SpTlsDowngrade{
    SP_UPSTREAM, SpDesc("downgrade:1:1"),
    WFX::EndpointConfig{
        .connLimit             = 1,
        .connectTimeoutSeconds = 5,
        .requestTimeoutSeconds = 5,
        .idleTimeoutSeconds    = 5,
        .maxReconnectAttempts  = 1,
        .tlsConfig             = WFX::EpTlsInsecure,
    }
};

// Mock answers "S" then sends garbage instead of a ServerHello: the upgrade has
// to fail and tear the slot down cleanly, not hang or leak the coroutine frame.
inline const SpEp SpTlsGarbage{
    SP_UPSTREAM, SpDesc("tlsgarbage:1:0"),
    WFX::EndpointConfig{
        .connLimit             = 1,
        .connectTimeoutSeconds = 5,
        .requestTimeoutSeconds = 5,
        .idleTimeoutSeconds    = 5,
        .maxReconnectAttempts  = 1,
        .tlsConfig             = WFX::EpTlsInsecure,
    }
};

namespace {
const SpEp* SpEndpointOf(std::string_view name) noexcept
{
    if(name == "downgrade")  return &SpTlsDowngrade;
    if(name == "tlsgarbage") return &SpTlsGarbage;
    return &SpGood;
}
} // namespace

// --- onAbort: graceful cancel over a side connection when the client bails --------
// connLimit=4 so overlapping primaries can be in flight at once (needed to drive
// auxConnLimit=1 into exhaustion on purpose, plus headroom for a tight sequential
// leak-reclaim test). connectTimeoutSeconds=5 is the engine's own hard floor
// (must be >= the timer tick interval, INVOKE_TIMEOUT_COOLDOWN) - the reclaim
// test just waits it out rather than fighting it.
inline const SpAbortEp SpAbort{
    SP_UPSTREAM, SpDesc("abort:0:0"),
    WFX::EndpointConfig{
        .connLimit             = 4,
        .auxConnLimit          = 1,
        .connectTimeoutSeconds = 5,
        .requestTimeoutSeconds = 10,
        .idleTimeoutSeconds    = 5,
        .maxReconnectAttempts  = 1,
        .tlsConfig             = WFX::EpTlsInsecure,
    }
};

// auxConnLimit=0: onAbort still fires (desc.onAbort is set), but OpenSideConnection()
// must fail every time with nowhere to allocate from, never a crash or a hang
inline const SpAbortEp SpAbortNoAux{
    SP_UPSTREAM, SpDesc("abortnoaux:0:0"),
    WFX::EndpointConfig{
        .connLimit             = 1,
        .auxConnLimit          = 0,
        .connectTimeoutSeconds = 5,
        .requestTimeoutSeconds = 10,
        .idleTimeoutSeconds    = 5,
        .maxReconnectAttempts  = 1,
        .tlsConfig             = WFX::EpTlsInsecure,
    }
};

// Mock stalls 1s before answering AUTH. Gives the audit a window to abandon the
// client while onConnect is still running, proving onAbort never fires while
// inOnConnectPhase is set (it would steal onConnect's asyncData mid-flight).
inline const SpAbortEp SpAbortMidConnect{
    SP_UPSTREAM, SpDesc("abortmidconnect:0:0"),
    WFX::EndpointConfig{
        .connLimit             = 2,
        .auxConnLimit          = 1,
        .connectTimeoutSeconds = 5,
        .requestTimeoutSeconds = 10,
        .idleTimeoutSeconds    = 5,
        .maxReconnectAttempts  = 1,
        .tlsConfig             = WFX::EpTlsInsecure,
    }
};

// Deliberately hostile onAbort (SpOnAbortDoubleClose): a real protocol author will
// misuse .Close() sooner or later, the engine must not crash or corrupt a DIFFERENT
// side connection over it. Own endpoint (not just a slotState flag), since the whole
// point is exercising the ABI's close path directly, not layering it on SpAbort's config
inline const SpAbortBadCloseEp SpAbortBadClose{
    SP_UPSTREAM, SpDesc("abort:0:0"),
    WFX::EndpointConfig{
        .connLimit             = 1,
        .auxConnLimit          = 1,
        .connectTimeoutSeconds = 5,
        .requestTimeoutSeconds = 10,
        .idleTimeoutSeconds    = 5,
        .maxReconnectAttempts  = 1,
        .tlsConfig             = WFX::EpTlsInsecure,
    }
};

// -----------------------------------------------------------------------
// WFX::SmtpEndpoint against the hostile SMTP mock (smtp_upstream.py). One instance per
// persona, port and name both compiled in here and mirrored in endpoint_audit.py's
// SMTP_PERSONAS table, keep them in sync. Credentials match smtp_upstream.py's
// _check_auth defaults.
//
// Two config budgets: SmtpCfg() for personas that fail (or succeed) immediately, SmtpCfgFast()
// for the ones that never answer on their own (flood/huge-line/slow-trickle/silent-hang), so
// those phases cost a few seconds each instead of the full default timeout.
// -----------------------------------------------------------------------
static WFX::SmtpEndpointConfig SmtpCfg(std::uint16_t connectTimeout = 8, std::uint16_t requestTimeout = 8) noexcept
{
    return WFX::SmtpEndpointConfig{
        .connLimit             = 4,
        .connectTimeoutSeconds = connectTimeout,
        .requestTimeoutSeconds = requestTimeout,
        .maxReconnectAttempts  = 0, // every call here is client-waited, background retry never applies
        .username              = "audituser",
        .password              = "audit-pass-123",
        .heloName               = "endpoint-audit.wfx.test",
    };
}
static WFX::SmtpEndpointConfig SmtpCfgFast() noexcept
{
    // 5, not lower: EndpointConfig.connectTimeoutSeconds/requestTimeoutSeconds must be >=
    // INVOKE_TIMEOUT_COOLDOWN (5, the timeout timer's own tick period) or the engine refuses
    // to boot (see epoll_connection.cpp's BuildEndpoint-time Fatal check)
    return SmtpCfg(5, 5);
}

inline const auto Smtp_good               = WFX::SmtpEndpoint{"127.0.0.1:8100", SmtpCfg()};
// Dedicated pool for /smtp/inject, same mock persona/port as Smtp_good but never touched by
// /smtp/send. /smtp/send's "good" transactions never call Quit(), so a successful one leaves its
// connection pooled and still alive (ReturnEndpointToPool); Begin() has no way to tell a pooled
// alive slot from a genuinely fresh one, so phase_smtp_inject could silently inherit a connection
// left over from phase_smtp_handshake's earlier /smtp/send "good" calls instead of a clean one
inline const auto Smtp_good_injectroute   = WFX::SmtpEndpoint{"127.0.0.1:8100", SmtpCfg()};
inline const auto Smtp_auth_login_only    = WFX::SmtpEndpoint{"127.0.0.1:8101", SmtpCfg()};
inline const auto Smtp_inject             = WFX::SmtpEndpoint{"127.0.0.1:8103", SmtpCfg()};
inline const auto Smtp_no_starttls        = WFX::SmtpEndpoint{"127.0.0.1:8102", SmtpCfg()};
inline const auto Smtp_selfsigned         = WFX::SmtpEndpoint{"127.0.0.1:8104", SmtpCfg()};
inline const auto Smtp_wronghost          = WFX::SmtpEndpoint{"127.0.0.1:8105", SmtpCfg()};
inline const auto Smtp_expired            = WFX::SmtpEndpoint{"127.0.0.1:8106", SmtpCfg()};
inline const auto Smtp_auth_fail          = WFX::SmtpEndpoint{"127.0.0.1:8107", SmtpCfg()};
inline const auto Smtp_no_auth_mechs      = WFX::SmtpEndpoint{"127.0.0.1:8108", SmtpCfg()};
inline const auto Smtp_mismatched_code    = WFX::SmtpEndpoint{"127.0.0.1:8109", SmtpCfg()};
inline const auto Smtp_malformed_greeting = WFX::SmtpEndpoint{"127.0.0.1:8116", SmtpCfg()};
inline const auto Smtp_drop_greeting      = WFX::SmtpEndpoint{"127.0.0.1:8117", SmtpCfg()};
inline const auto Smtp_drop_pre_handshake = WFX::SmtpEndpoint{"127.0.0.1:8118", SmtpCfg()};
inline const auto Smtp_drop_starttls      = WFX::SmtpEndpoint{"127.0.0.1:8119", SmtpCfg()};
inline const auto Smtp_drop_auth          = WFX::SmtpEndpoint{"127.0.0.1:8120", SmtpCfg()};
inline const auto Smtp_drop_data_prompt   = WFX::SmtpEndpoint{"127.0.0.1:8121", SmtpCfg()};

inline const auto Smtp_flood_greeting     = WFX::SmtpEndpoint{"127.0.0.1:8110", SmtpCfgFast()};
inline const auto Smtp_flood_ehlo2        = WFX::SmtpEndpoint{"127.0.0.1:8111", SmtpCfgFast()};
inline const auto Smtp_huge_line_greeting = WFX::SmtpEndpoint{"127.0.0.1:8112", SmtpCfgFast()};
inline const auto Smtp_huge_line_ehlo2    = WFX::SmtpEndpoint{"127.0.0.1:8113", SmtpCfgFast()};
inline const auto Smtp_slow_trickle       = WFX::SmtpEndpoint{"127.0.0.1:8114", SmtpCfgFast()};
inline const auto Smtp_silent_data        = WFX::SmtpEndpoint{"127.0.0.1:8115", SmtpCfgFast()};

// heloName itself carries a CRLF-injection attempt. Points at the 'good' mock port, but the
// connection is never actually opened, SmtpOnConnect's HasInjectionBytes(opts->heloName) check
// fails before the first byte is sent (see smtp.hpp). Not part of SMTP_PERSONAS: no mock
// listener needs to exist for this one, the wire is never touched
inline const auto Smtp_heloinject = WFX::SmtpEndpoint{"127.0.0.1:8100", WFX::SmtpEndpointConfig{
    .connLimit             = 1,
    .connectTimeoutSeconds = 5,
    .requestTimeoutSeconds = 5,
    .maxReconnectAttempts  = 0,
    .username              = "audituser",
    .password              = "audit-pass-123",
    .heloName              = "evil\r\nMAIL FROM:<hacked@evil>",
}};

static const WFX::SmtpEndpoint* SmtpEndpointOf(std::string_view e) noexcept
{
    if(e == "auth_login_only")    return &Smtp_auth_login_only;
    if(e == "inject")             return &Smtp_inject;
    if(e == "no_starttls")        return &Smtp_no_starttls;
    if(e == "selfsigned")         return &Smtp_selfsigned;
    if(e == "wronghost")          return &Smtp_wronghost;
    if(e == "expired")            return &Smtp_expired;
    if(e == "auth_fail")          return &Smtp_auth_fail;
    if(e == "no_auth_mechs")      return &Smtp_no_auth_mechs;
    if(e == "mismatched_code")    return &Smtp_mismatched_code;
    if(e == "malformed_greeting") return &Smtp_malformed_greeting;
    if(e == "drop_greeting")      return &Smtp_drop_greeting;
    if(e == "drop_pre_handshake") return &Smtp_drop_pre_handshake;
    if(e == "drop_starttls")      return &Smtp_drop_starttls;
    if(e == "drop_auth")          return &Smtp_drop_auth;
    if(e == "drop_data_prompt")   return &Smtp_drop_data_prompt;
    if(e == "flood_greeting")     return &Smtp_flood_greeting;
    if(e == "flood_ehlo2")        return &Smtp_flood_ehlo2;
    if(e == "huge_line_greeting") return &Smtp_huge_line_greeting;
    if(e == "huge_line_ehlo2")    return &Smtp_huge_line_ehlo2;
    if(e == "slow_trickle")       return &Smtp_slow_trickle;
    if(e == "silent_data")        return &Smtp_silent_data;
    if(e == "heloinject")         return &Smtp_heloinject;
    return &Smtp_good;
}

// Result reflection

template <typename OutT>
static void Emit(WFX::Request& req, WFX::Response& res, WFX::Shared::EndpointStatus st, OutT& out)
{
    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("ep", static_cast<std::uint64_t>(static_cast<unsigned>(st)));

    if(st == WFX::EpOk) {
        j.Write("status", static_cast<std::uint64_t>(out->status));
        j.Write("bodylen", static_cast<std::uint64_t>(out->body.size()));
        j.Write("body", std::string_view{out->body.data(), out->body.size()});

        std::string_view want;
        if(req.GetHeader("X-Want", want)) {
            std::string_view hv;
            j.Write("hdr", out->GetHeader(want, hv) ? hv : std::string_view{});
        }
    }
}

// EndpointStatus -> the plain int JsonWriter can actually take an overload for
static std::uint64_t EpJ(WFX::Shared::EndpointStatus s) noexcept
{
    return static_cast<std::uint64_t>(static_cast<unsigned>(s));
}

// Small unsigned header values (counts, sizes). Header-driven like every other
// knob in this app, so parsing lives in one place instead of per route.
static std::uint32_t HeaderU32(WFX::Request& req, const char* name, std::uint32_t fallback) noexcept
{
    std::string_view sv;
    if(!req.GetHeader(name, sv) || sv.empty())
        return fallback;

    std::uint32_t v = 0;
    for(char c : sv) {
        if(c < '0' || c > '9')
            return fallback;
        v = v * 10 + static_cast<std::uint32_t>(c - '0');
    }
    return v;
}

// Map the X-Method header to the enum; unknown -> GET.
static WFX::HttpMethod MethodOf(std::string_view m) noexcept
{
    if(m == "HEAD")    return WFX::HttpMethod::HEAD;
    if(m == "OPTIONS") return WFX::HttpMethod::OPTIONS;
    if(m == "DELETE")  return WFX::HttpMethod::DELETE;
    if(m == "POST")    return WFX::HttpMethod::POST;
    if(m == "PUT")     return WFX::HttpMethod::PUT;
    if(m == "PATCH")   return WFX::HttpMethod::PATCH;
    return WFX::HttpMethod::GET;
}

static const WFX::HttpEndpoint* EndpointOf(std::string_view e) noexcept
{
    if(e == "small")    return &EpSmall;
    if(e == "fast")     return &EpFast;
    if(e == "coalesce") return &EpCoalesce;
    if(e == "reuse")    return &EpReuse;
    if(e == "dead")     return &EpDead;
    if(e == "unreach")  return &EpUnreach;
    if(e == "idle")     return &EpIdle;
    if(e == "prewarm")  return &EpPrewarm;
    return &EpDefault;
}

// Routes
WFX_GET("/health", [](WFX::Request, WFX::Response res) { res.Status(200).SendText("ok"); })

// The one generic proxy route. All outbound behaviour is driven by X-* headers
// (see the file header). Returns the reflected JSON described above.
WFX_GET("/call", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view epName = "default", method = "GET", path = "/ok";
    req.GetHeader("X-Ep", epName);
    req.GetHeader("X-Method", method);
    req.GetHeader("X-Path", path);

    const WFX::HttpEndpoint* ep = EndpointOf(epName);
    WFX::HttpMethod m = MethodOf(method);

    // Up to three forwarded headers ("Name: Value") via X-Fwd / X-Fwd2 / X-Fwd3,
    // added in that order so the harness can assert header ordering and that a
    // forged Host/CL/TE is dropped even when surrounded by clean headers. The
    // views point into the inbound request buffer, which outlives this
    // coroutine's co_await, so it is safe
    WFX::HttpEndpointRequestHeaders hdrs;
    auto addFwd = [&](std::string_view hdrName) {
        std::string_view fwd;
        if(req.GetHeader(hdrName, fwd)) {
            auto colon = fwd.find(':');
            if(colon != std::string_view::npos) {
                std::string_view name = fwd.substr(0, colon);
                std::string_view val = fwd.substr(colon + 1);
                while(!val.empty() && val.front() == ' ')
                    val.remove_prefix(1);
                hdrs.Add(name, val);
            }
        }
    };
    addFwd("X-Fwd");
    addFwd("X-Fwd2");
    addFwd("X-Fwd3");

    std::string_view body;
    req.GetHeader("X-Body", body);

    // One co_await per method family; Emit reflects the outcome regardless of path.
    switch(m) {
        case WFX::HttpMethod::HEAD: {
            auto pr = co_await ep->Head(path, std::move(hdrs));
            Emit(req, res, pr.first, pr.second);
            break;
        }
        case WFX::HttpMethod::OPTIONS: {
            auto pr = co_await ep->Options(path, std::move(hdrs));
            Emit(req, res, pr.first, pr.second);
            break;
        }
        case WFX::HttpMethod::DELETE: {
            auto pr = co_await ep->Delete(path, std::move(hdrs));
            Emit(req, res, pr.first, pr.second);
            break;
        }
        case WFX::HttpMethod::POST: {
            auto pr = co_await ep->Post(path, body, std::move(hdrs));
            Emit(req, res, pr.first, pr.second);
            break;
        }
        case WFX::HttpMethod::PUT: {
            auto pr = co_await ep->Put(path, body, std::move(hdrs));
            Emit(req, res, pr.first, pr.second);
            break;
        }
        case WFX::HttpMethod::PATCH: {
            auto pr = co_await ep->Patch(path, body, std::move(hdrs));
            Emit(req, res, pr.first, pr.second);
            break;
        }
        default: {
            auto pr = co_await ep->Get(path, std::move(hdrs));
            Emit(req, res, pr.first, pr.second);
            break;
        }
    }

    co_return;
})

// Serialize-side injection probe. The inbound POST *body* carries raw bytes that
// may contain CR/LF/NUL, bytes the inbound header parser would never allow, so
// this is the only way to feed the client serializer a genuinely hostile path or
// header. The client MUST refuse (EpSerializeError == 10), never emit a request
// that smuggles a second header/line upstream.
//
//   X-Ep      which endpoint (default)
//   X-Inject  "path"   -> request target is the raw body
//             "header" -> body is "Name:Value" (value may hold CR/LF/NUL), added as a header
//   body      the raw injection payload
WFX_POST("/inject", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view epName = "default", mode = "path";
    req.GetHeader("X-Ep", epName);
    req.GetHeader("X-Inject", mode);
    const WFX::HttpEndpoint* ep = EndpointOf(epName);

    std::string_view raw = req.Body(); // opaque bytes, may contain CR/LF/NUL

    WFX::HttpEndpointRequest r{};
    r.method = WFX::HttpMethod::GET;

    if(mode == "header") {
        r.path = "/ok";
        auto colon = raw.find(':');
        std::string_view name = colon == std::string_view::npos ? raw : raw.substr(0, colon);
        std::string_view val = colon == std::string_view::npos ? std::string_view{} : raw.substr(colon + 1);
        r.headers.Add(name, val);
    }
    else {
        r.path = raw; // hostile path straight into the request line
    }

    auto pr = co_await ep->Send(std::move(r));
    Emit(req, res, pr.first, pr.second);
    co_return;
})

// WFX::SmtpEndpoint, driven end to end: MAIL FROM -> RCPT TO -> DATA -> body, all on one
// Reserve()'d connection. Stops at the first stage that fails, transport or protocol level.
//
//   X-Persona  which SmtpEndpointOf() persona (default "good")
//   X-From / X-FromName / X-To / X-ToName / X-Subject / X-ReplyTo   (all optional, sane defaults)
//   body       the message body (POST body, not a header, so it can carry raw CR/LF for
//              dot-stuffing round-trip tests)
//
//   { "ep": <EndpointStatus int>, "stage": "reserve"|"mail"|"rcpt"|"data_start"|"data_body"|"done",
//     "code": <SMTP reply code>, "success": <bool> }        // code/success only when ep == 0
WFX_POST("/smtp/send", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view personaName = "good";
    req.GetHeader("X-Persona", personaName);

    std::string_view fromAddr = "sender@wfx.test", fromName{}, toAddr = "recipient@wfx.test",
                     toName{}, subject = "audit", replyTo{};
    req.GetHeader("X-From", fromAddr);
    req.GetHeader("X-FromName", fromName);
    req.GetHeader("X-To", toAddr);
    req.GetHeader("X-ToName", toName);
    req.GetHeader("X-Subject", subject);
    req.GetHeader("X-ReplyTo", replyTo);
    std::string_view body = req.Body();

    const WFX::SmtpEndpoint* ep = SmtpEndpointOf(personaName);
    auto tx = ep->Begin();

    res.Status(200);
    auto j = WFX::ImJson(res);

    if(!tx.IsValid()) {
        j.Write("ep", EpJ(WFX::EpPoolExhausted));
        j.Write("stage", std::string_view{"reserve"});
        co_return;
    }

    auto [s1, r1] = co_await tx.MailFrom(fromAddr);
    if(s1 != WFX::EpOk) {
        j.Write("ep", EpJ(s1));
        j.Write("stage", std::string_view{"mail"});
        co_return;
    }
    j.Write("code", r1->code);
    j.Write("success", r1->Success());
    if(!r1->Success()) {
        j.Write("ep", EpJ(WFX::EpOk));
        j.Write("stage", std::string_view{"mail"});
        co_return;
    }

    auto [s2, r2] = co_await tx.RcptTo(toAddr);
    if(s2 != WFX::EpOk) {
        j.Write("ep", EpJ(s2));
        j.Write("stage", std::string_view{"rcpt"});
        co_return;
    }
    j.Write("code", r2->code);
    j.Write("success", r2->Success());
    if(!r2->Success()) {
        j.Write("ep", EpJ(WFX::EpOk));
        j.Write("stage", std::string_view{"rcpt"});
        co_return;
    }

    auto [s3, r3] = co_await tx.DataStart();
    if(s3 != WFX::EpOk) {
        j.Write("ep", EpJ(s3));
        j.Write("stage", std::string_view{"data_start"});
        co_return;
    }
    j.Write("code", r3->code);
    j.Write("success", r3->Continue());
    if(!r3->Continue()) {
        j.Write("ep", EpJ(WFX::EpOk));
        j.Write("stage", std::string_view{"data_start"});
        co_return;
    }

    auto [s4, r4] = co_await tx.DataBody(fromAddr, fromName, toAddr, toName, subject, body, replyTo);
    if(s4 != WFX::EpOk) {
        j.Write("ep", EpJ(s4));
        j.Write("stage", std::string_view{"data_body"});
        co_return;
    }
    j.Write("ep", EpJ(WFX::EpOk));
    j.Write("code", r4->code);
    j.Write("success", r4->Success());
    j.Write("stage", std::string_view{r4->Success() ? "done" : "data_body"});
    co_return;
})

// Serialize-side injection probe for the SMTP client, mirrors HTTP's /inject: the injection
// screen (Smtp::Detail::HasInjectionBytes) only rejects CR/LF/NUL bytes that an HTTP *header*
// could never carry in the first place, so the hostile payload travels as the POST body and
// this route splices it into whichever field X-Field names. Always against the 'good' persona;
// fields ahead of the one under test get clean placeholder values so the run reaches it.
//
//   X-Field  mailfrom | rcptto | fromname | toname | subject | replyto | body   (default mailfrom)
//   body     the hostile payload (may contain CR/LF/NUL)
//
//   { "ep": <EndpointStatus int>, "stage": "mail"|"rcpt"|"data_start"|"data_body",
//     "code": <SMTP reply code>, "success": <bool> }   // code/success only on a clean-step stage;
//                                                       // EpSerializeError (11) is the correct
//                                                       // refusal on the field-under-test's stage
WFX_POST("/smtp/inject", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view field = "mailfrom";
    req.GetHeader("X-Field", field);
    std::string_view raw = req.Body();

    auto tx = Smtp_good_injectroute.Begin();

    res.Status(200);
    auto j = WFX::ImJson(res);
    if(!tx.IsValid()) {
        j.Write("ep", EpJ(WFX::EpPoolExhausted));
        j.Write("stage", std::string_view{"reserve"});
        co_return;
    }

    std::string_view mailFrom = (field == "mailfrom") ? raw : std::string_view{"sender@wfx.test"};
    auto [s1, r1] = co_await tx.MailFrom(mailFrom);
    if(field == "mailfrom") {
        j.Write("ep", EpJ(s1));
        j.Write("stage", std::string_view{"mail"});
        co_return;
    }
    if(s1 != WFX::EpOk) {
        j.Write("ep", EpJ(s1));
        j.Write("stage", std::string_view{"mail"});
        co_return;
    }
    if(!r1->Success()) {
        j.Write("ep", EpJ(WFX::EpOk));
        j.Write("stage", std::string_view{"mail"});
        j.Write("code", r1->code);
        j.Write("success", false);
        co_return;
    }

    std::string_view rcptTo = (field == "rcptto") ? raw : std::string_view{"recipient@wfx.test"};
    auto [s2, r2] = co_await tx.RcptTo(rcptTo);
    if(field == "rcptto") {
        j.Write("ep", EpJ(s2));
        j.Write("stage", std::string_view{"rcpt"});
        co_return;
    }
    if(s2 != WFX::EpOk) {
        j.Write("ep", EpJ(s2));
        j.Write("stage", std::string_view{"rcpt"});
        co_return;
    }
    if(!r2->Success()) {
        j.Write("ep", EpJ(WFX::EpOk));
        j.Write("stage", std::string_view{"rcpt"});
        j.Write("code", r2->code);
        j.Write("success", false);
        co_return;
    }

    auto [s3, r3] = co_await tx.DataStart();
    if(s3 != WFX::EpOk) {
        j.Write("ep", EpJ(s3));
        j.Write("stage", std::string_view{"data_start"});
        co_return;
    }
    if(!r3->Continue()) {
        j.Write("ep", EpJ(WFX::EpOk));
        j.Write("stage", std::string_view{"data_start"});
        j.Write("code", r3->code);
        j.Write("success", false);
        co_return;
    }

    std::string_view fromName = (field == "fromname") ? raw : std::string_view{};
    std::string_view toName   = (field == "toname")   ? raw : std::string_view{};
    std::string_view subject  = (field == "subject")  ? raw : std::string_view{"audit"};
    std::string_view replyTo  = (field == "replyto")  ? raw : std::string_view{};
    std::string_view body     = (field == "body")     ? raw : std::string_view{"hello"};

    auto [s4, r4] = co_await tx.DataBody("sender@wfx.test", fromName, "recipient@wfx.test", toName,
                                         subject, body, replyTo);
    j.Write("ep", EpJ(s4));
    j.Write("stage", std::string_view{"data_body"});
    if(s4 == WFX::EpOk) {
        j.Write("code", r4->code);
        j.Write("success", r4->Success());
    }
    co_return;
})

// See "Raw-protocol endpoint" above: onConnect / onDisconnect / multiplexing
// against the raw WFX::Endpoint<> primitive, HttpEndpoint can't reach any of it.
WFX_GET("/proto/call", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view name = "good", key = "hello";
    req.GetHeader("X-Proto", name);
    req.GetHeader("X-Key", key);

    const ProtoEp* ep = ProtoEndpointOf(name);
    auto [status, out] = co_await ep->SendPayload(ProtoReq{WFX::String(key.data(), key.size())});

    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("ep", static_cast<std::uint64_t>(static_cast<unsigned>(status)));
    if(status == WFX::EpOk)
        j.Write("value", std::string_view{out->value});

    co_return;
})

WFX_GET("/proto/disconnects", [](WFX::Request, WFX::Response res) {
    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("idle", g_idleDisconnects);
    j.Write("handshake", g_handshakeTimeouts);
    j.Write("error", g_errorDisconnects);
})

WFX_POST("/proto/disconnects/reset", [](WFX::Request, WFX::Response res) {
    g_idleDisconnects = 0;
    g_handshakeTimeouts = 0;
    g_errorDisconnects = 0;
    res.Status(200).SendText("ok");
})

// --- SP routes: pinning, streaming, push, TLS upgrade, onAbort -------------
// Plain single request. X-Sp picks the endpoint instance
// (good/downgrade/tlsgarbage/abort/abortnoaux/abortmidconnect/abortbadclose).
// X-Leak=1 tells onAbort (on the abort* endpoints) to skip side.Close()
WFX_GET("/sp/get", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view name = "good", key = "hello";
    req.GetHeader("X-Sp", name);
    req.GetHeader("X-Key", key);

    const bool leak = HeaderU32(req, "X-Leak", 0) != 0;
    SpReq sreq{SpMode::GET, WFX::String(key.data(), key.size()), 0, 0, 0, 0, leak};

    WFX::Shared::EndpointStatus status{};
    WFX::EndpointOutput<SpRes> out;

    if(name == "abort")
        std::tie(status, out) = co_await SpAbort.SendPayload(sreq);
    else if(name == "abortnoaux")
        std::tie(status, out) = co_await SpAbortNoAux.SendPayload(sreq);
    else if(name == "abortmidconnect")
        std::tie(status, out) = co_await SpAbortMidConnect.SendPayload(sreq);
    else if(name == "abortbadclose")
        std::tie(status, out) = co_await SpAbortBadClose.SendPayload(sreq);
    else
        std::tie(status, out) = co_await SpEndpointOf(name)->SendPayload(sreq);

    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("ep", static_cast<std::uint64_t>(static_cast<unsigned>(status)));
    if(status == WFX::EpOk) {
        j.Write("value", std::string_view{out->value});
        j.Write("conn", out->connId);
    }

    co_return;
})

// Drains a stream to completion, accumulating ONLY counters. Nothing here keeps
// a chunk alive past its iteration, so if peak RSS grows with X-Count then the
// engine is buffering, not the app. X-Mode server -> CHUNK_READY,
// fetch -> CHUNK_READY_FETCH. X-Stop > 0 abandons the stream after N chunks.
// X-Sp="abort" plus X-StallAfter/X-StallMs lets the audit abandon the CLIENT
// mid-stream (after isStreaming is already set) and prove onAbort's scope cut:
// a streaming request still force-closes on client disconnect, never fires onAbort
WFX_GET("/sp/stream", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view name = "good", mode = "server";
    req.GetHeader("X-Sp", name);
    req.GetHeader("X-Mode", mode);

    const std::uint32_t count = HeaderU32(req, "X-Count", 10);
    const std::uint32_t stop = HeaderU32(req, "X-Stop", 0);
    const std::uint32_t stallAfter = HeaderU32(req, "X-StallAfter", 0);
    const std::uint32_t stallMs = HeaderU32(req, "X-StallMs", 0);

    SpReq sreq{mode == "fetch" ? SpMode::STREAM_FETCH : SpMode::STREAM_SERVER, {}, count,
              HeaderU32(req, "X-Size", 64), stallAfter, stallMs, false};

    auto stream = (name == "abort") ? SpAbort.Stream(sreq) : SpGood.Stream(sreq);

    std::uint64_t chunks = 0, bytes = 0, checksum = 0, conn = 0;
    auto epStatus = WFX::EpOk;
    bool done = false;

    while(true) {
        auto chunk = co_await stream.Next();

        if(chunk.status != WFX::EpOk) {
            epStatus = chunk.status;
            break;
        }
        if(chunk.done) {
            done = true;
            break;
        }
        if(!chunk.data)
            break;

        chunks++;
        bytes += chunk.data->value.size();
        conn = chunk.data->connId;

        // Order-sensitive fold over every chunk: proves the harness got the same
        // bytes in the same order, so a dropped or reordered chunk is detectable
        checksum = WFX::WyHash(std::string_view{chunk.data->value.data(), chunk.data->value.size()}, checksum);

        // Abandon mid-stream: the slot must still be reclaimed, not stranded
        if(stop != 0 && chunks >= stop)
            break;
    }

    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("ep", static_cast<std::uint64_t>(static_cast<unsigned>(epStatus)));
    j.Write("chunks", chunks);
    j.Write("bytes", bytes);
    j.Write("checksum", checksum);
    j.Write("conn", conn);
    j.Write("done", static_cast<std::uint64_t>(done ? 1 : 0));

    co_return;
})

// Reserves a connection, runs X-N requests on it, reports the connId seen each
// time. All must match: that is the isolation guarantee pinning exists to give.
// X-Release early|late|double controls the release pattern.
WFX_GET("/sp/reserve", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view releaseMode = "late";
    req.GetHeader("X-Release", releaseMode);

    const std::uint32_t n = HeaderU32(req, "X-N", 3);

    auto slot = SpGood.Reserve();

    res.Status(200);
    auto j = WFX::ImJson(res);

    if(!slot.IsValid()) {
        j.Write("reserved", static_cast<std::uint64_t>(0));
        co_return;
    }

    j.Write("reserved", static_cast<std::uint64_t>(1));

    std::uint64_t firstConn = 0, sameConn = 1, lastStatus = 0;
    for(std::uint32_t i = 0; i < n; i++) {
        auto [status, out] = co_await slot.SendPayload(SpReq{SpMode::GET, "pinned", 0, 0});
        lastStatus = static_cast<std::uint64_t>(static_cast<unsigned>(status));

        if(status != WFX::EpOk)
            break;

        if(firstConn == 0)
            firstConn = out->connId;
        else if(out->connId != firstConn)
            sameConn = 0;
    }

    j.Write("conn", firstConn);
    j.Write("same", sameConn);
    j.Write("last", lastStatus);

    // Releasing twice must be harmless; the handle clears on the first call
    if(releaseMode == "double") {
        slot.Release();
        slot.Release();
    }
    else if(releaseMode == "early")
        slot.Release();

    // "late" leaves it to the destructor, the path an early co_return would take

    co_return;
})

// Two independent reservations must land on two different physical connections
// and must never be coalesced into one backend round trip, even byte-identical.
WFX_GET("/sp/reserve/pair", [](WFX::Request, WFX::Response res) -> WFX::Coro {
    auto a = SpGood.Reserve();
    auto b = SpGood.Reserve();

    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("a_ok", static_cast<std::uint64_t>(a.IsValid() ? 1 : 0));
    j.Write("b_ok", static_cast<std::uint64_t>(b.IsValid() ? 1 : 0));

    if(!a.IsValid() || !b.IsValid())
        co_return;

    auto [sa, oa] = co_await a.SendPayload(SpReq{SpMode::GET, "same", 0, 0});
    auto [sb, ob] = co_await b.SendPayload(SpReq{SpMode::GET, "same", 0, 0});

    j.Write("sa", static_cast<std::uint64_t>(static_cast<unsigned>(sa)));
    j.Write("sb", static_cast<std::uint64_t>(static_cast<unsigned>(sb)));

    if(sa == WFX::EpOk && sb == WFX::EpOk) {
        j.Write("conn_a", oa->connId);
        j.Write("conn_b", ob->connId);
        j.Write("distinct", static_cast<std::uint64_t>(oa->connId != ob->connId ? 1 : 0));
    }

    co_return;
})

// Streaming through a pinned connection: the two features must compose, and
// every chunk must come from the reserved slot rather than a pooled one.
WFX_GET("/sp/reserve/stream", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    const std::uint32_t count = HeaderU32(req, "X-Count", 8);

    auto slot = SpGood.Reserve();

    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("reserved", static_cast<std::uint64_t>(slot.IsValid() ? 1 : 0));

    if(!slot.IsValid())
        co_return;

    auto stream = slot.Stream(SpReq{SpMode::STREAM_SERVER, {}, count, 32});

    std::uint64_t chunks = 0, conn = 0, sameConn = 1;
    while(true) {
        auto chunk = co_await stream.Next();
        if(chunk.status != WFX::EpOk || chunk.done || !chunk.data)
            break;

        chunks++;
        if(conn == 0)
            conn = chunk.data->connId;
        else if(chunk.data->connId != conn)
            sameConn = 0;
    }

    j.Write("chunks", chunks);
    j.Write("conn", conn);
    j.Write("same", sameConn);

    co_return;
})

WFX_GET("/sp/push", [](WFX::Request, WFX::Response res) {
    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("count", g_spPushCount);
    j.Write("bytes", g_spPushBytes);
    j.Write("rejects", g_spPushRejects);
    j.Write("disconnects", g_spDisconnects);
})

WFX_POST("/sp/push/reset", [](WFX::Request, WFX::Response res) {
    g_spPushCount = 0;
    g_spPushBytes = 0;
    g_spPushRejects = 0;
    g_spDisconnects = 0;
    res.Status(200).SendText("ok");
})

// The app's own view of onAbort activity, independent of what the mock recorded on
// the wire: separates "onAbort never ran" from "it ran but OpenSideConnection failed"
WFX_GET("/sp/abort", [](WFX::Request, WFX::Response res) {
    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("runs", g_spAbortRuns);
    j.Write("side_opens", g_spAbortSideOpens);
    j.Write("side_failures", g_spAbortSideFailures);
})

WFX_POST("/sp/abort/reset", [](WFX::Request, WFX::Response res) {
    g_spAbortRuns = 0;
    g_spAbortSideOpens = 0;
    g_spAbortSideFailures = 0;
    res.Status(200).SendText("ok");
})

// Worker memory, via the telemetry API every user already gets for free. The
// harness samples this before and after a large stream: if peak memory tracks
// X-Count rather than X-Size, the engine is accumulating chunks instead of
// reusing one output object.
WFX_GET("/sp/rss", [](WFX::Request, WFX::Response res) {
    const auto self = WFX::GetProcessMetricsAt(WFX::WorkerIndex());

    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("rss", self.rssBytes);
    j.Write("vm", self.vmBytes);
    j.Write("pid", static_cast<std::uint64_t>(self.pid));
})

// Per-endpoint metrics, summed across workers, each tagged with its host. Several endpoint
// instances share the same host (all the UPSTREAM ones), so the metrics phase asserts on the
// aggregate delta across every slot rather than trying to map a slot back to one instance.
// ev.host is a Shared::StringView, written directly via the JsonWriter StringView overload
WFX_GET("/metrics", [](WFX::Request, WFX::Response res) {
    const bool latencyOn = WFX::MetricsLatencyEnabled();

    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("latency_enabled", latencyOn);

    j.Arr("endpoints");
    for(std::uint16_t e = 0; e < WFX::EndpointMetricCount(); e++) {
        const auto ev = WFX::GetEndpointMetricsAt(e);
        j.Obj();
        j.Write("host", ev.host);
        j.Write("requests", ev.metrics.requests);
        j.Write("completed", ev.metrics.completed);
        j.Write("status_1xx", ev.metrics.status1xx);
        j.Write("status_2xx", ev.metrics.status2xx);
        j.Write("status_3xx", ev.metrics.status3xx);
        j.Write("status_4xx", ev.metrics.status4xx);
        j.Write("status_5xx", ev.metrics.status5xx);
        j.Write("connect_failures", ev.metrics.connectFailures);
        j.Write("tls_failures", ev.metrics.tlsFailures);
        j.Write("request_timeouts", ev.metrics.requestTimeouts);
        j.Write("pool_exhausted", ev.metrics.poolExhausted);
        j.Write("other_errors", ev.metrics.otherErrors);
        j.Write("reconnects", ev.metrics.reconnects);
        j.Write("coalesce_hits", ev.metrics.coalesceHits);
        j.Write("bytes_out", ev.metrics.bytesOut);
        j.Write("bytes_in", ev.metrics.bytesIn);
        j.Write("slots_in_use", ev.metrics.slotsInUse);

        if(latencyOn) {
            const auto st = WFX::ComputeLatencyStats(WFX::GetEndpointLatencyAt(e));
            j.Obj("latency");
            j.Write("count", st.count);
            j.Write("mean_us", static_cast<std::uint64_t>(st.meanUs));
            j.Write("p50_us", st.p50Us);
            j.Write("p99_us", st.p99Us);
            j.Write("max_us", st.maxUs);
            j.End();
        }
        j.End();
    }
    j.End();
})
