// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_ENDPOINT_BASE_HPP
#define WFX_INC_WFX_ENDPOINT_BASE_HPP

// -----------------------------------------------------------------------
// wfx/endpoint/base.hpp
// Raw building block for outbound protocol endpoints.
//
// Provides:
//   WFX::Endpoint<TReq, TRes [, OnConnect [, OnAbort]]> : the endpoint template
//   WFX::EndpointOutput<T>                  : RAII response wrapper
//   WFX::SlotHandle                         : handle passed into onConnect
//   WFX::AbortSlotHandle                    : handle passed into onAbort (OpenSideConnection only)
//   WFX::SlotReceiveResult                  : result of co_await handle.Receive()
//   WFX::EpCoro                             : return type for onConnect functions
//   WFX::EpAbortCoro                        : return type for onAbort functions
//   WFX::EndpointDesc                       : describe the protocol (serialize / parse / ...)
//   WFX::EndpointConfig                     : connection pool settings
//   WFX::ReservedSlot<TReq, TRes>           : one pinned connection, from ep.Reserve()
//   WFX::StreamHandle / StreamChunk         : chunked reads, from ep.Stream()
//   WFX::DisconnectReason                   : type behind the onDisconnect reasons
//   WFX::EndpointStatus                     : type behind the EpOk / EpXxx constants
//
//   SendPayload result codes (from co_await ep.SendPayload(req).first):
//     WFX::EpOk                  success, output is valid
//     WFX::EpPoolExhausted       all connections in use, try later
//     WFX::EpConnectFailure      connect() failed immediately (a later refusal is EpInternalError)
//     WFX::EpSslFailure          TLS handshake failed
//     WFX::EpHandshakeTimeout    TCP connect / TLS handshake / onConnect took too long
//     WFX::EpRequestTimeout      no response within requestTimeoutSeconds
//     WFX::EpSerializeError      your serialize() returned EpSerError
//     WFX::EpSocketFailure       socket creation / options failed (before ever reaching connect())
//     WFX::EpBufferError         internal buffer init failure
//     WFX::EpInsufficientBuffer  request too large even after the engine grew the buffer to its maximum
//     WFX::EpInvalidKey          nowhere valid to go: a dead ReservedSlot, or Next() on an ended stream
//     WFX::EpEpollError          epoll re-arm failed on a reused slot (engine-internal)
//     WFX::EpInternalError       unclassified engine failure
//
//   onConnect flow control (return from EpCoro):
//     WFX::EpReady               slot is authenticated, enter pool
//     WFX::EpRetry               transient failure, engine will reconnect with backoff
//     WFX::EpFatal               permanent failure, slot is discarded
//
//   onDisconnect reason (parameter to onDisconnect):
//     WFX::EpIdleTimeout             timed out outside the connect phase: idle, or a request timeout
//     WFX::EpHandshakeTimeoutReason  slot closed while still connecting / handshaking / in onConnect
//     WFX::EpDisconnectTlsError      TLS handshake failed, an in-band upgrade's included
//     WFX::EpDisconnectError         everything else: I/O error, protocol error, peer closed, etc
//
//   TLS config (set in EndpointConfig::tlsConfig):
//     WFX::EpTlsAuto             TLS on for well-known TLS ports (443, 465, 993, 8443, ...)
//     WFX::EpTlsRequire          TLS always, regardless of port
//     WFX::EpTlsInsecure         plaintext always, even on port 443
//
//   In serialize():
//     WFX::EpSerOk               bytes written, proceed
//     WFX::EpSerBufferTooSmall   buf is too small; engine retries with a larger buffer
//     WFX::EpSerError            unrecoverable error; slot is closed
//
//   In parse():
//     WFX::EpParseIncomplete     need more bytes; call again when more data arrives
//     WFX::EpParseChunk          one chunk ready; more bytes arrive unprompted
//     WFX::EpParseChunkFetch     one chunk ready; engine re-serializes for the next batch
//     WFX::EpParseDone           complete message received; slot returns to pool
//     WFX::EpParseClose          complete message received; close the slot after delivery
//     WFX::EpParseError          unrecoverable parse error; slot is closed
//
//   In onConnect, from co_await handle.Send(), (co_await handle.Receive()).status
//   and co_await handle.UpgradeToTLS(). Anything but EpSlotOk is fatal for the
//   slot, co_return EpFatal:
//     WFX::EpSlotOk              operation succeeded
//     WFX::EpSlotBufferError     slot buffer couldn't be allocated or grown
//     WFX::EpSlotEpollError      re-arming the slot with epoll failed
//     WFX::EpSlotIoError         socket read/write failed
//     WFX::EpSlotTlsError        UpgradeToTLS couldn't wrap, or a side connection's handshake failed
//     WFX::EpSlotInvalidState    invalid for this slot now (upgrading an already-secure one)
//
//   onAbort fires when a single-slot request's client disconnects before the backend replied,
//   instead of force-closing the slot (a multiplexed request just drops its stream, mid-Stream()
//   and still-connecting requests force-close)
//   The slot is left alone, its real response still arrives and completes normally.
//   AbortSlotHandle only has OpenSideConnection()/NegotiatedProtocol(), since the slot is
//   still mid-request:
//     co_await handle.OpenSideConnection() -> {EpSlotOk/error, SlotHandle side}
//       side.Send(...) / side.Receive() / side.UpgradeToTLS() same as any SlotHandle
//       side.Close()   closes it early; connectTimeoutSeconds is the safety net either way
//
// -----------------------------------------------------------------------
// Minimal example (stateless JSON API client)
// -----------------------------------------------------------------------
//
//   struct ApiReq { std::string_view path; };
//   struct ApiRes { int status; std::string body; };
//
//   WFX::Shared::SerializeResult Serialize(void*, const void* req, char* buf, std::uint32_t len,
//                                          std::uint32_t* written, std::uint64_t* /*streamKey*/)
//   {
//       auto& r = *static_cast<const ApiReq*>(req);
//       int n = snprintf(buf, len, "GET %s HTTP/1.0\r\nHost: api.example.com\r\n\r\n",
//                        r.path.data());
//       if(n >= (int)len)
//           return WFX::EpSerBufferTooSmall;
//
//       *written = n;
//       return WFX::EpSerOk;
//   }
//
//   WFX::Shared::ParseResult Parse(void*, void*, const char* buf, std::uint32_t len, std::uint32_t* consumed,
//                                  void* out, bool /*isEof*/, std::uint64_t* /*completedKey*/)
//   {
//       auto* res = static_cast<ApiRes*>(out);
//       // ... fill res->status and res->body from buf ...
//       *consumed = len;
//       return WFX::EpParseDone;
//   }
//
//   inline const auto Api = WFX::Endpoint<ApiReq, ApiRes>{
//       "api.example.com:80",
//       WFX::EndpointDesc{
//           .serialize     = Serialize,
//           .parse         = Parse,
//           .createOutput  = [](void*) -> void* { return WFX::New<ApiRes>(); },
//           .destroyOutput = [](void* p) { WFX::Delete(static_cast<ApiRes*>(p)); },
//       },
//       WFX::EndpointConfig{
//           .connLimit             = 4,
//           .connectTimeoutSeconds = 10,
//           .requestTimeoutSeconds = 10,
//           .idleTimeoutSeconds    = 60,
//       }
//   };
//
//   WFX_GET("/proxy", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
//       auto [status, out] = co_await Api.SendPayload({"/users"});
//       if(status != WFX::EpOk) { res.Status(502).SendText("upstream error"); co_return; }
//       res.Status(out->status).SendText(out->body);
//       co_return;
//   });
//
// -----------------------------------------------------------------------
// With onConnect (authenticated Redis connection)
// -----------------------------------------------------------------------
//
//   WFX::EpCoro Authenticate(WFX::SlotHandle h, void* /*state*/)
//   {
//       static const char kAuth[] = "*2\r\n$4\r\nAUTH\r\n$8\r\npassword\r\n";
//       if(co_await h.Send(kAuth, sizeof(kAuth) - 1) != WFX::EpSlotOk)
//           co_return WFX::EpFatal;
//
//       auto recv = co_await h.Receive();
//       if(recv.status != WFX::EpSlotOk)
//           co_return WFX::EpFatal;
//
//       co_return (recv.len >= 3 && recv.buf[0] == '+') ? WFX::EpReady : WFX::EpFatal;
//   }
//
//   inline const auto Redis = WFX::Endpoint<RedisGet, RedisVal, &Authenticate>{
//       "redis.internal:6379",
//       WFX::EndpointDesc{ .serialize = ..., .parse = ... },
//       WFX::EndpointConfig{ .connLimit = 8, .connectTimeoutSeconds = 10, .requestTimeoutSeconds = 5,
//                            .idleTimeoutSeconds = 60 }
//   };
//
// -----------------------------------------------------------------------
// With onAbort (Postgres-style cancel over a fresh connection)
// -----------------------------------------------------------------------
//
//   WFX::EpAbortCoro CancelQuery(WFX::AbortSlotHandle h, void* state)
//   {
//       auto* pg = static_cast<PgSlotState*>(state);
//       auto [status, side] = co_await h.OpenSideConnection();
//       if (status == WFX::EpSlotOk) {
//           char msg[16];
//           EncodeCancelRequest(msg, pg->backendPid, pg->secretKey);
//           co_await side.Send(msg, sizeof(msg));
//           side.Close();
//       }
//       co_return;
//   }
//
//   inline const auto Postgres = WFX::Endpoint<PgReq, PgRes, &Connect, &CancelQuery>{
//       "postgres.internal:5432",
//       WFX::EndpointDesc{ .serialize = ..., .parse = ... },
//       WFX::EndpointConfig{ .connLimit = 8, .auxConnLimit = 4, .connectTimeoutSeconds = 10,
//                            .requestTimeoutSeconds = 30, .idleTimeoutSeconds = 60 }
//   };
//
// -----------------------------------------------------------------------
// parse() isEof note:
//   isEof is true on the final call after the peer closed the connection.
//   At that point you MUST NOT return EpParseIncomplete (return EpParseClose
//   or EpParseError). Used for HTTP/1.0-style close-delimited bodies.
// -----------------------------------------------------------------------

