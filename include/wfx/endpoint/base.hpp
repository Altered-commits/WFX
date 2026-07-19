// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_ENDPOINT_BASE_HPP
#define WFX_INC_WFX_ENDPOINT_BASE_HPP

// -----------------------------------------------------------------------
// wfx/endpoint/base.hpp
// Raw building block for outbound protocol endpoints.
//
// Provides:
//   WFX::Endpoint<TReq, TRes [, OnConnect]> : the endpoint template
//   WFX::EndpointOutput<T>                  : RAII response wrapper
//   WFX::SlotHandle                         : handle passed into onConnect
//   WFX::SlotReceiveResult                  : result of co_await handle.Receive()
//   WFX::EpCoro                             : return type for onConnect functions
//   WFX::EndpointDesc                       : describe the protocol (serialize / parse / ...)
//   WFX::EndpointConfig                     : connection pool settings
//   WFX::EndpointTLS                        : TLS mode type alias
//
//   SendPayload result codes (from co_await ep.SendPayload(req).first):
//     WFX::EpOk                  success, output is valid
//     WFX::EpPoolExhausted       all connections in use, try later
//     WFX::EpConnectFailure      TCP handshake failed
//     WFX::EpSslFailure          TLS handshake failed
//     WFX::EpHandshakeTimeout    TCP connect / TLS handshake / onConnect took too long
//     WFX::EpRequestTimeout      no response within requestTimeoutSeconds
//     WFX::EpSerializeError      your serialize() returned EpSerError
//     WFX::EpSocketFailure       socket creation / options failed (before ever reaching connect())
//     WFX::EpBufferError         internal buffer init failure
//     WFX::EpInsufficientBuffer  buffer too small (engine retries once with a larger one)
//     WFX::EpInvalidKey          endpoint index out of range (engine bug, not user error)
//     WFX::EpEpollError          epoll re-arm failed on a reused slot (engine-internal)
//     WFX::EpInternalError       unclassified engine failure
//
//   onConnect flow control (return from EpCoro):
//     WFX::EpReady               slot is authenticated, enter pool
//     WFX::EpRetry               transient failure, engine will reconnect with backoff
//     WFX::EpFatal               permanent failure, slot is discarded
//
//   onDisconnect reason (parameter to onDisconnect):
//     WFX::EpIdleTimeout             slot closed after idleTimeoutSeconds with no activity
//     WFX::EpHandshakeTimeoutReason  slot closed while still connecting / handshaking
//     WFX::EpDisconnectError         everything else: I/O error, protocol error, peer closed, etc
//
//   TLS config (set in EndpointConfig::tlsConfig):
//     WFX::EpTlsAuto             TLS on by default for port 443/8443
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
//   In onConnect, from co_await handle.Send(), (co_await handle.Receive()).status-
//   -and co_await handle.UpgradeToTLS(). Anything but EpSlotOk is fatal for the-
//   -slot, co_return EpFatal:
//     WFX::EpSlotOk              operation succeeded
//     WFX::EpSlotBufferError     slot buffer couldn't be allocated or grown
//     WFX::EpSlotEpollError      re-arming the slot with epoll failed
//     WFX::EpSlotIoError         socket read/write failed
//     WFX::EpSlotTlsError        TLS wrap or handshake failed
//     WFX::EpSlotInvalidState    invalid for this slot now (upgrading an already-secure one)
//
// -----------------------------------------------------------------------
// Minimal example (stateless JSON API client)
// -----------------------------------------------------------------------
//
//   struct ApiReq { std::string_view path; };
//   struct ApiRes { int status; std::string body; };
//
//   WFX::SerializeResult Serialize(void*, const void* req,
//                                  char* buf, std::uint32_t len, std::uint32_t* written)
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
//   WFX::ParseResult Parse(void*, void*, const char* buf, std::uint32_t len,
//                          std::uint32_t* consumed, void* out, bool /*isEof*/)
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
//           .requestTimeoutSeconds = 10,
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
//       WFX::EndpointConfig{ .connLimit = 8, .requestTimeoutSeconds = 5 }
//   };
//
// -----------------------------------------------------------------------
// parse() isEof note:
//   isEof is true on the final call after the peer closed the connection.
//   At that point you MUST NOT return EpParseIncomplete (return EpParseClose-
//   -or EpParseError). Used for HTTP/1.0-style close-delimited bodies.
// -----------------------------------------------------------------------

#include "async/endpoint.hpp"
#include "wfx/async.hpp"

