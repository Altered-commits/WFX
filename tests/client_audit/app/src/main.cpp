// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits
//
// Audit target for the shipped protocol clients, WFX::HttpEndpoint,
// WFX::SmtpEndpoint and WFX::PostgresEndpoint.
//
// Every route turns an inbound request into an outbound call on one of the three
// clients, against a hostile mock, and reflects the outcome back as JSON the
// harness asserts on. All three mocks are byte oracles rather than conformant
// servers: http_upstream.py replays exactly the bytes it was handed,
// smtp_upstream.py and postgres_upstream.py run the real handshake with one thing
// flipped per persona.
//
// Routes:
//   GET  /health        liveness
//   GET  /call          one HttpEndpoint call, everything chosen by X-* headers
//   POST /inject        feed the HTTP serializer a hostile path or header
//   POST /smtp/send     a full SMTP transaction, one command at a time
//   POST /smtp/send-mail  the same transaction through SmtpEndpoint::SendMail
//   POST /smtp/inject   feed the SMTP serializer a hostile field value
//   POST /pg/query      one pooled Postgres query
//   POST /pg/session    a pinned session: Begin, N statements, Commit or Rollback
//   POST /pg/stream     a chunked Postgres read through PostgresEndpoint::Stream
//   GET  /metrics       per-endpoint metrics table
//
// /call reflects:
//   { "ep": <EndpointStatus int>,         // 0 == SUCCESS, see shared/abis/types.hpp
//     "status": <upstream HTTP status>,   // the rest only when ep == 0
//     "bodylen": <response body length>,
//     "body": "<response body>",          // mock bodies are small ASCII
//     "hdr": "<value of the X-Want header>" }   // only if X-Want was sent
//
// and is driven header-only, so the harness never has to encode anything into a
// path it also wants to control byte for byte:
//
//   X-Ep      which HttpEndpoint instance, see EndpointOf   (default "default")
//   X-Method  GET | HEAD | OPTIONS | DELETE | POST | PUT | PATCH   (default GET)
//   X-Path    upstream request target                             (default "/ok")
//   X-Body    request body for POST/PUT/PATCH                      (optional)
//   X-Fwd / X-Fwd2 / X-Fwd3   headers to forward upstream, "Name: Value"
//   X-Want    upstream response header to echo back into "hdr"     (optional)
//
// The raw WFX::Endpoint<> primitive underneath both clients is audited separately,
// in tests/endpoint_audit.

#include <wfx/http.hpp>
#include <wfx/memory.hpp>
#include <wfx/telemetry.hpp>
#include <wfx/utils/hash.hpp>
#include <wfx/endpoint/http.hpp>
#include <wfx/endpoint/postgres.hpp>
#include <wfx/endpoint/smtp.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// An endpoint bakes host:port into the instance, so the HTTP mock is pinned at
// COMPILE time. The harness must launch http_upstream.py on this exact port; it is
// mirrored in client_audit.py as HTTP_PORT.
#define UPSTREAM "127.0.0.1:8091"

// WFX::HttpEndpoint instances
//
// They differ only in the knobs one group of vectors needs: caps small enough to
// trip on a few hundred bytes, budgets short enough to time out inside the run, and
// hosts that cannot be reached at all.

// The everyday client: roomy limits, plaintext, since the mock speaks cleartext
// HTTP/1.1.
inline const auto EpDefault = WFX::HttpEndpoint{UPSTREAM, WFX::HttpEndpointConfig{
    .connLimit             = 4,
    .requestTimeoutSeconds = 10,
    .tlsConfig             = WFX::EpTlsInsecure,
}};

// Tiny header, body and header-count caps, so limit enforcement is reachable with
// a few hundred bytes.
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

// Coalesces identical concurrent GETs into one backend call. The key is FNV-1a over
// the path, and a non-GET returns 0, which is how http.hpp spells "never coalesce
// this one".
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

// Points at a port with nothing listening, so connect() is refused with an RST and
// the request has to fail cleanly and quickly rather than hang the worker.
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

