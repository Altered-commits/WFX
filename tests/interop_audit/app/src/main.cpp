// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits
//
// Audit target for interop_audit: the shipped protocol clients, WFX::PostgresEndpoint,
// WFX::SmtpEndpoint and WFX::HttpEndpoint, driven against real, spec-compliant upstreams
// (Docker Postgres, Docker smtp4dev x2, and a second real WFX server acting as the HTTP
// upstream), every leg on real TLS. Not the hand-rolled hostile mocks client_audit uses:
// this suite proves interop with the real thing, not just that the parser survives abuse.
//
// Routes:
//   GET  /health          liveness, polled by the harness before driving anything
//   GET  /pg/select-one   connect + SCRAM auth + query sanity, touching int/bool/text
//   POST /pg/roundtrip    real INSERT ... RETURNING against real disk-backed storage
//   POST /pg/tx           a real transaction, commit or rollback, checked from a second
//                          pooled connection afterward, real atomicity, not a mock's word for it
//   POST /pg/savepoint    SAVEPOINT / ROLLBACK TO / COMMIT, checked the same way
//   POST /pg/stream       a chunked read via PostgresEndpoint::Stream against real Postgres
//   POST /smtp/send       full MAIL/RCPT(xN)/DATA transaction, STARTTLS + AUTH PLAIN or LOGIN
//   POST /smtp/send-mail  the same shape through SmtpEndpoint::SendMail's single-call wrapper
//   POST /smtp/reset      RSET mid-transaction, then a fresh transaction on the same connection
//   GET  /http/call       one HttpEndpoint call against the real HTTPS upstream

#include <wfx/http.hpp>
#include <wfx/memory.hpp>
#include <wfx/endpoint/http.hpp>
#include <wfx/endpoint/postgres.hpp>
#include <wfx/endpoint/smtp.hpp>

#include <cstdint>
#include <string_view>
#include <tuple>

// vvv Real upstreams (ports match docker-compose.yml / upstream/) vvv
// clang-format off

// Enterprise-shaped pool: real concurrency headroom, a real reconnect/backoff policy, TLS
// required (not preferred), SCRAM only, session-level statement/lock/idle-in-tx timeouts set
// rather than left server-default, application_name so this connection is identifiable in
// pg_stat_activity like any real service would want.
inline const auto Db = WFX::PostgresEndpoint{"127.0.0.1:5533", WFX::PostgresConfig{
    .connLimit                  = 16,
    .auxConnLimit                = 4,
    .connectTimeoutSeconds       = 8,
    .requestTimeoutSeconds       = 20,
    .maxReconnectAttempts        = 3,
    .reconnectBackoffBaseSeconds = 1,
    .reconnectBackoffMaxSeconds  = 10,
    .prewarm                     = 4,
    .user                        = "interop",
    .password                    = "interop-pass",
    .database                    = "interop",
    .applicationName             = "wfx-interop-audit",
    .encryption                  = WFX::PgEncryption::REQUIRED,
    .authPolicy                  = WFX::PgAuthPolicy::SCRAM_ONLY,
    .statementTimeoutMs          = 15000,
    .lockTimeoutMs               = 5000,
    .idleInTransactionTimeoutMs  = 30000,
    .statementCacheSize          = 64,
    .statementCacheMinUses       = 2,
}};

// AUTH PLAIN preferred path: smtp4dev's default offers PLAIN, so this is the everyday case.
inline const auto Mail = WFX::SmtpEndpoint{"127.0.0.1:2525", WFX::SmtpEndpointConfig{
    .connLimit             = 4,
    .connectTimeoutSeconds = 10,
    .requestTimeoutSeconds = 15,
    .maxReconnectAttempts  = 3,
    .prewarm                = 1,
    .username              = "interop",
    .password              = "interop-pass",
    .heloName              = "wfx-interop.test",
}};

// Only LOGIN is offered here, which forces the fallback path AUTH PLAIN would otherwise
// always take instead.
inline const auto MailLoginOnly = WFX::SmtpEndpoint{"127.0.0.1:2526", WFX::SmtpEndpointConfig{
    .connLimit             = 2,
    .connectTimeoutSeconds = 10,
    .requestTimeoutSeconds = 15,
    .username              = "interop",
    .password              = "interop-pass",
    .heloName              = "wfx-interop.test",
}};

