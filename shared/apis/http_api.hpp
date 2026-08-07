// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_HTTP_API_HPP
#define WFX_SHARED_HTTP_API_HPP

#include "shared/json/json_object_fwd.hpp"
#include "shared/abis/constants.hpp"
#include "shared/abis/types.hpp"
#include "shared/abis/any.hpp"
#include "shared/abis/segment_variant.hpp"
#include "shared/abis/string_view.hpp"

// Fwd declare stuff
namespace WFX::Http {

class Router;
class HttpMiddleware;
class HttpConnectionHandler;

} // namespace WFX::Http

namespace WFX::Shared {

// Data internally used by Http API
struct HttpAPIDataExt1 {
    Http::Router* router = nullptr;
    Http::HttpMiddleware* middleware = nullptr;
    Http::HttpConnectionHandler* connHandler = nullptr;
    void* data = nullptr; // Any data type-erased
};

// vvv All aliases for clarity vvv
// Routing
using RegisterRouteFn = void (*)(HttpMethod, StringView path, RouteCallback);
using RegisterRouteExFn = void (*)(HttpMethod, StringView path, const MwCallback* mwStack, std::size_t mwStackSize,
                                   RouteCallback);
using PushRoutePrefixFn = void (*)(StringView prefix);
using PopRoutePrefixFn = void (*)();

// Middleware
using RegisterMiddlewareFn = void (*)(StringView name, MwCallback);

// Request Control
using GetMethodFn = HttpMethod (*)(const void* request);
using GetVersionFn = HttpVersion (*)(const void* request);
using GetPathFn = StringView (*)(const void* request);
using GetBodyFn = StringView (*)(const void* request);
using GetHeaderFn = bool (*)(const void* request, StringView key, StringView* outVal);
using GetSegmentCountFn = std::uint64_t (*)(const void* request);
using GetSegmentFn = Shared::SegmentVariant (*)(const void* request, std::uint64_t index);
using SetContextFn = void (*)(void* request, StringView key, Any value);
using GetContextFn = bool (*)(const void* request, StringView key, Any* outVal);
using EraseContextFn = void (*)(void* request, StringView key);

// Response Control
using SetStatusFn = void (*)(void* response, HttpStatus);
using SetHeaderFn = void (*)(void* response, StringView key, StringView value);
using WriteBodyFn = void (*)(void* response, StringView data);
using WriteFileFn = void (*)(void* response, StringView path, bool autoHandle404);
using WriteStreamFn = void (*)(void* response, StreamGenerator, bool chunked);
using WriteTemplateFn = void (*)(void* response, StringView path, JsonObject* ctx);
using CommitFn = void (*)(void* response);

// Data API
using SetGlobalPtrDataFn = void (*)(void*);
using GetGlobalPtrDataFn = void* (*)();

// vvv API declarations vvv
struct HttpAPIExt1 {
    // Routing
    RegisterRouteFn registerRoute;
    RegisterRouteExFn registerRouteEx;
    PushRoutePrefixFn pushRoutePrefix;
    PopRoutePrefixFn popRoutePrefix;

    // Middleware
    RegisterMiddlewareFn registerMiddleware;

    // Request Control
    GetMethodFn getMethod;
    GetVersionFn getVersion;
    GetPathFn getPath;
    GetBodyFn getBody;
    GetHeaderFn getHeader;
    GetSegmentCountFn getSegmentCount;
    GetSegmentFn getSegment;
    SetContextFn setContext;
    GetContextFn getContext;
    EraseContextFn eraseContext;

    // Response Control
    SetStatusFn setStatus;
    SetHeaderFn setHeader;
    WriteBodyFn writeBody;
    WriteFileFn writeFile;
    WriteStreamFn writeStream;
    WriteTemplateFn writeTemplate;
    CommitFn commit;

