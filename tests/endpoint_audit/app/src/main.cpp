// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits
//
// Audit target for the raw WFX::Endpoint<> primitive.
//
// Every route turns an inbound request into an outbound call on a hand-rolled
// protocol and reflects the outcome back as JSON the harness asserts on. Two
// protocols are needed, because half the primitive is unreachable through a
// multiplexed endpoint:
//
//   mux   sets hasCapacity, which is what the engine reads to pick the
//         multiplexed receive loop. Drives onConnect, onDisconnect, and several
//         requests sharing one physical connection
//   solo  leaves hasCapacity null. Reserve is refused outright on a multiplexed
//         endpoint (INVALID_KEY), the multiplexed receive loop has no chunk case
//         so Stream fails the slot there, and onAbort never runs for a
//         multiplexed request (the abandoned stream is dropped and the shared
//         connection carries on). onPush and UpgradeToTLS do reach both, and are
//         driven here alongside the rest
//
// Both are spoken by primitive_upstream.py, which mirrors the wire formats
// written above each section here. The shipped protocol clients built on this
// primitive are audited separately, in tests/client_audit.
//
// Routes:
//   GET  /health                  liveness
//   GET  /mux/call                one call on a mux instance
//   GET  /mux/disconnects         onDisconnect counters, by reason
//   POST /mux/disconnects/reset   zero those counters
//   GET  /solo/get                one call on a solo instance
//   GET  /solo/stream             drain a stream, report counters only
//   GET  /solo/reserve            pin a connection, run N requests on it
//   GET  /solo/reserve/pair       two live reservations at once
//   GET  /solo/reserve/stream     stream through a pinned connection
//   GET  /solo/push               onPush counters
//   POST /solo/push/reset         zero those counters
//   GET  /solo/abort              onAbort counters
//   POST /solo/abort/reset        zero those counters
//   GET  /rss                     worker resident memory
//   GET  /metrics                 per-endpoint metrics table

#include <wfx/http.hpp>
#include <wfx/memory.hpp>
#include <wfx/telemetry.hpp>
#include <wfx/utils/hash.hpp>
#include <wfx/endpoint/base.hpp>

#include <cstdint>
#include <cstdio>
#include <string_view>
#include <tuple>
#include <utility>

// An endpoint bakes host:port into the instance, so both upstreams are pinned at
// COMPILE time. The harness must launch primitive_upstream.py on these exact
// ports; they are mirrored in endpoint_audit.py as MUX_PORT and SOLO_PORT.
#define MUX_UPSTREAM  "127.0.0.1:8092"
#define SOLO_UPSTREAM "127.0.0.1:8093"

// Shared helpers
namespace {

// EndpointStatus -> the plain integer JsonWriter has an overload for
std::uint64_t EpJ(WFX::Shared::EndpointStatus status) noexcept
{
    return static_cast<std::uint64_t>(static_cast<unsigned>(status));
}

// Small unsigned header values (counts, sizes). Every knob in this app is
// header-driven, so parsing them lives in one place instead of per route.
std::uint32_t HeaderU32(WFX::Request& req, const char* name, std::uint32_t fallback) noexcept
{
    std::string_view sv;
    if(!req.GetHeader(name, sv) || sv.empty())
        return fallback;

    std::uint32_t value = 0;
    for(char c : sv) {
        if(c < '0' || c > '9')
            return fallback;
        value = value * 10 + static_cast<std::uint32_t>(c - '0');
    }
    return value;
}

} // namespace