// The real HTTP upstream: real TLS required, never insecure. outbound_ca_path (patched in by
// interop_audit.py's setup()) is what makes this cert verification succeed rather than fail.
inline const auto Web = WFX::HttpEndpoint{"127.0.0.1:8443", WFX::HttpEndpointConfig{
    .connLimit              = 16,
    .connectTimeoutSeconds  = 8,
    .requestTimeoutSeconds  = 15,
    .maxReconnectAttempts   = 3,
    .tlsConfig              = WFX::EpTlsRequire,
    .prewarm                = 2,
}};
// clang-format on

static const WFX::SmtpEndpoint* MailEndpointOf(std::string_view e) noexcept
{
    return e == "login" ? &MailLoginOnly : &Mail;
}

// Small unsigned header values (row counts, chunk sizes). No std::string involved: header
// values are plain string_views into the request buffer, parsed digit by digit.
static std::int64_t HeaderI64(WFX::Request& req, const char* name, std::int64_t fallback) noexcept
{
    std::string_view sv;
    if(!req.GetHeader(name, sv) || sv.empty())
        return fallback;

    std::int64_t v = 0;
    for(char c : sv) {
        if(c < '0' || c > '9')
            return fallback;
        v = v * 10 + (c - '0');
    }
    return v;
}

// EndpointStatus -> the plain integer JsonWriter has an overload for
static std::uint64_t EpJ(WFX::EndpointStatus s) noexcept
{
    return static_cast<std::uint64_t>(static_cast<unsigned>(s));
}

WFX_GET("/health", [](WFX::Request req, WFX::Response res) {
    res.Status(200).SendText("ok");
})

// Postgres: connect + SCRAM auth + query sanity, touching int/bool/text in one row
WFX_GET("/pg/select-one", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    res.Status(200);
    auto j = WFX::ImJson(res);

    auto [status, result] = co_await Db.Query("SELECT 1::int AS one, true AS flag, 'hi'::text AS txt");
    j.Write("ep", EpJ(status));
    if(status != WFX::EpOk)
        co_return;

    j.Write("failed", result->Failed());
    if(result->Failed()) {
        j.Write("sqlstate", result->Error().SqlState());
        j.Write("message", result->Error().Message());
        co_return;
    }

    auto row = result->At(0);
    j.Write("one", row.Get<std::int64_t>("one"));
    j.Write("flag", row.Get<bool>("flag"));
    j.Write("txt", row.Get<std::string_view>("txt"));
    co_return;
})

// Postgres: a real INSERT ... RETURNING against real disk-backed storage, proving the full
// encode/network/decode round trip, not just an echoed bind parameter. body is the value.
WFX_POST("/pg/roundtrip", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view val = req.Body();

    res.Status(200);
    auto j = WFX::ImJson(res);

    auto [createStatus, createResult] =
        co_await Db.Query("CREATE TABLE IF NOT EXISTS interop_roundtrip (id BIGSERIAL PRIMARY KEY, val TEXT NOT NULL)");
    j.Write("create_ep", EpJ(createStatus));
    if(createStatus != WFX::EpOk || createResult->Failed())
        co_return;

    auto [status, result] = co_await Db.Query("INSERT INTO interop_roundtrip(val) VALUES ($1) RETURNING id, val", val);
    j.Write("ep", EpJ(status));
    if(status != WFX::EpOk)
        co_return;

    j.Write("failed", result->Failed());
    if(result->Failed()) {
        j.Write("sqlstate", result->Error().SqlState());
        j.Write("message", result->Error().Message());
        co_return;
    }

    auto row = result->At(0);
    j.Write("id", row.Get<std::int64_t>("id"));
    j.Write("val", row.Get<std::string_view>("val"));
    co_return;
})

