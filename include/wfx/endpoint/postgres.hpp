// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_ENDPOINT_POSTGRES_HPP
#define WFX_INC_WFX_ENDPOINT_POSTGRES_HPP

// -----------------------------------------------------------------------
// wfx/endpoint/postgres.hpp
// Outbound PostgreSQL client (PostgresEndpoint).
//
// Every query goes through the extended query protocol, so parameters are
// sent as typed values and never pasted into SQL. Results come back in
// binary where a codec exists.
//
// Provides:
//   WFX::PostgresEndpoint       the endpoint, declared at namespace scope
//   WFX::PostgresConfig         pool, identity and session settings
//   WFX::PgResult / PgRow       result set and row cursors
//   WFX::PgError                SQLSTATE and the rest of the error fields
//   WFX::PgNull                 binds SQL NULL
//   WFX::PgBytes / PgUuid       bytea and uuid parameters
//   WFX::PgTimestamp / PgDate / PgTime / PgInterval / PgNumeric
//
// -----------------------------------------------------------------------
// Usage
// -----------------------------------------------------------------------
//
//   inline const auto Db = WFX::PostgresEndpoint{"db.internal:5432", WFX::PostgresConfig{
//       .user     = "app",
//       .password = WFX::GetEnvString("PGPASSWORD"),
//       .database = "app",
//   }};
//
//   WFX_GET("/users", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
//       auto [status, rows] = co_await Db.Query(
//           "SELECT id, email FROM users WHERE active = $1 LIMIT $2", true, 50);
//
//       // Two failures to tell apart: the connection never delivered a
//       // result, or the server answered with an error
//       if(status != WFX::EpOk) {
//           res.Status(502).SendText("database unavailable");
//           co_return;
//       }
//       if(rows->Failed()) {
//           res.Status(500).SendText(rows->Error().Message());
//           co_return;
//       }
//
//       for(std::uint32_t i = 0; i < rows->RowCount(); ++i) {
//           auto row = rows->At(i);
//           const auto id    = row.Get<std::int64_t>("id");
//           const auto email = row.Get<std::string_view>("email");
//           ...
//       }
//
//       co_return;
//   });
//
// A result too large to hold at once is read a chunk at a time instead. The
// connection is held for the whole stream, inside a transaction, because that
// is the only thing that keeps a portal alive between rounds.
//
//   WFX_GET("/export", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
//       auto stream = Db.Stream(500, "SELECT id, email FROM users");
//
//       while(true) {
//           auto chunk = co_await stream.Next();
//           if(chunk.status != WFX::EpOk || chunk.done)
//               break;
//
//           // Borrowed until the next Next(), so anything kept is copied
//           for(std::uint32_t i = 0; i < chunk.data->RowCount(); ++i)
//               ...
//       }
//
//       co_return;
//   });
//
// -----------------------------------------------------------------------
// Notes
// -----------------------------------------------------------------------
//
//   Row data borrows the response buffer, so a string_view read out of a row
//   dies with the EndpointOutput that produced it. Copy anything that has to
//   outlive the handler.
//
//   Reading a NULL yields a value-initialized value. Call IsNull() where the
//   difference matters.
//
//   Get<T> trusts the type you ask for rather than checking it, because
//   enums, domains and extension types get an OID per database that no fixed
//   table can recognize, and rejecting those would make them unreadable.
//   PgResult::Matches<T>() reports what can actually be confirmed.
//
//   Authentication is SCRAM-SHA-256 or cleartext. The engine's crypto backend
//   exposes SHA-2 only, so a server asking for md5 is refused rather than
//   answered.
// -----------------------------------------------------------------------

#include "wfx/endpoint/postgres/wire.hpp"