    // Data API
    SetGlobalPtrDataFn setGlobalPtrData;
    GetGlobalPtrDataFn getGlobalPtrData;
};
static_assert(std::is_standard_layout<HttpAPIExt1>::value, "'HTTP_API_EXT1' must be standard layout");

// Data internally used by Endpoint API
struct EndpointAPIDataExt1 {
    Http::HttpConnectionHandler* connHandler = nullptr;
};

using AllocateEndpointApiFn = std::uint16_t (*)(const char* host, EndpointDesc desc, EndpointConfig config);

// pinnedSlot is 0 to route through the pool as usual, else the request goes on that exact slot
using SendPayloadApiFn = EndpointStatus (*)(void* clientCtx, std::uint16_t endpointIdx, const void* req,
                                            AsyncData onComplete, std::uint64_t pinnedSlot);

// Pins one connection to the caller across several requests, for protocols where consecutive
// requests must share a connection (SQL transactions, LISTEN/NOTIFY, any sticky session). The
// handle packs endpointIdx, pool index, and the slot's generationId, so one outliving its slot
// (torn down and recycled) is detected rather than left dangling. 0 means reserve failed.
using ReserveSlotApiFn = std::uint64_t (*)(std::uint16_t endpointIdx);
using ReleaseSlotApiFn = void (*)(std::uint64_t pinnedSlot);

// Pulls the next chunk of a streamed response. req is the original request, handed back so
// cursor/paging protocols can serialize a continuation from it. Returns CHUNK_AVAILABLE when
// buffered bytes already held one (caller must not suspend), PENDING when it arrives later.
using StreamNextApiFn = EndpointStatus (*)(void* clientCtx, const void* req, AsyncData onComplete);

// Borrows the chunk streamNext just produced synchronously (CHUNK_AVAILABLE), which never went
// through a completion callback. Valid until the next streamNext on the same client.
using StreamChunkApiFn = const void* (*)(void* clientCtx);

// Slot-level operations, all valid only from inside an onConnect coroutine.
// slotUpgradeTls wraps a still-plaintext slot in TLS, for protocols that negotiate encryption
// in-band (Postgres SSLRequest, SMTP STARTTLS, ...) instead of at connect time.
// slotReceive's 'consumed' is how many bytes of the PREVIOUS Receive() result the caller already
// used; trimmed from the front of the read buffer before this read is armed, so a multi-round-trip
// handshake (STARTTLS EHLO/EHLO/AUTH, ...) never gets an earlier response redelivered.
using SlotSendApiFn = void (*)(void* endpointCtx, const void* data, std::uint32_t size, AsyncData onComplete);
using SlotReceiveApiFn = void (*)(void* endpointCtx, std::uint32_t consumed, AsyncData onComplete);
using SlotUpgradeTlsApiFn = void (*)(void* endpointCtx, AsyncData onComplete);

// Empty unless the slot is TLS and its handshake finished
using NegotiatedProtocolApiFn = StringView (*)(void* endpointCtx);

// Opens a second, throwaway connection to the same endpoint (Postgres CancelRequest, MySQL
// COM_PROCESS_KILL, ...). Valid from onConnect and onAbort alike; ownerCtx only resolves which
// endpoint's host/TLS config to dial. onComplete's data is the new slot's impl pointer.
using OpenSideConnectionApiFn = void (*)(void* ownerCtx, AsyncData onComplete);

struct EndpointAPIExt1 {
    AllocateEndpointApiFn allocateEndpoint;
    SendPayloadApiFn sendPayload;
    SlotSendApiFn slotSend;
    SlotReceiveApiFn slotReceive;
    SlotUpgradeTlsApiFn slotUpgradeTls;
    NegotiatedProtocolApiFn negotiatedProtocol;
    ReserveSlotApiFn reserveSlot;
    ReleaseSlotApiFn releaseSlot;
    StreamNextApiFn streamNext;
    StreamChunkApiFn streamChunk;
    OpenSideConnectionApiFn openSideConnection;
    EndpointSlotCloseFn closeSideConnection;
};
static_assert(std::is_standard_layout_v<EndpointAPIExt1>, "'ENDPOINT_API_EXT1' must be standard layout");

// vvv Getter & Initializers vvv
const HttpAPIExt1* GetHttpAPIExt1();
void InitHttpAPIExt1(Http::Router*, Http::HttpMiddleware*);

const EndpointAPIExt1* GetEndpointAPIExt1();
void InitEndpointAPIExt1(Http::HttpConnectionHandler* connHandler);

} // namespace WFX::Shared

#endif // WFX_SHARED_HTTP_API_HPP