// Postgres: a real transaction, committed or rolled back, then checked from a *second*,
// freshly pooled connection, real atomicity a mock cannot fake either way.
//
//   X-Isolation  read_committed | repeatable_read | serializable  (default read_committed)
//   X-Finish     commit | rollback                                 (default commit)
//   body         the value to insert, unique per call so the visibility check is unambiguous
WFX_POST("/pg/tx", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view isolation = "read_committed", finish = "commit";
    req.GetHeader("X-Isolation", isolation);
    req.GetHeader("X-Finish", finish);
    std::string_view val = req.Body();

    const auto iso = isolation == "repeatable_read"  ? WFX::PgIsolation::REPEATABLE_READ
                     : isolation == "serializable"    ? WFX::PgIsolation::SERIALIZABLE
                                                       : WFX::PgIsolation::READ_COMMITTED;

    res.Status(200);
    auto j = WFX::ImJson(res);

    auto tx = Db.Session();
    if(!tx.IsValid()) {
        j.Write("tx_ep", EpJ(WFX::EpPoolExhausted));
        co_return;
    }

    auto [beginStatus, beginResult] = co_await tx.Begin(iso);
    j.Write("begin_ep", EpJ(beginStatus));
    if(beginStatus != WFX::EpOk)
        co_return;

    auto [createStatus, createResult] =
        co_await tx.Query("CREATE TABLE IF NOT EXISTS interop_tx (id BIGSERIAL PRIMARY KEY, val TEXT NOT NULL)");
    if(createStatus != WFX::EpOk || createResult->Failed()) {
        j.Write("tx_ep", EpJ(createStatus));
        co_return;
    }

    auto [insStatus, insResult] = co_await tx.Query("INSERT INTO interop_tx(val) VALUES ($1)", val);
    j.Write("tx_ep", EpJ(insStatus));

    WFX::EndpointStatus finishStatus;
    if(finish == "rollback")
        finishStatus = (co_await tx.Rollback()).first;
    else
        finishStatus = (co_await tx.Commit()).first;
    j.Write("finish_ep", EpJ(finishStatus));

    // tx drops here, releasing the pinned connection back to the pool. The COUNT below
    // deliberately runs on a *different* connection: the whole point is proving the commit or
    // rollback is durable and visible to someone who was never part of this transaction.
    auto [checkStatus, checkResult] = co_await Db.Query("SELECT COUNT(*) AS n FROM interop_tx WHERE val = $1", val);
    j.Write("check_ep", EpJ(checkStatus));
    if(checkStatus == WFX::EpOk && !checkResult->Failed())
        j.Write("visible_count", checkResult->At(0).Get<std::int64_t>("n"));
    co_return;
})

// Postgres: SAVEPOINT / ROLLBACK TO / COMMIT, checked the same way as /pg/tx. body is a unique
// marker; rows "<marker>-a" and "<marker>-b" are inserted, b's insert is undone by RollbackTo.
WFX_POST("/pg/savepoint", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view marker = req.Body();

    res.Status(200);
    auto j = WFX::ImJson(res);

    auto tx = Db.Session();
    if(!tx.IsValid()) {
        j.Write("tx_ep", EpJ(WFX::EpPoolExhausted));
        co_return;
    }

    if((co_await tx.Begin()).first != WFX::EpOk) {
        j.Write("tx_ep", EpJ(WFX::EpInternalError));
        co_return;
    }

    auto [createStatus, createResult] = co_await tx.Query(
        "CREATE TABLE IF NOT EXISTS interop_savepoint (id BIGSERIAL PRIMARY KEY, val TEXT NOT NULL)");
    if(createStatus != WFX::EpOk || createResult->Failed()) {
        j.Write("tx_ep", EpJ(createStatus));
        co_return;
    }

    WFX::String valA{marker};
    valA += "-a";
    WFX::String valB{marker};
    valB += "-b";

    co_await tx.Query("INSERT INTO interop_savepoint(val) VALUES ($1)", std::string_view{valA});

    auto [spStatus, spResult] = co_await tx.Savepoint("sp_interop");
    j.Write("savepoint_ep", EpJ(spStatus));

    co_await tx.Query("INSERT INTO interop_savepoint(val) VALUES ($1)", std::string_view{valB});

    auto [rbStatus, rbResult] = co_await tx.RollbackTo("sp_interop");
    j.Write("rollback_to_ep", EpJ(rbStatus));

    auto [commitStatus, commitResult] = co_await tx.Commit();
    j.Write("commit_ep", EpJ(commitStatus));

    auto [checkStatus, checkResult] =
        co_await Db.Query("SELECT val FROM interop_savepoint WHERE val = $1 OR val = $2 ORDER BY val",
                          std::string_view{valA}, std::string_view{valB});
    j.Write("check_ep", EpJ(checkStatus));
    if(checkStatus == WFX::EpOk && !checkResult->Failed()) {
        j.Write("rows", static_cast<std::uint64_t>(checkResult->RowCount()));
        if(checkResult->RowCount() > 0)
            j.Write("first_val", checkResult->At(0).Get<std::string_view>("val"));
    }
    co_return;
})