namespace WFX {

// vvv Result types vvv
using PgResult = Postgres::Detail::PgResult;
using PgRow = Postgres::Detail::PgRow;
using PgError = Postgres::Detail::PgError;
using PgColumn = Postgres::Detail::PgColumn;
using PgArrayView = Postgres::Detail::PgArrayView;

// vvv Parameter and value types vvv
using PgNull = Postgres::Detail::PgNull;
using PgBytes = Postgres::Detail::PgBytes;
using PgUuid = Postgres::Detail::PgUuid;
using PgTimestamp = Postgres::Detail::PgTimestamp;
using PgDate = Postgres::Detail::PgDate;
using PgTime = Postgres::Detail::PgTime;
using PgInterval = Postgres::Detail::PgInterval;
using PgNumeric = Postgres::Detail::PgNumeric;

// vvv Policy types vvv
using PgEncryption = Postgres::Detail::PgEncryption;
using PgAuthPolicy = Postgres::Detail::PgAuthPolicy;
using PgIsolation = Postgres::Detail::PgIsolation;

// -----------------------------------------------------------------------
// Connection pool, identity and session settings.
//
// Only settings this client can honour are here. Certificate trust, TCP
// keepalives and connection max-age belong to the engine or are not reachable
// through the endpoint API, so they are configured there rather than being
// accepted and ignored.
// -----------------------------------------------------------------------
struct PostgresConfig {
    // vvv Pool and lifecycle vvv
    // Slots are allocated exactly, so connLimit is the real ceiling rather
    // than being rounded up. It is also per worker process: the server sees
    // connLimit * worker_processes at most, which has to stay under its
    // max_connections.
    std::uint32_t connLimit = 8;
    std::uint32_t auxConnLimit = 2;           // side connections for query cancellation
    std::uint16_t connectTimeoutSeconds = 10; // TCP + TLS + startup + auth budget
    std::uint16_t requestTimeoutSeconds = 30; // one query, measured on this side

    // Kept under the idle cutoff of common proxies and NAT gateways, which is
    // around 350s, so a connection is closed here rather than being silently
    // dropped in the middle and failing on next use
    std::uint32_t idleTimeoutSeconds = 300;

    std::uint16_t maxReconnectAttempts = 3;
    std::uint16_t reconnectBackoffBaseSeconds = 1;
    std::uint16_t reconnectBackoffMaxSeconds = 30;
    std::uint32_t prewarm = 0;
    std::uint32_t dnsRefreshSeconds = 0; // 0 respects the record's own TTL

    // vvv Identity, sent in the startup message vvv
    std::string_view user{};
    std::string_view password{}; // e.g. WFX::GetEnvString("PGPASSWORD") (never logged)
    std::string_view database{};
    std::string_view applicationName{}; // shows up in pg_stat_activity

    // vvv Security vvv
    PgEncryption encryption = PgEncryption::REQUIRED;
    PgAuthPolicy authPolicy = PgAuthPolicy::NO_PLAINTEXT;

    // vvv Session settings, sent as startup parameters vvv
    std::string_view searchPath{};
    std::string_view timeZone{};
    std::uint32_t statementTimeoutMs = 0; // server side statement_timeout, 0 leaves it unset
    std::uint32_t lockTimeoutMs = 0;
    std::uint32_t idleInTransactionTimeoutMs = 0;

    // vvv Wire vvv
    bool preferBinary = true;
    std::uint32_t maxMessageBytes = 16u * 1024u * 1024u; // rejects an absurd length field