// WFX::SmtpEndpoint instances
//
// One per smtp_upstream.py persona. Both the port and the name are compiled in here
// and mirrored in client_audit.py's SMTP_PERSONAS table, keep the two in sync. The
// credentials match smtp_upstream.py's own defaults.
//
// Two budgets: SmtpCfg() gives 8s to personas that answer or refuse straight away,
// and SmtpCfgFast() drops that to 5s, the engine's floor, for the ones that never
// answer at all and can only be ended by the client's own timeout.
static WFX::SmtpEndpointConfig SmtpCfg(std::uint16_t connectTimeout = 8, std::uint16_t requestTimeout = 8) noexcept
{
    return WFX::SmtpEndpointConfig{
        .connLimit             = 4,
        .connectTimeoutSeconds = connectTimeout,
        .requestTimeoutSeconds = requestTimeout,
        .maxReconnectAttempts  = 0, // every call here is client-waited, background retry never applies
        .username              = "audituser",
        .password              = "audit-pass-123",
        .heloName              = "client-audit.wfx.test",
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

// WFX::PostgresEndpoint instances
//
// One per postgres_upstream.py persona. Ports are compiled in here and mirrored in
// client_audit.py's PG_PERSONAS table, keep the two in sync. None of these personas
// needs a certificate: every SSL-verdict fault (garbage byte, plaintext splice,
// plaintext-when-required) is refused by the client before a TLS handshake would
// ever start, so postgres_upstream.py never wraps the socket for them.
//
// PgCfg's defaults match postgres_upstream.py's SCRAM_USER/SCRAM_PASSWORD and skip TLS
// negotiation (PgEncryption::NONE) so most personas exercise only the StartupMessage
// and auth path, not the SSL verdict byte, which is what pg_ssl's own personas are for.
static WFX::PostgresConfig PgCfg(std::uint16_t connectTimeout = 8, std::uint16_t requestTimeout = 8,
                                 WFX::PgEncryption encryption = WFX::PgEncryption::NONE,
                                 WFX::PgAuthPolicy authPolicy = WFX::PgAuthPolicy::NO_PLAINTEXT,
                                 std::uint32_t maxMessageBytes = 16u * 1024u * 1024u) noexcept
{
    return WFX::PostgresConfig{
        .connLimit             = 2,
        .connectTimeoutSeconds = connectTimeout,
        .requestTimeoutSeconds = requestTimeout,
        .maxReconnectAttempts  = 0, // every call here is client-waited, background retry never applies
        .user                  = "audituser",
        .password              = "audit-pass-123",
        .database              = "audit",
        .encryption            = encryption,
        .authPolicy            = authPolicy,
        .maxMessageBytes       = maxMessageBytes,
        .statementCacheSize    = 8,
        .statementCacheMinUses = 2,
    };
}
// 5, not lower: see SmtpCfgFast, the same INVOKE_TIMEOUT_COOLDOWN floor applies here
static WFX::PostgresConfig PgCfgFast() noexcept
{
    return PgCfg(5, 5);
}

inline const auto Pg_good                  = WFX::PostgresEndpoint{"127.0.0.1:8130", PgCfg()};
inline const auto Pg_drop_startup          = WFX::PostgresEndpoint{"127.0.0.1:8131", PgCfg()};
inline const auto Pg_drop_auth_challenge   = WFX::PostgresEndpoint{"127.0.0.1:8132", PgCfg()};
inline const auto Pg_drop_auth_final       = WFX::PostgresEndpoint{"127.0.0.1:8133", PgCfg()};
inline const auto Pg_drop_backendkeydata   = WFX::PostgresEndpoint{"127.0.0.1:8134", PgCfg()};
inline const auto Pg_drop_ready            = WFX::PostgresEndpoint{"127.0.0.1:8135", PgCfg()};
inline const auto Pg_unknown_type_startup  = WFX::PostgresEndpoint{"127.0.0.1:8136", PgCfg()};
inline const auto Pg_flood_backendkeydata  = WFX::PostgresEndpoint{"127.0.0.1:8137", PgCfgFast()};
// maxMessageBytes small enough that FrameMessage's declared-length check rejects the
// oversized NoticeResponse without the mock having to actually move a huge payload
inline const auto Pg_huge_ready            = WFX::PostgresEndpoint{"127.0.0.1:8138", PgCfg(5, 5, WFX::PgEncryption::NONE, WFX::PgAuthPolicy::NO_PLAINTEXT, 4096)};
inline const auto Pg_never_reply_ready     = WFX::PostgresEndpoint{"127.0.0.1:8139", PgCfgFast()};

inline const auto Pg_ssl_garbage           = WFX::PostgresEndpoint{"127.0.0.1:8140", PgCfg(8, 8, WFX::PgEncryption::REQUIRED)};
inline const auto Pg_ssl_inject            = WFX::PostgresEndpoint{"127.0.0.1:8141", PgCfg(8, 8, WFX::PgEncryption::REQUIRED)};
inline const auto Pg_ssl_reject_required   = WFX::PostgresEndpoint{"127.0.0.1:8142", PgCfg(8, 8, WFX::PgEncryption::REQUIRED)};

inline const auto Pg_wrong_auth_md5        = WFX::PostgresEndpoint{"127.0.0.1:8143", PgCfg()};
inline const auto Pg_wrong_auth_gss        = WFX::PostgresEndpoint{"127.0.0.1:8144", PgCfg()};
inline const auto Pg_wrong_auth_sspi       = WFX::PostgresEndpoint{"127.0.0.1:8145", PgCfg()};
inline const auto Pg_wrong_auth_cleartext  = WFX::PostgresEndpoint{"127.0.0.1:8146", PgCfg()};
inline const auto Pg_cleartext_allowed     = WFX::PostgresEndpoint{"127.0.0.1:8147", PgCfg(8, 8, WFX::PgEncryption::NONE, WFX::PgAuthPolicy::ANY)};

inline const auto Pg_scram_offer_empty     = WFX::PostgresEndpoint{"127.0.0.1:8148", PgCfg()};
inline const auto Pg_scram_offer_plus_only = WFX::PostgresEndpoint{"127.0.0.1:8149", PgCfg()};
inline const auto Pg_scram_offer_garbage   = WFX::PostgresEndpoint{"127.0.0.1:8150", PgCfg()};
inline const auto Pg_scram_bad_nonce       = WFX::PostgresEndpoint{"127.0.0.1:8151", PgCfg()};
inline const auto Pg_scram_iter_zero       = WFX::PostgresEndpoint{"127.0.0.1:8152", PgCfg()};
inline const auto Pg_scram_iter_over       = WFX::PostgresEndpoint{"127.0.0.1:8153", PgCfg()};
inline const auto Pg_scram_bad_salt        = WFX::PostgresEndpoint{"127.0.0.1:8154", PgCfg()};
inline const auto Pg_scram_bad_signature   = WFX::PostgresEndpoint{"127.0.0.1:8155", PgCfg()};

inline const auto Pg_wire_stream           = WFX::PostgresEndpoint{"127.0.0.1:8156", PgCfg()};
inline const auto Pg_cache_epoch_feature   = WFX::PostgresEndpoint{"127.0.0.1:8157", PgCfg()};
inline const auto Pg_cache_epoch_badname   = WFX::PostgresEndpoint{"127.0.0.1:8158", PgCfg()};
inline const auto Pg_cancel_probe          = WFX::PostgresEndpoint{"127.0.0.1:8159", PgCfgFast()};
// maxMessageBytes small enough that PGAUDIT_HUGE_ROW's oversized field trips FrameMessage's
// MALFORMED bound instead of actually moving 20MB over the loopback socket
inline const auto Pg_resource_huge_row     = WFX::PostgresEndpoint{"127.0.0.1:8160", PgCfg(8, 8, WFX::PgEncryption::NONE, WFX::PgAuthPolicy::NO_PLAINTEXT, 4096)};
inline const auto Pg_resource_slow_trickle = WFX::PostgresEndpoint{"127.0.0.1:8161", PgCfgFast()};

// applicationName carries an embedded NUL. WriteStartup's CStr() writes the raw bytes
// verbatim plus its own terminator, with no screening for one already inside the
// string, so this proves whether that desyncs the startup packet's parameter list
// (see connection.hpp's WriteStartup; SMTP's heloinject persona is the same class
// of check against SmtpOnConnect's HasInjectionBytes, which Postgres has no equivalent of)
inline const auto Pg_startup_nul_inject = WFX::PostgresEndpoint{"127.0.0.1:8162", WFX::PostgresConfig{
    .connLimit             = 2,
    .connectTimeoutSeconds = 8,
    .requestTimeoutSeconds = 8,
    .maxReconnectAttempts  = 0,
    .user                  = "audituser",
    .password              = "audit-pass-123",
    .database              = "audit",
    .applicationName       = std::string_view("evil\0trailing", 13),
    .encryption            = WFX::PgEncryption::NONE,
    .authPolicy            = WFX::PgAuthPolicy::NO_PLAINTEXT,
}};

// maxReconnectAttempts >= 1 so a first connection the mock refuses gets retried
// automatically. Proves the retried connection's SlotState starts genuinely fresh
// rather than carrying anything over from the failed attempt (CVE-2018-10915 class)
inline const auto Pg_reconnect_isolation = WFX::PostgresEndpoint{"127.0.0.1:8163", WFX::PostgresConfig{
    .connLimit                   = 1,
    .connectTimeoutSeconds       = 8,
    .requestTimeoutSeconds       = 8,
    .maxReconnectAttempts        = 2,
    .reconnectBackoffBaseSeconds = 1,
    .reconnectBackoffMaxSeconds  = 1,
    .user                        = "audituser",
    .password                    = "audit-pass-123",
    .database                    = "audit",
    .encryption                  = WFX::PgEncryption::NONE,
    .authPolicy                  = WFX::PgAuthPolicy::NO_PLAINTEXT,
}};

inline const auto Pg_scram_mixed          = WFX::PostgresEndpoint{"127.0.0.1:8164", PgCfg()};
inline const auto Pg_scram_server_error   = WFX::PostgresEndpoint{"127.0.0.1:8165", PgCfg()};
inline const auto Pg_error_at_handshake   = WFX::PostgresEndpoint{"127.0.0.1:8166", PgCfg()};

static const WFX::PostgresEndpoint* PgEndpointOf(std::string_view e) noexcept
{
    if(e == "drop_startup")          return &Pg_drop_startup;
    if(e == "drop_auth_challenge")   return &Pg_drop_auth_challenge;
    if(e == "drop_auth_final")       return &Pg_drop_auth_final;
    if(e == "drop_backendkeydata")   return &Pg_drop_backendkeydata;
    if(e == "drop_ready")            return &Pg_drop_ready;
    if(e == "unknown_type_startup")  return &Pg_unknown_type_startup;
    if(e == "flood_backendkeydata")  return &Pg_flood_backendkeydata;
    if(e == "huge_ready")            return &Pg_huge_ready;
    if(e == "never_reply_ready")     return &Pg_never_reply_ready;
    if(e == "ssl_garbage")           return &Pg_ssl_garbage;
    if(e == "ssl_inject")            return &Pg_ssl_inject;
    if(e == "ssl_reject_required")   return &Pg_ssl_reject_required;
    if(e == "wrong_auth_md5")        return &Pg_wrong_auth_md5;
    if(e == "wrong_auth_gss")        return &Pg_wrong_auth_gss;
    if(e == "wrong_auth_sspi")       return &Pg_wrong_auth_sspi;
    if(e == "wrong_auth_cleartext")  return &Pg_wrong_auth_cleartext;
    if(e == "cleartext_allowed")     return &Pg_cleartext_allowed;
    if(e == "scram_offer_empty")     return &Pg_scram_offer_empty;
    if(e == "scram_offer_plus_only") return &Pg_scram_offer_plus_only;
    if(e == "scram_offer_garbage")   return &Pg_scram_offer_garbage;
    if(e == "scram_bad_nonce")       return &Pg_scram_bad_nonce;
    if(e == "scram_iter_zero")       return &Pg_scram_iter_zero;
    if(e == "scram_iter_over")       return &Pg_scram_iter_over;
    if(e == "scram_bad_salt")        return &Pg_scram_bad_salt;
    if(e == "scram_bad_signature")   return &Pg_scram_bad_signature;
    if(e == "wire_stream")           return &Pg_wire_stream;
    if(e == "cache_epoch_feature")   return &Pg_cache_epoch_feature;
    if(e == "cache_epoch_badname")   return &Pg_cache_epoch_badname;
    if(e == "cancel_probe")          return &Pg_cancel_probe;
    if(e == "resource_huge_row")     return &Pg_resource_huge_row;
    if(e == "resource_slow_trickle") return &Pg_resource_slow_trickle;
    if(e == "startup_nul_inject")    return &Pg_startup_nul_inject;
    if(e == "reconnect_isolation")   return &Pg_reconnect_isolation;
    if(e == "scram_mixed")           return &Pg_scram_mixed;
    if(e == "scram_server_error")    return &Pg_scram_server_error;
    if(e == "error_at_handshake")    return &Pg_error_at_handshake;
    return &Pg_good;
}

// Reflecting the outcome

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

// EndpointStatus -> the plain integer JsonWriter has an overload for
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

// Reflects row 0, column 0 of a Postgres result as one JSON "value" string, whatever
// its wire type. Reduces every codec to text so the harness can assert with plain
// string comparisons instead of a JSON field per possible C++ type. Arrays are the one
// exception: the decoded shape and Count() are what pg_types actually needs to see,
// not a stringified element list, so those get their own three fields instead.
static void WritePgValue(WFX::Shared::JsonWriter& j, const WFX::PgResult& result, const WFX::PgRow& row,
                         std::uint16_t col)
{
    if(row.IsNull(col)) {
        j.Write("value_null", true);
        j.Write("value", std::string_view{});
        return;
    }
    j.Write("value_null", false);

    const std::uint32_t oid = result.Column(col).typeOid;
    char buf[64];

    switch(oid) {
        case 20: case 21: case 23: case 26: { // int8, int2, int4, oid
            const auto v = row.Get<std::int64_t>(col);
            const int n = std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
            j.Write("value", std::string_view{buf, static_cast<std::size_t>(n)});
            break;
        }
        case 1700: { // numeric
            const double d = row.Get<WFX::PgNumeric>(col).ToDouble();
            const int n = std::snprintf(buf, sizeof(buf), "%g", d);
            j.Write("value", std::string_view{buf, static_cast<std::size_t>(n)});
            break;
        }
        case 1007: { // int4[]
            // Get<PgArrayView> leaves elements null on a decode failure (a malformed
            // header) and sets it, even for a genuinely empty array, on success, so it
            // doubles as the ok/fail signal DecodeArray's own bool return would give
            const auto av = row.Get<WFX::PgArrayView>(col);
            const bool ok = av.elements != nullptr;
            j.Write("array_ok", ok);
            j.Write("array_ndim", static_cast<std::int64_t>(av.ndim));
            j.Write("array_count", static_cast<std::int64_t>(ok ? av.Count() : 0));

            // Walking is a second, independent check from Count(): a header that
            // overflows Count()'s product can still legitimately have zero walkable
            // bytes behind it, and a truncated element has to stop the walk rather
            // than read past what actually arrived
            std::int64_t walked = 0;
            bool anyNull = false;
            if(ok) {
                const char* cursor = av.elements;
                std::string_view elem;
                bool isNull = false;
                while(av.NextElement(cursor, elem, isNull)) {
                    ++walked;
                    anyNull = anyNull || isNull;
                }
            }
            j.Write("array_walked", walked);
            j.Write("array_any_null", anyNull);
            j.Write("value", std::string_view{"array"});
            break;
        }
        default:
            j.Write("value", row.Get<std::string_view>(col));
            break;
    }
}

static WFX::PgIsolation IsolationOf(std::string_view s) noexcept
{
    if(s == "repeatable_read") return WFX::PgIsolation::REPEATABLE_READ;
    if(s == "serializable")    return WFX::PgIsolation::SERIALIZABLE;
    return WFX::PgIsolation::READ_COMMITTED;
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
// header. The client must refuse it (EpSerializeError), never emit a request that
// smuggles a second header or line upstream.
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

// Same personas and headers as /smtp/send, but drives the whole exchange through
// SmtpEndpoint::SendMail instead of calling MailFrom/RcptTo/DataStart/DataBody by hand.
// SendMail returns a WFX::Coro that this handler co_awaits, so it is what proves one
// coroutine can be awaited from another end to end rather than merely compile.
//
//   { "ep": <EndpointStatus int>, "code": <SMTP reply code, 0 if none arrived>, "success": <bool> }
WFX_POST("/smtp/send-mail", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
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

    WFX::SmtpSendOutcome outcome;
    co_await ep->SendMail(fromAddr, fromName, toAddr, toName, subject, body, outcome, replyTo);

    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("ep", EpJ(outcome.status));
    j.Write("code", outcome.response ? outcome.response->code : 0);
    j.Write("success", outcome.Success());
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

// Postgres client: one pooled query.
//
//   X-Persona  which PgEndpointOf() persona (default "good")
//   X-Param    if present, bound as the query's single $1 parameter instead of
//              leaving it parameterless; lets a caller send SQL metacharacters as
//              data and prove they never reach the SQL text (see pg_injection)
//   body       the SQL text; a PGAUDIT_<NAME> marker anywhere in it selects a hostile
//              postgres_upstream.py response shape instead of the default good row
//
//   { "ep": <EndpointStatus int>,           // the rest only when ep == 0
//     "failed": <bool>,                     // res->Failed(), an ErrorResponse came back
//     "sqlstate": str, "message": str,       // only when failed
//     "rows": n, "cols": n,                  // only when not failed
//     "value": str, "value_null": bool,      // row 0, column 0, see WritePgValue
//     "array_ok": bool, "array_ndim": n, "array_count": n,   // column 0 only, arrays
//     "array_walked": n, "array_any_null": bool }            // elements NextElement actually stepped
WFX_POST("/pg/query", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view personaName = "good";
    req.GetHeader("X-Persona", personaName);
    std::string_view sql = req.Body();
    std::string_view param;
    const bool hasParam = req.GetHeader("X-Param", param);

    const WFX::PostgresEndpoint* ep = PgEndpointOf(personaName);

    res.Status(200);
    auto j = WFX::ImJson(res);

    auto emit = [&j](WFX::Shared::EndpointStatus status, const auto& result) {
        j.Write("ep", EpJ(status));
        if(status != WFX::EpOk)
            return;

        j.Write("failed", result->Failed());
        if(result->Failed()) {
            j.Write("sqlstate", result->Error().SqlState());
            j.Write("message", result->Error().Message());
            return;
        }

        j.Write("rows", static_cast<std::uint64_t>(result->RowCount()));
        j.Write("cols", static_cast<std::uint64_t>(result->ColumnCount()));
        j.Write("affected_rows", result->AffectedRows());
        if(result->RowCount() > 0 && result->ColumnCount() > 0)
            WritePgValue(j, *result, result->At(0), 0);
    };

    if(hasParam) {
        auto [status, result] = co_await ep->Query(sql, param);
        emit(status, result);
    }
    else {
        auto [status, result] = co_await ep->Query(sql);
        emit(status, result);
    }
    co_return;
})

// Postgres client: one pinned session, for everything that needs the same connection
// across statements (transactions, savepoints, the statement cache).
//
//   X-Persona    which PgEndpointOf() persona (default "good")
//   X-Isolation  read_committed | repeatable_read | serializable | none  (default
//                read_committed; "none" skips Begin, so every statement still runs on
//                the same connection, just outside a transaction)
//   X-Finish     commit | rollback | none  (default commit)
//   body         one statement per line, run in order. A line starting with
//                "SAVEPOINT ", "ROLLBACK TO SAVEPOINT " or "RELEASE SAVEPOINT " runs
//                through PgSession's matching call instead of Query, with everything
//                after the keyword as the name; anything else runs through Query
//
//   { "ep": <EndpointStatus int>,          // EpPoolExhausted if the pool had nothing to reserve
//     "begin_ep": <EndpointStatus int>,    // only when X-Isolation != none
//     "steps": [ { "ep": int, "failed": bool, "sqlstate": str, "rows": n }, ... ],
//     "finish_ep": <EndpointStatus int>, "finish_failed": bool }  // only when X-Finish != none
WFX_POST("/pg/session", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view personaName = "good";
    req.GetHeader("X-Persona", personaName);
    std::string_view isolation = "read_committed";
    req.GetHeader("X-Isolation", isolation);
    std::string_view finish = "commit";
    req.GetHeader("X-Finish", finish);
    std::string_view body = req.Body();

    const WFX::PostgresEndpoint* ep = PgEndpointOf(personaName);
    auto tx = ep->Session();

    res.Status(200);
    auto j = WFX::ImJson(res);

    if(!tx.IsValid()) {
        j.Write("ep", EpJ(WFX::EpPoolExhausted));
        co_return;
    }
    j.Write("ep", EpJ(WFX::EpOk));

    if(isolation != "none") {
        auto [bstatus, bout] = co_await tx.Begin(IsolationOf(isolation));
        (void)bout;
        j.Write("begin_ep", EpJ(bstatus));
        if(bstatus != WFX::EpOk)
            co_return;
    }

    j.Arr("steps");
    std::size_t pos = 0;
    while(pos < body.size()) {
        const std::size_t nl = body.find('\n', pos);
        const std::string_view line = body.substr(pos, (nl == std::string_view::npos ? body.size() : nl) - pos);
        pos = (nl == std::string_view::npos) ? body.size() : nl + 1;
        if(line.empty())
            continue;

        // Real savepoint syntax as the line prefix, so a hostile name is exactly
        // whatever follows it. name never travels as a parameter: PgSession::Savepoint
        // composes it into a plain-Query statement, guarded only by IsValidIdentifier
        // (wire.hpp), which is what these lines exist to prove holds against SQL
        // metacharacters, not just clean identifiers.
        j.Obj();
        if(line.rfind("SAVEPOINT ", 0) == 0) {
            auto [status, result] = co_await tx.Savepoint(line.substr(10));
            j.Write("ep", EpJ(status));
            if(status == WFX::EpOk) {
                j.Write("failed", result->Failed());
                if(result->Failed())
                    j.Write("sqlstate", result->Error().SqlState());
            }
        }
        else if(line.rfind("ROLLBACK TO SAVEPOINT ", 0) == 0) {
            auto [status, result] = co_await tx.RollbackTo(line.substr(22));
            j.Write("ep", EpJ(status));
            if(status == WFX::EpOk) {
                j.Write("failed", result->Failed());
                if(result->Failed())
                    j.Write("sqlstate", result->Error().SqlState());
            }
        }
        else if(line.rfind("RELEASE SAVEPOINT ", 0) == 0) {
            auto [status, result] = co_await tx.ReleaseSavepoint(line.substr(19));
            j.Write("ep", EpJ(status));
            if(status == WFX::EpOk) {
                j.Write("failed", result->Failed());
                if(result->Failed())
                    j.Write("sqlstate", result->Error().SqlState());
            }
        }
        else {
            auto [status, result] = co_await tx.Query(line);
            j.Write("ep", EpJ(status));
            if(status == WFX::EpOk) {
                j.Write("failed", result->Failed());
                j.Write("rows", static_cast<std::uint64_t>(result->RowCount()));
                if(result->Failed()) {
                    j.Write("sqlstate", result->Error().SqlState());
                    j.Write("message", result->Error().Message());
                }
            }
        }
        j.End();
    }
    j.End();

    if(finish == "rollback") {
        auto [fstatus, fout] = co_await tx.Rollback();
        j.Write("finish_ep", EpJ(fstatus));
        j.Write("finish_failed", fstatus == WFX::EpOk && fout->Failed());
    }
    else if(finish != "none") {
        auto [fstatus, fout] = co_await tx.Commit();
        j.Write("finish_ep", EpJ(fstatus));
        j.Write("finish_failed", fstatus == WFX::EpOk && fout->Failed());
    }
    co_return;
})

// Postgres client: a chunked read through PostgresEndpoint::Stream, holding the
// connection until the portal is exhausted.
//
//   X-Persona    which PgEndpointOf() persona (default "good")
//   X-ChunkRows  rows fetched per round (default 2)
//   body         the SQL text, normally a PGAUDIT_STREAM query against wire_stream
//
//   { "ep": <EndpointStatus int>,      // the status of whichever round ended the stream
//     "failed": <bool>, "sqlstate": str,  // only when a chunk carried an ErrorResponse
//     "chunks": n, "rows": n }
WFX_POST("/pg/stream", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view personaName = "good";
    req.GetHeader("X-Persona", personaName);
    const std::uint32_t chunkRows = HeaderU32(req, "X-ChunkRows", 2);
    std::string_view sql = req.Body();

    const WFX::PostgresEndpoint* ep = PgEndpointOf(personaName);
    auto stream = ep->Stream(chunkRows, sql);

    res.Status(200);
    auto j = WFX::ImJson(res);

    std::uint64_t chunks = 0, rows = 0;
    while(true) {
        auto chunk = co_await stream.Next();
        if(chunk.status != WFX::EpOk) {
            j.Write("ep", EpJ(chunk.status));
            j.Write("chunks", chunks);
            j.Write("rows", rows);
            co_return;
        }
        if(chunk.done)
            break;

        ++chunks;
        rows += chunk.data->RowCount();
        if(chunk.data->Failed()) {
            j.Write("ep", EpJ(WFX::EpOk));
            j.Write("failed", true);
            j.Write("sqlstate", chunk.data->Error().SqlState());
            j.Write("chunks", chunks);
            j.Write("rows", rows);
            co_return;
        }
    }

    j.Write("ep", EpJ(WFX::EpOk));
    j.Write("failed", false);
    j.Write("chunks", chunks);
    j.Write("rows", rows);
    co_return;
})

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