// Postgres: a chunked read via PostgresEndpoint::Stream against real Postgres, using
// generate_series so no fixture table is needed. X-Rows total rows, X-ChunkRows per chunk.
WFX_POST("/pg/stream", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    const std::int64_t rows = HeaderI64(req, "X-Rows", 100);
    const auto chunkRows = static_cast<std::uint32_t>(HeaderI64(req, "X-ChunkRows", 10));

    res.Status(200);
    auto j = WFX::ImJson(res);

    auto stream = Db.Stream(chunkRows, "SELECT generate_series(1, $1) AS n", rows);

    std::int64_t seen = 0, sum = 0;
    std::uint32_t chunks = 0;
    WFX::EndpointStatus lastStatus = WFX::EpOk;

    while(true) {
        auto chunk = co_await stream.Next();
        lastStatus = chunk.status;
        if(chunk.status != WFX::EpOk || chunk.done)
            break;

        ++chunks;
        for(std::uint32_t i = 0; i < chunk.data->RowCount(); i++)
            sum += chunk.data->At(i).Get<std::int64_t>("n");
        seen += chunk.data->RowCount();
    }

    j.Write("ep", EpJ(lastStatus));
    j.Write("rows_seen", static_cast<std::uint64_t>(seen));
    j.Write("chunks", static_cast<std::uint64_t>(chunks));
    j.Write("sum", sum);
    co_return;
})

// SMTP: a full transaction over a real STARTTLS + AUTH handshake, delivered to smtp4dev.
// Supports up to three envelope recipients (RCPT TO run before DATA), so multi-recipient
// delivery is exercised too, not just the single-To shape.
//
//   X-Endpoint  "plain" (default, AUTH PLAIN) | "login" (forces AUTH LOGIN)
//   X-From / X-To / X-To2 / X-To3 / X-Subject / X-ReplyTo   optional, sane defaults
//   body        the message body, dot-stuffing (a line starting with '.') included on purpose
WFX_POST("/smtp/send", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view epName = "plain";
    std::string_view fromAddr = "sender@wfx-interop.test", toAddr = "recipient@wfx-interop.test",
                     toAddr2{}, toAddr3{}, subject = "interop", replyTo{};
    req.GetHeader("X-Endpoint", epName);
    req.GetHeader("X-From", fromAddr);
    req.GetHeader("X-To", toAddr);
    req.GetHeader("X-To2", toAddr2);
    req.GetHeader("X-To3", toAddr3);
    req.GetHeader("X-Subject", subject);
    req.GetHeader("X-ReplyTo", replyTo);
    std::string_view body = req.Body();

    auto tx = MailEndpointOf(epName)->Begin();

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

    for(std::string_view rcpt : {toAddr, toAddr2, toAddr3}) {
        if(rcpt.empty())
            continue;

        auto [s2, r2] = co_await tx.RcptTo(rcpt);
        if(s2 != WFX::EpOk) {
            j.Write("ep", EpJ(s2));
            j.Write("stage", std::string_view{"rcpt"});
            co_return;
        }
    }

    auto [s3, r3] = co_await tx.DataStart();
    if(s3 != WFX::EpOk || !r3->Continue()) {
        j.Write("ep", EpJ(s3));
        j.Write("stage", std::string_view{"data_start"});
        co_return;
    }

    auto [s4, r4] = co_await tx.DataBody(fromAddr, "WFX Interop", toAddr, "Recipient", subject, body, replyTo);
    j.Write("ep", EpJ(s4));
    j.Write("stage", std::string_view{"done"});
    if(s4 == WFX::EpOk) {
        j.Write("code", r4->code);
        j.Write("success", r4->Success());
    }
    co_return;
})

// SMTP: the same shape as /smtp/send, but through SmtpEndpoint::SendMail's own single-call
// coroutine wrapper, a genuinely different code path (include/wfx/endpoint/smtp.hpp).
//
//   X-Endpoint  "plain" | "login"
//   X-To / X-Subject   optional
//   body                the message body
WFX_POST("/smtp/send-mail", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view epName = "plain";
    std::string_view toAddr = "recipient@wfx-interop.test", subject = "interop-sendmail";
    req.GetHeader("X-Endpoint", epName);
    req.GetHeader("X-To", toAddr);
    req.GetHeader("X-Subject", subject);
    std::string_view body = req.Body();

    WFX::SmtpSendOutcome outcome;
    co_await MailEndpointOf(epName)->SendMail("sender@wfx-interop.test", "WFX Interop", toAddr, "Recipient", subject,
                                              body, outcome);

    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("ep", EpJ(outcome.status));
    j.Write("success", outcome.Success());
    if(outcome.response)
        j.Write("code", outcome.response->code);
    co_return;
})