    // vvv Statement cache vvv
    std::uint32_t statementCacheSize = 64;   // named statements per connection, 0 turns it off
    std::uint16_t statementCacheMinUses = 2; // executions before SQL earns a name
};

namespace Postgres::Detail {

// -----------------------------------------------------------------------
// State and output factories
// -----------------------------------------------------------------------
inline void* CreateSlotState(void* userCtx) noexcept
{
    auto* s = New<SlotState>();
    if(!s)
        return nullptr;

    const auto* opts = static_cast<const PgOptions*>(userCtx);
    s->options = opts;
    s->stmtCache.Init(opts->cacheControl ? opts->statementCacheSize : 0, opts->statementCacheMinUses);

    return s;
}

inline void DestroySlotState(void* state) noexcept
{
    Delete(static_cast<SlotState*>(state));
}

inline void* CreateOutput(void*) noexcept
{
    return New<PgResult>();
}

inline void DestroyOutput(void* out) noexcept
{
    Delete(static_cast<PgResult*>(out));
}

// No parse state: everything that survives between parse calls of one request
// lives in the result, which the engine keeps for that request
inline EndpointDesc BuildDesc(PgOptions* opts) noexcept
{
    EndpointDesc d{};
    d.serialize = &Serialize;
    d.parse = &Parse;
    d.createSlotState = &CreateSlotState;
    d.destroySlotState = &DestroySlotState;
    d.createOutput = &CreateOutput;
    d.destroyOutput = &DestroyOutput;
    d.userCtx = opts;
    return d;
}

// Statement text handed to Begin. These are static so the view inside the
// request stays valid for as long as the request is in flight.
inline constexpr std::string_view SQL_BEGIN_READ_COMMITTED = "BEGIN ISOLATION LEVEL READ COMMITTED";
inline constexpr std::string_view SQL_BEGIN_REPEATABLE_READ = "BEGIN ISOLATION LEVEL REPEATABLE READ";
inline constexpr std::string_view SQL_BEGIN_SERIALIZABLE = "BEGIN ISOLATION LEVEL SERIALIZABLE";
inline constexpr std::string_view SQL_COMMIT = "COMMIT";
inline constexpr std::string_view SQL_ROLLBACK = "ROLLBACK";

inline std::string_view BeginStatement(PgIsolation iso) noexcept
{
    switch(iso) {
        case PgIsolation::REPEATABLE_READ:
            return SQL_BEGIN_REPEATABLE_READ;
        case PgIsolation::SERIALIZABLE:
            return SQL_BEGIN_SERIALIZABLE;
        default:
            return SQL_BEGIN_READ_COMMITTED;
    }
}

// Shared by the pooled and the pinned query paths
template <typename... Ts> inline PgRequest MakeRequest(std::string_view sql, const Ts&... params)
{
    PgRequest req;
    req.sql = sql;
    req.paramCount = sizeof...(Ts);

    if constexpr(sizeof...(Ts) > 0) {
        // One array per instantiation, so its address outlives the request
        static constexpr std::uint32_t K_OIDS[] = {ParamOid<Ts>()...};
        req.paramOids = K_OIDS;
    }

    req.encoded = EncodeParams(req, params...);
    return req;
}

// Same request, marked as running on a reserved connection so Serialize leaves
// any open transaction on it alone
template <typename... Ts> inline PgRequest MakePinnedRequest(std::string_view sql, const Ts&... params)
{
    PgRequest req = MakeRequest(sql, params...);
    req.pinned = true;
    return req;
}

// chunkRows is what makes it a stream, so zero runs as an ordinary query that
// completes on the first Next()
template <typename... Ts>
inline PgRequest MakeStreamRequest(std::uint32_t chunkRows, bool pinned, std::string_view sql, const Ts&... params)
{
    PgRequest req = MakeRequest(sql, params...);
    req.streamRows = chunkRows;
    req.pinned = pinned;
    return req;
}

// sql carries the savepoint name here. A name that is not a plain identifier
// would be composed straight into the statement, so it is rejected instead.
inline PgRequest MakeSavepointRequest(PgStatementKind kind, std::string_view name)
{
    PgRequest req;
    req.sql = name;
    req.pinned = true;
    req.kind = kind;
    req.encoded = IsValidIdentifier(name);
    return req;
}

inline PgOptions BuildOptions(const PostgresConfig& cfg) noexcept
{
    PgOptions o;
    o.user = cfg.user;
    o.password = cfg.password;
    o.database = cfg.database;
    o.applicationName = cfg.applicationName;
    o.searchPath = cfg.searchPath;
    o.timeZone = cfg.timeZone;
    o.statementTimeoutMs = cfg.statementTimeoutMs;
    o.lockTimeoutMs = cfg.lockTimeoutMs;
    o.idleInTransactionTimeoutMs = cfg.idleInTransactionTimeoutMs;
    o.maxMessageBytes = cfg.maxMessageBytes;
    o.statementCacheSize = cfg.statementCacheSize;
    o.statementCacheMinUses = cfg.statementCacheMinUses;
    o.encryption = cfg.encryption;
    o.authPolicy = cfg.authPolicy;
    o.preferBinary = cfg.preferBinary;
    return o;
}

} // namespace Postgres::Detail

// -----------------------------------------------------------------------
// PgSession
//
// Pins one connection so every statement sent through it runs on that
// connection, and releases it when the session goes out of scope.
//
// A transaction is one use of the pin, not what the pin is for: prepared
// statements, SET SESSION, advisory locks and LISTEN are all session state
// that also needs one connection held for as long as they matter. Begin is
// optional. Without it, every statement still runs on the same connection,
// just outside a transaction.
//
// Always finish a transaction with Commit or Rollback. Going out of scope
// without one leaves the connection idle in a transaction, holding its row
// locks and blocking VACUUM across the database. idleInTransactionTimeoutMs
// bounds that server side if you want a backstop.
//
// The pool stays correct regardless: the next request on that slot sends a
// rollback before its own statement, so nobody sees the unfinished work.
//
//   auto tx = Db.Session();
//   if(!tx.IsValid()) { ... pool exhausted ... }
//
//   if((co_await tx.Begin()).first != WFX::EpOk) { ... }
//   co_await tx.Query("INSERT INTO certificate (id, name) VALUES ($1, $2)", id, name);
//   co_await tx.Query("INSERT INTO audit (action) VALUES ($1)", "issue");
//   auto [status, out] = co_await tx.Commit();
// -----------------------------------------------------------------------
class PgSession {
public: // Constructors
    explicit PgSession(Async::ReservedSlot<Postgres::Detail::PgRequest, PgResult>&& slot) noexcept
        : slot_(std::move(slot))
    {}

public: // State
    // False when the pool had no connection to pin
    bool IsValid() const noexcept
    {
        return slot_.IsValid();
    }

public: // Statements
    auto Begin(PgIsolation isolation = PgIsolation::READ_COMMITTED) const
    {
        return slot_.SendPayload(Postgres::Detail::MakePinnedRequest(Postgres::Detail::BeginStatement(isolation)));
    }