#include "async/endpoint.hpp"
#include "wfx/async.hpp"

namespace WFX {

// -----------------------------------------------------------------------
// Core endpoint template
//
// Declare at namespace scope, before Run(). Each declaration registers its own
// endpoint, with its own connection pool described by EndpointConfig.
//
//   inline const auto MyEp = WFX::Endpoint<MyReq, MyRes>{ host, desc, cfg };
//
// With an onConnect coroutine (TLS auth, Redis AUTH, SMTP EHLO, etc.):
//   inline const auto MyEp = WFX::Endpoint<MyReq, MyRes, &MyOnConnect>{ ... };
//
// With onAbort too (graceful cancel on client disconnect, needs .auxConnLimit set):
//   inline const auto MyEp = WFX::Endpoint<MyReq, MyRes, &MyOnConnect, &MyOnAbort>{ ... };
// -----------------------------------------------------------------------
template <typename TReq, typename TRes, Async::UserOnConnectFn OnConnect = nullptr,
          Async::UserOnAbortFn OnAbort = nullptr>
using Endpoint = Async::Resolve<TReq, TRes, OnConnect, OnAbort>;

// -----------------------------------------------------------------------
// RAII owner for the response returned by co_await ep.SendPayload()
//
// Access the response with * or -> or .Get().
// Valid until the variable goes out of scope. Do not store the raw pointer.
// -----------------------------------------------------------------------
template <typename T> using EndpointOutput = Async::EndpointOutput<T>;

// -----------------------------------------------------------------------
// RAII owner of a connection pinned via ep.Reserve(), for protocols where
// consecutive requests must share one connection (SQL transactions,
// LISTEN/NOTIFY). Releases on destruction; check IsValid() first, since
// Reserve() returns an empty one when the pool is exhausted or the endpoint
// is multiplexed.
// -----------------------------------------------------------------------
template <typename TReq, typename TRes> using ReservedSlot = Async::ReservedSlot<TReq, TRes>;

// -----------------------------------------------------------------------
// Chunked consumption of a large response via ep.Stream(req). Hold the
// handle across the whole loop; each chunk's .data borrows engine memory
// and is only valid until the next Next(), which is what keeps peak
// memory at one chunk rather than the entire response.
// -----------------------------------------------------------------------
template <typename TReq, typename TRes> using StreamHandle = Async::StreamHandle<TReq, TRes>;
template <typename TRes> using StreamChunk = Async::StreamChunk<TRes>;

// -----------------------------------------------------------------------
// Passed into onConnect coroutines. Use handle.Send() and handle.Receive()
// to perform the handshake before the slot enters the pool.
// -----------------------------------------------------------------------
using SlotHandle = Async::SlotHandle;

// -----------------------------------------------------------------------
// Passed into onAbort coroutines. Only OpenSideConnection()/
// NegotiatedProtocol(), the slot is still mid-request, so
// Send/Receive/UpgradeToTLS aren't exposed here.
// -----------------------------------------------------------------------
using AbortSlotHandle = Async::AbortSlotHandle;

// -----------------------------------------------------------------------
// Returned by co_await handle.Receive()
//   .status   EpSlotOk, or one of the EpSlot*Error codes above
//   .buf      pointer to received bytes (valid until the next Receive call)
//   .len      number of bytes in .buf
// -----------------------------------------------------------------------
using SlotReceiveResult = Async::SlotReceiveResult;

// -----------------------------------------------------------------------
// Return type for onConnect functions
//
//   WFX::EpCoro Authenticate(WFX::SlotHandle h, void* state) {
//       // ... handshake ...
//       co_return WFX::EpReady;
//   }
// -----------------------------------------------------------------------
using EpCoro = Async::Task<Shared::ConnectResult>;

// -----------------------------------------------------------------------
// Return type for onAbort functions. No verdict to return, unlike EpCoro:
// the original slot's fate is decided by its own receive/parse/timeout
// cycle.
//
//   WFX::EpAbortCoro CancelQuery(WFX::AbortSlotHandle h, void* state) {
//       // ... co_await h.OpenSideConnection(), send the cancel, side.Close() ...
//       co_return;
//   }
// -----------------------------------------------------------------------
using EpAbortCoro = Async::Task<void>;

// -----------------------------------------------------------------------
// Protocol descriptor. Fill the fields your protocol needs, leave the
// rest null. serialize and parse are always required.
//
// Required:
//   .serialize(slotState, &req, buf, bufLen, &written, &streamKey)
//       Write the wire encoding of req into buf. Return EpSerOk on
//       success, EpSerBufferTooSmall if buf is too small (engine grows
//       the buffer and calls again, up to its configured maximum),
//       EpSerError on unrecoverable failure. *streamKey stays 0 unless
//       the protocol multiplexes.
//
//   .parse(slotState, parseState, buf, len, &consumed, outObj, isEof, &completedKey)
//       Read arriving bytes and fill outObj. Set *consumed to the number
//       of bytes you read. Return EpParseDone or EpParseClose on a
//       complete message, EpParseIncomplete if you need more bytes,
//       EpParseError on failure. When isEof is true you must not return
//       EpParseIncomplete. *completedKey stays 0 unless the protocol
//       multiplexes.
//
// Nullable (omit or set to nullptr if not needed):
//   .onConnect                         -> set by Endpoint<> from the OnConnect template arg
//   .onAbort                           -> set by Endpoint<> from the OnAbort template arg
//   .onDisconnect(slotState, reason)
//   .createSlotState(userCtx)          -> void*  per-connection context
//   .destroySlotState(slotState)
//   .createParseState(slotState)       -> void*  per-request parser scratch space
//   .destroyParseState(parseState)
//   .resetParseState(parseState)       -> called between requests when non-null
//   .createOutput(slotState)           -> void*  allocate the TRes instance
//   .destroyOutput(outputPtr)          -> free the TRes instance (called by EndpointOutput)
//   .coalesceKey(&req)                 -> uint64_t  identical keys share one in-flight request
//   .cloneOutput(slotState, srcOutput) -> void*  REQUIRED when coalesceKey is set
//   .hasCapacity(slotState)            -> bool  multiplexing: room for one more request on this slot
//   .takeStreamOutput(slotState, key)  -> void*  multiplexing: a finished stream's response, or nullptr
//   .onPush(slotState, buf, len, &consumed) -> handle a message the server sends unprompted
//   .statusCode(outObj)                -> the response's status code for the status1xx-5xx metrics
//   .userCtx                           -> forwarded to createSlotState as its argument
// -----------------------------------------------------------------------
using EndpointDesc = Shared::EndpointDesc;

// -----------------------------------------------------------------------
// Connection pool settings
//
//   .connLimit              max simultaneous connections in the pool
//   .auxConnLimit           max simultaneous OpenSideConnection()s; 0 = disabled (default)
//   .dnsRefreshSeconds      0 = respect DNS TTL, N = refresh at most every N seconds (a shorter TTL wins)
//   .connectTimeoutSeconds  TCP + TLS + onConnect must finish in this window
//   .requestTimeoutSeconds  serialize + send + parse must finish in this window
//   .idleTimeoutSeconds     idle slots are closed after this many seconds
//   .maxReconnectAttempts   max backoff retries before a slot is marked fatal
//   .reconnectBackoffBase   initial backoff in seconds
//   .reconnectBackoffMax    backoff cap in seconds
//   .tlsConfig              EpTlsAuto / EpTlsRequire / EpTlsInsecure
//   .exactSlots             allocate connLimit/auxConnLimit exactly instead of rounding up to 64
//   .prewarm                slots to connect eagerly on startup
//   .maxConcurrentStreams   not read by the engine; hasCapacity caps multiplexing
//   .alpnProtocols          ALPN to offer on TLS, most preferred first: "h2,http/1.1"; empty offers none.
//                           A list TLS can't carry is fatal at startup
// -----------------------------------------------------------------------
using EndpointConfig = Shared::EndpointConfig;

// -----------------------------------------------------------------------
// Type behind the EpOk / EpXxx constants below
// -----------------------------------------------------------------------
using EndpointStatus = Shared::EndpointStatus;

// -----------------------------------------------------------------------
// TLS mode (set in EndpointConfig::tlsConfig)
// -----------------------------------------------------------------------
// NOLINTBEGIN(readability-identifier-naming)
inline constexpr auto EpTlsAuto = Shared::EndpointTLSConfig::AUTO;
inline constexpr auto EpTlsRequire = Shared::EndpointTLSConfig::FORCE_REQUIRE;
inline constexpr auto EpTlsInsecure = Shared::EndpointTLSConfig::FORCE_INSECURE;

// -----------------------------------------------------------------------
// SendPayload result codes
// Inspected on the first element of the pair from co_await ep.SendPayload()
// -----------------------------------------------------------------------
inline constexpr auto EpOk = Shared::EndpointStatus::SUCCESS;
inline constexpr auto EpBufferError = Shared::EndpointStatus::BUFFER_ERROR;
inline constexpr auto EpInsufficientBuffer = Shared::EndpointStatus::INSUFFICIENT_BUFFER;
inline constexpr auto EpInvalidKey = Shared::EndpointStatus::INVALID_KEY;
inline constexpr auto EpPoolExhausted = Shared::EndpointStatus::POOL_EXHAUSTED;
inline constexpr auto EpSocketFailure = Shared::EndpointStatus::SOCKET_FAILURE;
inline constexpr auto EpConnectFailure = Shared::EndpointStatus::CONNECT_FAILURE;
inline constexpr auto EpSslFailure = Shared::EndpointStatus::SSL_FAILURE;
inline constexpr auto EpInternalError = Shared::EndpointStatus::INTERNAL_ERROR;
inline constexpr auto EpSerializeError = Shared::EndpointStatus::SERIALIZE_ERROR;
inline constexpr auto EpEpollError = Shared::EndpointStatus::EPOLL_ERROR;
inline constexpr auto EpHandshakeTimeout = Shared::EndpointStatus::HANDSHAKE_TIMEOUT;
inline constexpr auto EpRequestTimeout = Shared::EndpointStatus::REQUEST_TIMEOUT;

// -----------------------------------------------------------------------
// onConnect flow control (co_return one of these from your EpCoro)
// -----------------------------------------------------------------------
inline constexpr auto EpReady = Shared::ConnectResult::READY;
inline constexpr auto EpRetry = Shared::ConnectResult::RETRY;
inline constexpr auto EpFatal = Shared::ConnectResult::FATAL;

// -----------------------------------------------------------------------
// onDisconnect reason (parameter to EndpointDesc::onDisconnect)
// -----------------------------------------------------------------------
using DisconnectReason = Shared::DisconnectReason;

inline constexpr auto EpIdleTimeout = Shared::DisconnectReason::TIMEOUT;
inline constexpr auto EpHandshakeTimeoutReason = Shared::DisconnectReason::HANDSHAKE_TIMEOUT;
inline constexpr auto EpDisconnectError = Shared::DisconnectReason::ERROR;
inline constexpr auto EpDisconnectTlsError = Shared::DisconnectReason::TLS_ERROR;

// -----------------------------------------------------------------------
// In serialize() (return value)
// -----------------------------------------------------------------------
inline constexpr auto EpSerOk = Shared::SerializeResult::OK;
inline constexpr auto EpSerBufferTooSmall = Shared::SerializeResult::BUFFER_TOO_SMALL;
inline constexpr auto EpSerError = Shared::SerializeResult::ERROR;

// -----------------------------------------------------------------------
// In parse() (return value)
// -----------------------------------------------------------------------
inline constexpr auto EpParseIncomplete = Shared::ParseResult::INCOMPLETE;
inline constexpr auto EpParseChunk = Shared::ParseResult::CHUNK_READY;
inline constexpr auto EpParseChunkFetch = Shared::ParseResult::CHUNK_READY_FETCH;
inline constexpr auto EpParseDone = Shared::ParseResult::COMPLETE_KEEP_ALIVE;
inline constexpr auto EpParseClose = Shared::ParseResult::COMPLETE_CLOSE;
inline constexpr auto EpParseError = Shared::ParseResult::ERROR;

// -----------------------------------------------------------------------
// In onConnect, shared by co_await handle.Send(), .Receive().status and
// .UpgradeToTLS(). Anything other than EpSlotOk is fatal for the slot:
// co_return EpFatal
// -----------------------------------------------------------------------
inline constexpr auto EpSlotOk = Shared::SlotStatus::OK;
inline constexpr auto EpSlotBufferError = Shared::SlotStatus::BUFFER_ERROR;
inline constexpr auto EpSlotEpollError = Shared::SlotStatus::EPOLL_ERROR;
inline constexpr auto EpSlotIoError = Shared::SlotStatus::IO_ERROR;
inline constexpr auto EpSlotTlsError = Shared::SlotStatus::TLS_ERROR;
inline constexpr auto EpSlotInvalidState = Shared::SlotStatus::INVALID_STATE;
// NOLINTEND(readability-identifier-naming)

} // namespace WFX

#endif // WFX_INC_WFX_ENDPOINT_BASE_HPP