// SMTP: RSET mid-transaction, then a fresh transaction on the *same* connection, proving RSET
// really aborts the first one rather than leaving stale envelope state behind.
//
//   X-Endpoint  "plain" | "login"
WFX_POST("/smtp/reset", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view epName = "plain";
    req.GetHeader("X-Endpoint", epName);

    auto tx = MailEndpointOf(epName)->Begin();

    res.Status(200);
    auto j = WFX::ImJson(res);

    if(!tx.IsValid()) {
        j.Write("ep", EpJ(WFX::EpPoolExhausted));
        j.Write("stage", std::string_view{"reserve"});
        co_return;
    }

    auto [s1, r1] = co_await tx.MailFrom("abandoned@wfx-interop.test");
    if(s1 != WFX::EpOk) {
        j.Write("ep", EpJ(s1));
        j.Write("stage", std::string_view{"mail1"});
        co_return;
    }

    auto [sr, rr] = co_await tx.Reset();
    j.Write("reset_ep", EpJ(sr));
    if(sr != WFX::EpOk) {
        j.Write("ep", EpJ(sr));
        j.Write("stage", std::string_view{"reset"});
        co_return;
    }

    auto [s2, r2] = co_await tx.MailFrom("sender@wfx-interop.test");
    if(s2 != WFX::EpOk) {
        j.Write("ep", EpJ(s2));
        j.Write("stage", std::string_view{"mail2"});
        co_return;
    }

    auto [s3, r3] = co_await tx.RcptTo("recipient@wfx-interop.test");
    if(s3 != WFX::EpOk) {
        j.Write("ep", EpJ(s3));
        j.Write("stage", std::string_view{"rcpt"});
        co_return;
    }

    auto [s4, r4] = co_await tx.DataStart();
    if(s4 != WFX::EpOk || !r4->Continue()) {
        j.Write("ep", EpJ(s4));
        j.Write("stage", std::string_view{"data_start"});
        co_return;
    }

    auto [s5, r5] = co_await tx.DataBody("sender@wfx-interop.test", "WFX Interop", "recipient@wfx-interop.test",
                                         "Recipient", "interop-reset", "after RSET");
    j.Write("ep", EpJ(s5));
    j.Write("stage", std::string_view{"done"});
    if(s5 == WFX::EpOk) {
        j.Write("code", r5->code);
        j.Write("success", r5->Success());
    }
    co_return;
})

// HTTP: one HttpEndpoint call against the real HTTPS upstream (interop_audit/upstream/).
//
//   X-Method  GET | POST | PUT | PATCH | DELETE   (default GET)
//   X-Path    upstream request target              (default "/get")
//   X-Body    request body for POST/PUT/PATCH       (optional)
//   X-Auth    "1" adds a Basic Authorization header for the /basic-auth route
//   X-Want    upstream response header to echo back into "hdr"
WFX_GET("/http/call", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view method = "GET", path = "/get", body{}, want{}, auth{};
    req.GetHeader("X-Method", method);
    req.GetHeader("X-Path", path);
    req.GetHeader("X-Body", body);
    req.GetHeader("X-Want", want);
    req.GetHeader("X-Auth", auth);

    WFX::HttpEndpointRequestHeaders hdrs;
    if(auth == "1")
        hdrs.Add("Authorization", "Basic aW50ZXJvcDp1cHN0cmVhbS1wYXNz"); // interop:upstream-pass

    WFX::EndpointStatus status;
    WFX::EndpointOutput<WFX::HttpEndpointResponse> out;

    if(method == "POST")
        std::tie(status, out) = co_await Web.Post(path, body, std::move(hdrs));
    else if(method == "PUT")
        std::tie(status, out) = co_await Web.Put(path, body, std::move(hdrs));
    else if(method == "PATCH")
        std::tie(status, out) = co_await Web.Patch(path, body, std::move(hdrs));
    else if(method == "DELETE")
        std::tie(status, out) = co_await Web.Delete(path, std::move(hdrs));
    else
        std::tie(status, out) = co_await Web.Get(path, std::move(hdrs));

    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("ep", EpJ(status));
    if(status != WFX::EpOk)
        co_return;

    j.Write("status", static_cast<std::uint64_t>(out->status));
    j.Write("bodylen", static_cast<std::uint64_t>(out->body.size()));
    j.Write("body", std::string_view{out->body.data(), out->body.size()});

    if(!want.empty()) {
        std::string_view hv;
        j.Write("hdr", out->GetHeader(want, hv) ? hv : std::string_view{});
    }
    co_return;
})