    template <typename... Ts> auto Query(std::string_view sql, const Ts&... params) const
    {
        return slot_.SendPayload(Postgres::Detail::MakePinnedRequest(sql, params...));
    }

    // Same chunked read as PostgresEndpoint::Stream, on this connection. Runs
    // inside the session's transaction when it has one, else opens its own.
    template <typename... Ts> auto Stream(std::uint32_t chunkRows, std::string_view sql, const Ts&... params) const
    {
        return slot_.Stream(Postgres::Detail::MakeStreamRequest(chunkRows, true, sql, params...));
    }

    auto Commit() const
    {
        return slot_.SendPayload(Postgres::Detail::MakePinnedRequest(Postgres::Detail::SQL_COMMIT));
    }

    auto Rollback() const
    {
        return slot_.SendPayload(Postgres::Detail::MakePinnedRequest(Postgres::Detail::SQL_ROLLBACK));
    }

public: // Savepoints
    // Marks a point to come back to, so a later failure can undo part of the
    // transaction instead of all of it. The name must be a plain identifier:
    // letters, digits and underscores, not starting with a digit.
    auto Savepoint(std::string_view name) const
    {
        return slot_.SendPayload(
            Postgres::Detail::MakeSavepointRequest(Postgres::Detail::PgStatementKind::SAVEPOINT, name));
    }