namespace WFX {

// -----------------------------------------------------------------------
// Core endpoint template
//
// Declare at namespace scope, before Run(). Each declaration occupies one-
// -slot in the connection pool described by EndpointConfig.
//
//   inline const auto MyEp = WFX::Endpoint<MyReq, MyRes>{ host, desc, cfg };
//
// With an onConnect coroutine (TLS auth, Redis AUTH, SMTP EHLO, etc.):
//   inline const auto MyEp = WFX::Endpoint<MyReq, MyRes, &MyOnConnect>{ ... };
// -----------------------------------------------------------------------
template <typename TReq, typename TRes, Async::UserOnConnectFn OnConnect = nullptr>
using Endpoint = Async::Resolve<TReq, TRes, OnConnect>;

// -----------------------------------------------------------------------
// RAII owner for the response returned by co_await ep.SendPayload()
//
// Access the response with * or -> or .get().
// Valid until the variable goes out of scope. Do not store the raw pointer.
// -----------------------------------------------------------------------
template <typename T> using EndpointOutput = Async::EndpointOutput<T>;

// -----------------------------------------------------------------------
// RAII owner of a connection pinned via ep.Reserve(), for protocols where-
// -consecutive requests must share one connection (SQL transactions,-
// -LISTEN/NOTIFY). Releases on destruction; check IsValid() first, since-
// -Reserve() returns an empty one when the pool is exhausted.
// -----------------------------------------------------------------------
template <typename TReq, typename TRes> using ReservedSlot = Async::ReservedSlot<TReq, TRes>;

// -----------------------------------------------------------------------
// Chunked consumption of a large response via ep.Stream(req). Hold the-
// -handle across the whole loop; each chunk's .data borrows engine memory-
// -and is only valid until the next Next(), which is what keeps peak-
// -memory at one chunk rather than the entire response.
// -----------------------------------------------------------------------
template <typename TReq, typename TRes> using StreamHandle = Async::StreamHandle<TReq, TRes>;
template <typename TRes> using StreamChunk = Async::StreamChunk<TRes>;

// -----------------------------------------------------------------------
// Passed into onConnect coroutines. Use handle.Send() and handle.Receive()-
// -to perform the handshake before the slot enters the pool.
// -----------------------------------------------------------------------
using SlotHandle = Async::SlotHandle;

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
// Protocol descriptor. Fill the fields your protocol needs, leave the-
// -rest null. serialize and parse are always required.
//
// Required:
//   .serialize(slotState, &req, buf, bufLen, &written)
//       Write the wire encoding of req into buf. Return EpSerOk on success,-
//       -EpSerBufferTooSmall if buf is too small (engine retries once with a-
//       -larger buffer), EpSerError on unrecoverable failure.
//
//   .parse(slotState, parseState, buf, len, &consumed, outObj, isEof)
//       Read arriving bytes and fill outObj. Set *consumed to the number-
//       -of bytes you read. Return EpParseDone or EpParseClose on a complete-
//       -message, EpParseIncomplete if you need more bytes, EpParseError on-
//       -failure. When isEof is true you must not return EpParseIncomplete.
//
// Nullable (omit or set to nullptr if not needed):
//   .onConnect                         -> set by Endpoint<> from the OnConnect template arg
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
//   .userCtx                           -> forwarded to createSlotState as its argument
// -----------------------------------------------------------------------
using EndpointDesc = Shared::EndpointDesc;

// -----------------------------------------------------------------------
// Connection pool settings
//
//   .connLimit              max simultaneous connections in the pool
//   .dnsRefreshSeconds      0 = respect DNS TTL, N = hard override
//   .connectTimeoutSeconds  TCP + TLS + onConnect must finish in this window
//   .requestTimeoutSeconds  serialize + send + parse must finish in this window
//   .idleTimeoutSeconds     idle slots are closed after this many seconds
//   .maxReconnectAttempts   max backoff retries before a slot is marked fatal
//   .reconnectBackoffBase   initial backoff in seconds
//   .reconnectBackoffMax    backoff cap in seconds
//   .tlsConfig              EpTlsAuto / EpTlsRequire / EpTlsInsecure
//   .prewarm                slots to connect eagerly on startup
// -----------------------------------------------------------------------
using EndpointConfig = Shared::EndpointConfig;

// -----------------------------------------------------------------------
// TLS mode (set in EndpointConfig::tlsConfig)
// -----------------------------------------------------------------------
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
// In onConnect, shared by co_await handle.Send(), .Receive().status and-
// -.UpgradeToTLS(). Anything other than EpSlotOk is fatal for the slot:-
// -co_return EpFatal
// -----------------------------------------------------------------------
inline constexpr auto EpSlotOk = Shared::SlotStatus::OK;
inline constexpr auto EpSlotBufferError = Shared::SlotStatus::BUFFER_ERROR;
inline constexpr auto EpSlotEpollError = Shared::SlotStatus::EPOLL_ERROR;
inline constexpr auto EpSlotIoError = Shared::SlotStatus::IO_ERROR;
inline constexpr auto EpSlotTlsError = Shared::SlotStatus::TLS_ERROR;
inline constexpr auto EpSlotInvalidState = Shared::SlotStatus::INVALID_STATE;

} // namespace WFX

#endif // WFX_INC_WFX_ENDPOINT_BASE_HPP