// The mux protocol: onConnect, onDisconnect, multiplexing
//
// Line oriented, '\n' terminated, spoken to primitive_upstream.py's mux listener:
//
//   handshake -> "AUTH <token>\n"      <- "OK\n" | "ERR\n" | (drop, no reply)
//   request   -> "REQ <id> <key>\n"    <- "RES <id> <value>\n"
//
// The id is caller-assigned and travels back on the reply, which is what lets
// the mock answer out of order on purpose: a key of "sleep:<secs>:<value>"
// delays its own reply by <secs> and answers with <value>. Matching a reply to
// its caller by id rather than by arrival order is the property under test.
namespace {

// How many requests this protocol lets share one connection
constexpr std::uint32_t MUX_MAX_IN_FLIGHT = 32;

struct MuxReq {
    WFX::String key;
};

struct MuxRes {
    WFX::String value;
};

struct PendingReply {
    std::uint64_t id;
    MuxRes* res; // owned until takeStreamOutput hands it off
};

// One per physical connection. token is which handshake this slot sends,
// injected via userCtx so every instance can share the same onConnect function.
struct MuxSlotState {
    const char* token;
    std::uint64_t nextId = 1;
    std::uint32_t inFlight = 0;
    WFX::Vector<PendingReply> finished;
};

// Single worker (see config/wfx.local.toml, worker_processes = 1), so these are
// only ever touched from the engine's own event-loop thread, no locking needed
std::uint64_t g_muxIdleDisconnects = 0;
std::uint64_t g_muxHandshakeTimeouts = 0;
std::uint64_t g_muxErrorDisconnects = 0;

void* MuxCreateSlotState(void* userCtx)
{
    auto* state = WFX::New<MuxSlotState>();
    state->token = static_cast<const char*>(userCtx);
    return state;
}

void MuxDestroySlotState(void* slotState)
{
    WFX::Delete(static_cast<MuxSlotState*>(slotState));
}

void MuxOnDisconnect(void* /*slotState*/, WFX::DisconnectReason reason)
{
    if(reason == WFX::EpIdleTimeout)
        g_muxIdleDisconnects++;
    else if(reason == WFX::EpHandshakeTimeoutReason)
        g_muxHandshakeTimeouts++;
    else
        g_muxErrorDisconnects++;
}

WFX::EpCoro MuxAuthenticate(WFX::SlotHandle h, void* slotStateVoid)
{
    auto* state = static_cast<MuxSlotState*>(slotStateVoid);

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

WFX::Shared::SerializeResult MuxSerialize(void* slotStateVoid, const void* reqVoid, char* buf,
                                          std::uint32_t bufLen, std::uint32_t* written,
                                          std::uint64_t* streamKey)
{
    auto* state = static_cast<MuxSlotState*>(slotStateVoid);
    auto& req = *static_cast<const MuxReq*>(reqVoid);

    const std::uint64_t id = state->nextId++;
    const int n = std::snprintf(buf, bufLen, "REQ %llu %s\n", static_cast<unsigned long long>(id),
                                req.key.c_str());
    if(n < 0 || static_cast<std::uint32_t>(n) >= bufLen)
        return WFX::EpSerBufferTooSmall;

    *written = static_cast<std::uint32_t>(n);
    *streamKey = id;
    state->inFlight++;
    return WFX::EpSerOk;
}

WFX::Shared::ParseResult MuxParse(void* slotStateVoid, void* /*parseState, unused*/, const char* buf,
                                  std::uint32_t len, std::uint32_t* consumed, void* /*outObj, unused here*/,
                                  bool isEof, std::uint64_t* completedKey)
{
    auto* state = static_cast<MuxSlotState*>(slotStateVoid);
    std::string_view view{buf, len};

    const auto nl = view.find('\n');
    if(nl == std::string_view::npos) {
        *consumed = 0;
        return isEof ? WFX::EpParseError : WFX::EpParseIncomplete;
    }

    std::string_view line = view.substr(0, nl);
    *consumed = static_cast<std::uint32_t>(nl + 1);

    if(!line.starts_with("RES "))
        return WFX::EpParseError;

    line.remove_prefix(4);
    const auto space = line.find(' ');
    if(space == std::string_view::npos)
        return WFX::EpParseError;

    std::uint64_t id = 0;
    for(char c : line.substr(0, space)) {
        if(c < '0' || c > '9')
            return WFX::EpParseError;
        id = id * 10 + static_cast<std::uint64_t>(c - '0');
    }

    auto* res = WFX::New<MuxRes>();
    const auto value = line.substr(space + 1);
    res->value.assign(value.data(), value.size());

    state->finished.push_back({id, res});
    state->inFlight--;
    *completedKey = id;

    // The connection stays open for the other in-flight streams, so this stream
    // finishing does not mean there is nothing left to read
    return WFX::EpParseIncomplete;
}

// Never actually called on the multiplexed path (the engine only calls
// destroyOutput here, to free a finished-but-abandoned or delivered response),
// but createOutput/destroyOutput are validated as a pair at startup regardless
// of hasCapacity, so this has to exist even though it is dead weight at runtime
void* MuxCreateOutput(void*)
{
    return nullptr;
}

void MuxDestroyOutput(void* outputPtr)
{
    WFX::Delete(static_cast<MuxRes*>(outputPtr));
}

// The only cap on how many requests share one connection. EndpointConfig has a
// maxConcurrentStreams field, but nothing in the engine reads it, so a protocol
// that wants a limit has to enforce it here.
bool MuxHasCapacity(void* slotStateVoid)
{
    return static_cast<MuxSlotState*>(slotStateVoid)->inFlight < MUX_MAX_IN_FLIGHT;
}

void* MuxTakeStreamOutput(void* slotStateVoid, std::uint64_t key)
{
    auto* state = static_cast<MuxSlotState*>(slotStateVoid);
    for(auto it = state->finished.begin(); it != state->finished.end(); ++it) {
        if(it->id == key) {
            void* res = it->res;
            state->finished.erase(it);
            return res;
        }
    }

    return nullptr; // not finished yet
}

WFX::EndpointDesc MuxDesc(const char* token)
{
    return WFX::EndpointDesc{
        .serialize        = MuxSerialize,
        .parse            = MuxParse,
        .onDisconnect     = MuxOnDisconnect,
        .createSlotState  = MuxCreateSlotState,
        .destroySlotState = MuxDestroySlotState,
        .createOutput     = MuxCreateOutput,
        .destroyOutput    = MuxDestroyOutput,
        .hasCapacity      = MuxHasCapacity,
        .takeStreamOutput = MuxTakeStreamOutput,
        .userCtx          = const_cast<void*>(static_cast<const void*>(token)),
    };
}

using MuxEp = WFX::Endpoint<MuxReq, MuxRes, &MuxAuthenticate>;

} // namespace

// The handshake succeeds. Concurrent calls end up on one connection because a
// multiplexed send prefers an already-open slot with spare capacity over dialing a
// new one, not because of connLimit; MuxHasCapacity is what decides "spare".
// idleTimeoutSeconds sits at the engine's 5s floor, which is what keeps the idle
// disconnect observable inside a reasonable test window.
inline const MuxEp MuxGood{
    MUX_UPSTREAM, MuxDesc("good"),
    WFX::EndpointConfig{
        .connLimit             = 1,
        .connectTimeoutSeconds = 5,
        .requestTimeoutSeconds = 10,
        .idleTimeoutSeconds    = 5,
        .maxReconnectAttempts  = 1,
        .tlsConfig             = WFX::EpTlsInsecure,
    }
};

// The mock answers ERR, so onConnect returns EpFatal and the request must fail
// cleanly rather than being served over an unauthenticated connection.
inline const MuxEp MuxBad{
    MUX_UPSTREAM, MuxDesc("bad"),
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

// The mock stalls its AUTH reply well past connectTimeoutSeconds (the engine's 5s
// floor), so the connect-phase timeout must surface as EpHandshakeTimeout.
inline const MuxEp MuxSlow{
    MUX_UPSTREAM, MuxDesc("slow"),
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

// The mock drops the connection mid-handshake without replying, so co_await
// h.Receive() fails and onConnect returns EpFatal from its own error path.
inline const MuxEp MuxReset{
    MUX_UPSTREAM, MuxDesc("reset"),
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

const MuxEp* MuxEndpointOf(std::string_view name) noexcept
{
    if(name == "bad")   return &MuxBad;
    if(name == "slow")  return &MuxSlow;
    if(name == "reset") return &MuxReset;
    return &MuxGood;
}

} // namespace

// The solo protocol: pinning, streaming, onPush, onAbort, UpgradeToTLS
//
// Line oriented, '\n' terminated, spoken to primitive_upstream.py's solo listener:
//
//   handshake -> "STARTTLS <token>\n"  <- "S\n" (upgrade) | "N\n" (stay plain)
//              -> "AUTH <token>\n"     <- "OK <connId>\n" | "ERR\n"
//   request    -> "GET <key>\n"        <- "VAL <connId>:<key>\n"
//              -> "STREAM <n> <sz> <stallAfter> <stallMs>\n"
//                                      <- "CHUNK <payload>\n" * n, then "END\n"
//              -> "PAGE <n> <sz>\n"    <- "CHUNK <payload>\n", then one more per
//                                         "MORE\n" the engine re-serializes, "END\n"
//   push       ->                      <- "PUSH <text>\n" at any idle moment
//
// STREAM keeps sending until it runs out, which is the CHUNK_READY path. PAGE
// answers exactly one chunk and then waits, so the engine has to re-serialize a
// "MORE" for every further one, which is the CHUNK_READY_FETCH path. connId comes
// from the mock's accept counter and is stamped on every response built here, so
// which physical connection served a request is observed rather than assumed,
// which is what makes the pinning isolation assertions possible.
namespace {

// Chunk payload size for /solo/reserve/stream, whose assertion is about which
// connection served each chunk rather than about how big a chunk is
constexpr std::uint32_t RESERVE_CHUNK_BYTES = 32;

enum class SoloMode : std::uint8_t { GET, STREAM_SERVER, STREAM_FETCH };

struct SoloReq {
    SoloMode mode = SoloMode::GET;
    WFX::String key;
    std::uint32_t count = 0;
    std::uint32_t size = 0;
    std::uint32_t stallAfter = 0; // STREAM_SERVER only: stall before this chunk index, 0 = never
    std::uint32_t stallMs = 0;    // how long that stall lasts
    bool leakAux = false;         // GET only: onAbort skips side.Close(), so the timeout has to reclaim it
};

// One instance per slot, reused for every chunk of a stream. value is ASSIGNED
// per chunk, never appended to: that reuse is exactly what the engine's
// bounded-memory guarantee rests on, and /rss is what proves it holds.
struct SoloRes {
    WFX::String value;
    std::uint64_t connId = 0;
    bool ended = false;
};

struct SoloSlotState {
    const char* token;
    bool tryTls;
    bool requireTls;
    std::uint64_t connId = 0;
    SoloMode mode = SoloMode::GET;
    std::uint32_t pagesSent = 0;
    bool leakAux = false; // stashed by SoloSerialize, read by SoloOnAbort
};

// Single worker (config/wfx.local.toml, worker_processes = 1), so plain globals
// are fine here too
std::uint64_t g_soloPushCount = 0;
std::uint64_t g_soloPushBytes = 0;
std::uint64_t g_soloPushRejects = 0;
std::uint64_t g_soloDisconnects = 0;

// onAbort observability: the app's own view, independent of what the mock recorded
// on the wire, so the audit can tell "OpenSideConnection failed" apart from
// "the cancel never arrived"
std::uint64_t g_soloAbortRuns = 0;
std::uint64_t g_soloAbortSideOpens = 0;
std::uint64_t g_soloAbortSideFailures = 0;

void* SoloCreateSlotState(void* userCtx)
{
    // userCtx packs the three per-endpoint knobs as "token:tryTls:requireTls"
    auto* state = WFX::New<SoloSlotState>();
    const std::string_view cfg{static_cast<const char*>(userCtx)};

    const auto c1 = cfg.find(':');
    const auto c2 = cfg.find(':', c1 + 1);

    state->token = static_cast<const char*>(userCtx); // the token is the leading field
    state->tryTls = cfg.substr(c1 + 1, c2 - c1 - 1) == "1";
    state->requireTls = cfg.substr(c2 + 1) == "1";

    return state;
}

void SoloDestroySlotState(void* slotState)
{
    WFX::Delete(static_cast<SoloSlotState*>(slotState));
}

void SoloOnDisconnect(void* /*slotState*/, WFX::DisconnectReason)
{
    g_soloDisconnects++;
}

void* SoloCreateOutput(void*)
{
    return WFX::New<SoloRes>();
}

void SoloDestroyOutput(void* out)
{
    WFX::Delete(static_cast<SoloRes*>(out));
}

WFX::EpCoro SoloConnect(WFX::SlotHandle h, void* slotStateVoid)
{
    auto* st = static_cast<SoloSlotState*>(slotStateVoid);

    // The token rides along on the probe: STARTTLS precedes AUTH, so it is the
    // only way the mock can tell which endpoint instance is connecting, and
    // therefore which upgrade behaviour to act out
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

        // The server declined. Failing closed here is the whole point: MySQL's
        // --ssl (CVE-2015-3152) and pgJDBC (CVE-2025-49146) both silently
        // continued in plaintext instead, which is what made them MITM-able
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

    // "OK <connId>", remembered so every response can report which physical
    // connection produced it
    ok.remove_prefix(2);
    while(!ok.empty() && ok.front() == ' ')
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

// Postgres-style graceful cancel: opens a throwaway connection to the same upstream
// and sends "CANCEL <connId>\n" with no handshake first, mirroring a real Postgres
// CancelRequest, which also rides a brand-new connection. connId identifies which
// physical primary connection is being aborted, exactly the way a real PID plus
// secret key would.
//
// Deliberately does not touch 'h' beyond OpenSideConnection(): the primary slot is
// still mid-request, which is why AbortSlotHandle does not even expose Send/Receive
// on it (see base.hpp's doc comment for why that would be unsafe).
WFX::EpAbortCoro SoloOnAbort(WFX::AbortSlotHandle h, void* slotStateVoid)
{
    auto* st = static_cast<SoloSlotState*>(slotStateVoid);
    g_soloAbortRuns++;

    auto [status, side] = co_await h.OpenSideConnection();
    if(status != WFX::EpSlotOk) {
        // auxConnLimit exhausted (or 0), or the dial itself failed. Either way there
        // is nothing to send the cancel on, so give up quietly: the primary slot's
        // own response/timeout cycle never depended on this
        g_soloAbortSideFailures++;
        co_return;
    }

    g_soloAbortSideOpens++;

    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "CANCEL %llu\n",
                                static_cast<unsigned long long>(st->connId));
    if(n > 0)
        co_await side.Send(buf, static_cast<std::uint32_t>(n));

    // leakAux deliberately skips this, to prove connectTimeoutSeconds reclaims a
    // side connection whose caller forgot, or here chose not, to Close() it
    if(!st->leakAux)
        side.Close();

    co_return;
}

// A real onAbort author will misuse Close() at least once. This one closes the side
// connection three times and keeps the handle afterwards. The engine must survive
// it (bump generationId before freeing, the same discipline every other pool-recycle
// path uses) rather than double-freeing the aux slot or corrupting whichever side
// connection is handed that slot next.
WFX::EpAbortCoro SoloOnAbortDoubleClose(WFX::AbortSlotHandle h, void* slotStateVoid)
{
    auto* st = static_cast<SoloSlotState*>(slotStateVoid);
    g_soloAbortRuns++;

    auto [status, side] = co_await h.OpenSideConnection();
    if(status != WFX::EpSlotOk) {
        g_soloAbortSideFailures++;
        co_return;
    }

    g_soloAbortSideOpens++;

    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "CANCEL %llu\n",
                                static_cast<unsigned long long>(st->connId));
    if(n > 0)
        co_await side.Send(buf, static_cast<std::uint32_t>(n));

    side.Close();
    side.Close(); // deliberate misuse: must be a harmless no-op, not a double free
    side.Close(); // and a third time for good measure

    co_return;
}

WFX::Shared::SerializeResult SoloSerialize(void* slotStateVoid, const void* reqVoid, char* buf,
                                           std::uint32_t bufLen, std::uint32_t* written,
                                           std::uint64_t* /*streamKey, not multiplexed*/)
{
    auto* st = static_cast<SoloSlotState*>(slotStateVoid);
    auto& req = *static_cast<const SoloReq*>(reqVoid);

    int n = 0;

    // A re-serialize on a fetch stream is the engine asking for the next page. The
    // cursor lives in slot state, exactly where a real protocol would keep its
    // portal name, paging_state or SCAN cursor
    if(req.mode == SoloMode::STREAM_FETCH && st->pagesSent > 0)
        n = std::snprintf(buf, bufLen, "MORE\n");
    else if(req.mode == SoloMode::STREAM_FETCH)
        n = std::snprintf(buf, bufLen, "PAGE %u %u\n", req.count, req.size);
    else if(req.mode == SoloMode::STREAM_SERVER)
        n = std::snprintf(buf, bufLen, "STREAM %u %u %u %u\n", req.count, req.size, req.stallAfter,
                          req.stallMs);
    else
        n = std::snprintf(buf, bufLen, "GET %s\n", req.key.c_str());

    if(n < 0 || static_cast<std::uint32_t>(n) >= bufLen)
        return WFX::EpSerBufferTooSmall;

    st->mode = req.mode;
    st->leakAux = req.leakAux;
    *written = static_cast<std::uint32_t>(n);

    return WFX::EpSerOk;
}

WFX::Shared::ParseResult SoloParse(void* slotStateVoid, void* /*parseState*/, const char* buf,
                                   std::uint32_t len, std::uint32_t* consumed, void* outObj,
                                   bool isEof, std::uint64_t* /*completedKey*/)
{
    auto* st = static_cast<SoloSlotState*>(slotStateVoid);
    auto* out = static_cast<SoloRes*>(outObj);

    const std::string_view view{buf, len};

    const auto nl = view.find('\n');
    if(nl == std::string_view::npos) {
        *consumed = 0;
        return isEof ? WFX::EpParseError : WFX::EpParseIncomplete;
    }

    const std::string_view line = view.substr(0, nl);
    *consumed = static_cast<std::uint32_t>(nl + 1);

    out->connId = st->connId;

    if(line.starts_with("VAL ")) {
        out->value.assign(line.substr(4));
        out->ended = true;
        st->pagesSent = 0;
        return WFX::EpParseDone;
    }

    if(line.starts_with("CHUNK ")) {
        // Assign, never append: peak memory stays at one chunk no matter how many
        // chunks the whole response turns out to be
        out->value.assign(line.substr(6));
        out->ended = false;
        st->pagesSent++;

        return st->mode == SoloMode::STREAM_FETCH ? WFX::EpParseChunkFetch : WFX::EpParseChunk;
    }

    if(line.starts_with("END")) {
        out->value.clear();
        out->ended = true;
        st->pagesSent = 0;
        return WFX::EpParseDone;
    }

    return WFX::EpParseError;
}

// Server-initiated "PUSH <text>". Only ever reached on a slot with no request in
// flight; anything arriving mid-request has to go through SoloParse instead, and
// the audit asserts that split explicitly.
bool SoloOnPush(void* /*slotState*/, const char* buf, std::uint32_t len, std::uint32_t* consumed)
{
    const std::string_view view{buf, len};

    const auto nl = view.find('\n');
    if(nl == std::string_view::npos) {
        *consumed = 0; // partial line, wait for the rest
        return true;
    }

    const std::string_view line = view.substr(0, nl);
    *consumed = static_cast<std::uint32_t>(nl + 1);

    if(!line.starts_with("PUSH ")) {
        g_soloPushRejects++;
        return false; // undecodable, so the engine closes the slot
    }

    g_soloPushCount++;
    g_soloPushBytes += line.size() - 5;

    return true;
}

WFX::EndpointDesc SoloDesc(const char* cfg)
{
    return WFX::EndpointDesc{
        .serialize        = SoloSerialize,
        .parse            = SoloParse,
        .onDisconnect     = SoloOnDisconnect,
        .createSlotState  = SoloCreateSlotState,
        .destroySlotState = SoloDestroySlotState,
        .createOutput     = SoloCreateOutput,
        .destroyOutput    = SoloDestroyOutput,
        .onPush           = SoloOnPush,
        .userCtx          = const_cast<void*>(static_cast<const void*>(cfg)),
    };
}

using SoloEp = WFX::Endpoint<SoloReq, SoloRes, &SoloConnect>;
using SoloAbortEp = WFX::Endpoint<SoloReq, SoloRes, &SoloConnect, &SoloOnAbort>;
using SoloAbortBadCloseEp = WFX::Endpoint<SoloReq, SoloRes, &SoloConnect, &SoloOnAbortDoubleClose>;

} // namespace

// Plain, no TLS probe. exactSlots is left off, so connLimit 4 rounds up to a full
// 64-bit bitmap word and the pool really holds 64 slots. That is deliberate: the
// pinning phase needs a pool long enough that a leaked reservation shows up as
// starvation rather than being absorbed.
inline const SoloEp SoloGood{
    SOLO_UPSTREAM, SoloDesc("good:0:0"),
    WFX::EndpointConfig{
        .connLimit             = 4,
        .connectTimeoutSeconds = 5,
        .requestTimeoutSeconds = 10,
        .idleTimeoutSeconds    = 5,
        .maxReconnectAttempts  = 1,
        .tlsConfig             = WFX::EpTlsInsecure,
    }
};

// Probes STARTTLS and requires it. The mock answers "N", so onConnect has to fail
// closed rather than continuing in plaintext (CVE-2015-3152 / CVE-2025-49146).
inline const SoloEp SoloTlsDowngrade{
    SOLO_UPSTREAM, SoloDesc("downgrade:1:1"),
    WFX::EndpointConfig{
        .connLimit             = 1,
        .connectTimeoutSeconds = 5,
        .requestTimeoutSeconds = 5,
        .idleTimeoutSeconds    = 5,
        .maxReconnectAttempts  = 1,
        .tlsConfig             = WFX::EpTlsInsecure,
    }
};

// The mock answers "S" then sends garbage instead of a ServerHello, so the upgrade
// has to fail and tear the slot down cleanly rather than hang or leak the coroutine
// frame.
inline const SoloEp SoloTlsGarbage{
    SOLO_UPSTREAM, SoloDesc("tlsgarbage:1:0"),
    WFX::EndpointConfig{
        .connLimit             = 1,
        .connectTimeoutSeconds = 5,
        .requestTimeoutSeconds = 5,
        .idleTimeoutSeconds    = 5,
        .maxReconnectAttempts  = 1,
        .tlsConfig             = WFX::EpTlsInsecure,
    }
};

// connLimit 4 so overlapping primaries can be in flight at once, plus headroom for
// the tight sequential leak-reclaim sequence. auxConnLimit 1 rounds up the same way
// connLimit does, so two concurrent aborts both get a side connection; the audit
// asserts they do not corrupt each other, not that 1 is a hard cap.
// connectTimeoutSeconds 5 is the engine's hard floor (it must be at least the timer
// tick, INVOKE_TIMEOUT_COOLDOWN), so the reclaim check waits it out rather than
// fighting it.
inline const SoloAbortEp SoloAbort{
    SOLO_UPSTREAM, SoloDesc("abort:0:0"),
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

// auxConnLimit 0: onAbort still fires, since desc.onAbort is set, but
// OpenSideConnection() has nowhere to allocate from and must fail every time,
// never crash and never hang.
inline const SoloAbortEp SoloAbortNoAux{
    SOLO_UPSTREAM, SoloDesc("abortnoaux:0:0"),
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

// The mock stalls one second before answering AUTH, which gives the audit a window
// to abandon the client while onConnect is still running. onAbort must not fire
// there: it would steal onConnect's own asyncData mid-await.
inline const SoloAbortEp SoloAbortMidConnect{
    SOLO_UPSTREAM, SoloDesc("abortmidconnect:0:0"),
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

// Deliberately hostile onAbort. Its own endpoint rather than a slot-state flag,
// because the point is exercising the ABI's close path directly instead of layering
// it onto SoloAbort's config.
inline const SoloAbortBadCloseEp SoloAbortBadClose{
    SOLO_UPSTREAM, SoloDesc("abort:0:0"),
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

namespace {

const SoloEp* SoloEndpointOf(std::string_view name) noexcept
{
    if(name == "downgrade")  return &SoloTlsDowngrade;
    if(name == "tlsgarbage") return &SoloTlsGarbage;
    return &SoloGood;
}

} // namespace

// Routes
WFX_GET("/health", [](WFX::Request, WFX::Response res) { res.Status(200).SendText("ok"); })

// One call on a mux instance.
//   X-Mux  good | bad | slow | reset               (default good)
//   X-Key  key to echo back; "sleep:<secs>:<value>" delays the mock's reply
WFX_GET("/mux/call", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view name = "good", key = "hello";
    req.GetHeader("X-Mux", name);
    req.GetHeader("X-Key", key);

    const MuxEp* ep = MuxEndpointOf(name);
    auto [status, out] = co_await ep->SendPayload(MuxReq{WFX::String(key.data(), key.size())});

    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("ep", EpJ(status));
    if(status == WFX::EpOk)
        j.Write("value", std::string_view{out->value});

    co_return;
})

WFX_GET("/mux/disconnects", [](WFX::Request, WFX::Response res) {
    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("idle", g_muxIdleDisconnects);
    j.Write("handshake", g_muxHandshakeTimeouts);
    j.Write("error", g_muxErrorDisconnects);
})

WFX_POST("/mux/disconnects/reset", [](WFX::Request, WFX::Response res) {
    g_muxIdleDisconnects = 0;
    g_muxHandshakeTimeouts = 0;
    g_muxErrorDisconnects = 0;
    res.Status(200).SendText("ok");
})

// One call on a solo instance.
//   X-Solo  good | downgrade | tlsgarbage | abort | abortnoaux | abortmidconnect |
//           abortbadclose                                       (default good)
//   X-Key   key to echo back; "slow:<secs>" delays the mock's reply
//   X-Leak  1 tells onAbort, on the abort instances, to skip side.Close()
WFX_GET("/solo/get", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view name = "good", key = "hello";
    req.GetHeader("X-Solo", name);
    req.GetHeader("X-Key", key);

    const bool leak = HeaderU32(req, "X-Leak", 0) != 0;
    SoloReq sreq{SoloMode::GET, WFX::String(key.data(), key.size()), 0, 0, 0, 0, leak};

    WFX::Shared::EndpointStatus status{};
    WFX::EndpointOutput<SoloRes> out;

    if(name == "abort")
        std::tie(status, out) = co_await SoloAbort.SendPayload(sreq);
    else if(name == "abortnoaux")
        std::tie(status, out) = co_await SoloAbortNoAux.SendPayload(sreq);
    else if(name == "abortmidconnect")
        std::tie(status, out) = co_await SoloAbortMidConnect.SendPayload(sreq);
    else if(name == "abortbadclose")
        std::tie(status, out) = co_await SoloAbortBadClose.SendPayload(sreq);
    else
        std::tie(status, out) = co_await SoloEndpointOf(name)->SendPayload(sreq);

    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("ep", EpJ(status));
    if(status == WFX::EpOk) {
        j.Write("value", std::string_view{out->value});
        j.Write("conn", out->connId);
    }

    co_return;
})

// Drains a stream to completion, accumulating counters only. Nothing here keeps a
// chunk alive past its own iteration, so if peak RSS grows with X-Count then it is
// the engine buffering, not the app.
//   X-Solo        good | abort                                (default good)
//   X-Mode        server -> CHUNK_READY, fetch -> CHUNK_READY_FETCH
//   X-Count       chunks to ask for
//   X-Size        payload bytes per chunk
//   X-Stop        abandon the stream after this many chunks, 0 = drain it
//   X-StallAfter  mock stalls before this chunk index, 0 = never
//   X-StallMs     how long that stall lasts
//
// X-Solo=abort with a stall lets the audit abandon the CLIENT mid-stream, once
// isStreaming is already set, which is where onAbort's scope cut applies: a
// streaming request still force-closes on client disconnect, it never fires onAbort.
WFX_GET("/solo/stream", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view name = "good", mode = "server";
    req.GetHeader("X-Solo", name);
    req.GetHeader("X-Mode", mode);

    const std::uint32_t count = HeaderU32(req, "X-Count", 10);
    const std::uint32_t stop = HeaderU32(req, "X-Stop", 0);

    SoloReq sreq{mode == "fetch" ? SoloMode::STREAM_FETCH : SoloMode::STREAM_SERVER, {}, count,
                 HeaderU32(req, "X-Size", 64), HeaderU32(req, "X-StallAfter", 0),
                 HeaderU32(req, "X-StallMs", 0), false};

    auto stream = (name == "abort") ? SoloAbort.Stream(sreq) : SoloGood.Stream(sreq);

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

        // Order-sensitive fold over every chunk, so the harness can prove it got
        // the same bytes in the same order: a dropped or reordered chunk shows up
        checksum = WFX::WyHash(std::string_view{chunk.data->value.data(), chunk.data->value.size()},
                               checksum);

        // Abandon mid-stream: the slot must still be reclaimed, not stranded
        if(stop != 0 && chunks >= stop)
            break;
    }

    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("ep", EpJ(epStatus));
    j.Write("chunks", chunks);
    j.Write("bytes", bytes);
    j.Write("checksum", checksum);
    j.Write("conn", conn);
    j.Write("done", static_cast<std::uint64_t>(done ? 1 : 0));

    co_return;
})

// Reserves a connection, runs X-N requests on it and reports the connId seen each
// time. All of them must match: that is the isolation guarantee pinning exists to
// give.
//   X-N        requests to run on the reservation
//   X-Release  early | late | double   which release pattern to take
WFX_GET("/solo/reserve", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view releaseMode = "late";
    req.GetHeader("X-Release", releaseMode);

    const std::uint32_t n = HeaderU32(req, "X-N", 3);

    auto slot = SoloGood.Reserve();

    res.Status(200);
    auto j = WFX::ImJson(res);

    if(!slot.IsValid()) {
        j.Write("reserved", static_cast<std::uint64_t>(0));
        co_return;
    }

    j.Write("reserved", static_cast<std::uint64_t>(1));

    std::uint64_t firstConn = 0, sameConn = 1, lastStatus = 0;
    for(std::uint32_t i = 0; i < n; i++) {
        auto [status, out] = co_await slot.SendPayload(SoloReq{SoloMode::GET, "pinned", 0, 0});
        lastStatus = EpJ(status);

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

    // Releasing twice must be harmless, the handle clears on the first call
    if(releaseMode == "double") {
        slot.Release();
        slot.Release();
    }
    else if(releaseMode == "early")
        slot.Release();

    // "late" leaves it to the destructor, the path an early co_return would take

    co_return;
})

// Two independent reservations must land on two different physical connections, and
// byte-identical payloads on the two pins must still come back from those two
// connections rather than from one shared round trip.
WFX_GET("/solo/reserve/pair", [](WFX::Request, WFX::Response res) -> WFX::Coro {
    auto a = SoloGood.Reserve();
    auto b = SoloGood.Reserve();

    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("a_ok", static_cast<std::uint64_t>(a.IsValid() ? 1 : 0));
    j.Write("b_ok", static_cast<std::uint64_t>(b.IsValid() ? 1 : 0));

    if(!a.IsValid() || !b.IsValid())
        co_return;

    auto [sa, oa] = co_await a.SendPayload(SoloReq{SoloMode::GET, "same", 0, 0});
    auto [sb, ob] = co_await b.SendPayload(SoloReq{SoloMode::GET, "same", 0, 0});

    j.Write("sa", EpJ(sa));
    j.Write("sb", EpJ(sb));

    if(sa == WFX::EpOk && sb == WFX::EpOk) {
        j.Write("conn_a", oa->connId);
        j.Write("conn_b", ob->connId);
        j.Write("distinct", static_cast<std::uint64_t>(oa->connId != ob->connId ? 1 : 0));
    }

    co_return;
})

// Streaming through a pinned connection: the two features have to compose, and
// every chunk must come off the reserved slot rather than a pooled one.
//   X-Count  chunks to ask for
WFX_GET("/solo/reserve/stream", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    const std::uint32_t count = HeaderU32(req, "X-Count", 8);

    auto slot = SoloGood.Reserve();

    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("reserved", static_cast<std::uint64_t>(slot.IsValid() ? 1 : 0));

    if(!slot.IsValid())
        co_return;

    auto stream = slot.Stream(SoloReq{SoloMode::STREAM_SERVER, {}, count, RESERVE_CHUNK_BYTES});

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

WFX_GET("/solo/push", [](WFX::Request, WFX::Response res) {
    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("count", g_soloPushCount);
    j.Write("bytes", g_soloPushBytes);
    j.Write("rejects", g_soloPushRejects);
    j.Write("disconnects", g_soloDisconnects);
})

WFX_POST("/solo/push/reset", [](WFX::Request, WFX::Response res) {
    g_soloPushCount = 0;
    g_soloPushBytes = 0;
    g_soloPushRejects = 0;
    g_soloDisconnects = 0;
    res.Status(200).SendText("ok");
})

// The app's own view of onAbort activity, independent of what the mock recorded on
// the wire: separates "onAbort never ran" from "it ran but OpenSideConnection failed".
WFX_GET("/solo/abort", [](WFX::Request, WFX::Response res) {
    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("runs", g_soloAbortRuns);
    j.Write("side_opens", g_soloAbortSideOpens);
    j.Write("side_failures", g_soloAbortSideFailures);
})

WFX_POST("/solo/abort/reset", [](WFX::Request, WFX::Response res) {
    g_soloAbortRuns = 0;
    g_soloAbortSideOpens = 0;
    g_soloAbortSideFailures = 0;
    res.Status(200).SendText("ok");
})

// Worker memory, via the telemetry API every user already gets for free. The audit
// samples this either side of a large stream: if peak memory tracks the chunk COUNT
// rather than the chunk SIZE, the engine is accumulating chunks instead of reusing
// one output object.
WFX_GET("/rss", [](WFX::Request, WFX::Response res) {
    const auto self = WFX::GetProcessMetricsAt(WFX::WorkerIndex());

    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("rss", self.rssBytes);
    j.Write("vm", self.vmBytes);
    j.Write("pid", static_cast<std::uint64_t>(self.pid));
})

// Per-endpoint metrics, summed across workers, each tagged with its host. Several
// instances share one host, so the metrics phase asserts on the aggregate delta
// across every slot rather than mapping a slot back to one instance. ev.host is a
// Shared::StringView, written through the JsonWriter overload for it.
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
        j.Write("connect_failures", ev.metrics.connectFailures);
        j.Write("tls_failures", ev.metrics.tlsFailures);
        j.Write("request_timeouts", ev.metrics.requestTimeouts);
        j.Write("pool_exhausted", ev.metrics.poolExhausted);
        j.Write("other_errors", ev.metrics.otherErrors);
        j.Write("reconnects", ev.metrics.reconnects);
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