    // Undoes everything after the savepoint. The transaction stays open, and
    // the savepoint itself survives to be rolled back to again.
    auto RollbackTo(std::string_view name) const
    {
        return slot_.SendPayload(
            Postgres::Detail::MakeSavepointRequest(Postgres::Detail::PgStatementKind::ROLLBACK_TO, name));
    }

    // Drops the savepoint, keeping the work done since it was taken
    auto ReleaseSavepoint(std::string_view name) const
    {
        return slot_.SendPayload(
            Postgres::Detail::MakeSavepointRequest(Postgres::Detail::PgStatementKind::RELEASE_SAVEPOINT, name));
    }

private:
    Async::ReservedSlot<Postgres::Detail::PgRequest, PgResult> slot_;
};

// -----------------------------------------------------------------------
// PostgresEndpoint
//
// Declare at namespace scope, before Run(), like any other endpoint.
// -----------------------------------------------------------------------
class PostgresEndpoint {
public: // Constructors
    explicit PostgresEndpoint(const char* hostPort, PostgresConfig config = {})
        : options_(Postgres::Detail::BuildOptions(config)),
          ep_(hostPort, Postgres::Detail::BuildDesc(&options_),
              Shared::EndpointConfig{
                  .connLimit = config.connLimit,
                  .auxConnLimit = config.auxConnLimit,
                  .dnsRefreshSeconds = config.dnsRefreshSeconds,
                  .connectTimeoutSeconds = config.connectTimeoutSeconds,
                  .requestTimeoutSeconds = config.requestTimeoutSeconds,
                  .idleTimeoutSeconds = config.idleTimeoutSeconds,
                  .maxReconnectAttempts = config.maxReconnectAttempts,
                  .reconnectBackoffBase = config.reconnectBackoffBaseSeconds,
                  .reconnectBackoffMax = config.reconnectBackoffMaxSeconds,

                  // Postgres negotiates encryption in-band with SSLRequest, and
                  // the engine refuses to upgrade a slot it already wrapped, so
                  // the endpoint stays plaintext and onConnect performs the upgrade
                  .tlsConfig = EpTlsInsecure,

                  // A database pool is small, and rounding it up to 64 would
                  // both allocate and permit far more connections than asked for
                  .exactSlots = true,
                  .prewarm = config.prewarm,
                  .maxConcurrentStreams = 0,
                  .alpnProtocols = {},
              })
    {
        // Set here rather than in BuildOptions, which has no address to give
        options_.cacheControl = &cacheControl_;
    }

    // Never copied or moved: the engine holds the address of options_
    PostgresEndpoint(const PostgresEndpoint&) = delete;
    PostgresEndpoint& operator=(const PostgresEndpoint&) = delete;

public: // Queries
    // co_await this. Parameters are numbered $1, $2, ... in the SQL and are
    // encoded by C++ type, so nothing is interpolated into the statement.
    template <typename... Ts> auto Query(std::string_view sql, const Ts&... params) const
    {
        return ep_.SendPayload(Postgres::Detail::MakeRequest(sql, params...));
    }

public: // Streaming
    // Reads the result chunkRows at a time, so peak memory tracks one chunk
    // rather than the total. Holds the connection until the stream ends.
    template <typename... Ts> auto Stream(std::uint32_t chunkRows, std::string_view sql, const Ts&... params) const
    {
        return ep_.Stream(Postgres::Detail::MakeStreamRequest(chunkRows, false, sql, params...));
    }

public: // Sessions
    // Takes a connection out of the pool for the caller. Check IsValid() before
    // using it, since the pool can be empty.
    PgSession Session() const
    {
        return PgSession{ep_.Reserve()};
    }

private:
    Postgres::Detail::PgCacheControl cacheControl_;
    Postgres::Detail::PgOptions options_;
    Async::Resolve<Postgres::Detail::PgRequest, PgResult, &Postgres::Detail::PgOnConnect, &Postgres::Detail::PgOnAbort>
        ep_;
};

} // namespace WFX

#endif // WFX_INC_WFX_ENDPOINT_POSTGRES_HPP
