# Postgres Endpoint

`WFX::PostgresEndpoint` is a ready-to-use PostgreSQL client, built on top of
the raw [`WFX::Endpoint<>` primitive](overview.md). One instance represents
one database and its connection pool.

!!! important
    This page only covers what's specific to Postgres. The connection pool, DNS
    resolution and refresh, reconnect/backoff behavior, and prewarm all work
    exactly as described on the [Endpoint overview](overview.md),
    `PostgresEndpoint` just wires them up for you with a Postgres-shaped
    handshake and wire protocol.

    To use it, include:
    ```cpp
    #include <wfx/endpoint/postgres.hpp>
    ```

Every query goes through the extended query protocol (`Parse` / `Bind` /
`Describe` / `Execute` / `Sync`). Parameters travel through `Bind` as typed
binary values, not as text interpolated into the SQL string. Results come
back in binary wherever a codec exists for the column's type. Authentication
is SCRAM-SHA-256 (the default since Postgres 10) or cleartext; MD5 isn't
supported, since the engine's crypto backend only implements SHA-2.

---

## Declaring & connecting

```cpp
inline const auto Db = WFX::PostgresEndpoint{"db.internal:5432", WFX::PostgresConfig{
    .user     = "app",
    .password = WFX::GetEnvString("PGPASSWORD"),
    .database = "app",
}};
```

Declaration rules (namespace scope, `"host:port"` format, no scheme prefix,
eager DNS resolution at startup) are the same as the raw primitive, see
[Declaring an endpoint](overview.md#declaring-an-endpoint) for the full
details.

`user` is the only field Postgres actually requires; leaving it empty is a
fatal error at connect time rather than something the server gets to reject.
Everything else defaults to a working, if generic, configuration, see
[Tuning & limits](#tuning-limits) for the full list.

### Encryption and authentication policy

```cpp
enum class PgEncryption : std::uint8_t {
    NONE,     // never ask
    OPTIONAL, // ask, continue in plaintext if refused
    REQUIRED, // ask, fail if refused
};

enum class PgAuthPolicy : std::uint8_t {
    ANY,          // whatever the server asks for, cleartext included
    NO_PLAINTEXT, // refuse cleartext, allow SCRAM
    SCRAM_ONLY,   // refuse everything except SCRAM
};
```

`encryption` defaults to `REQUIRED` and `authPolicy` to `NO_PLAINTEXT`, so an
endpoint declared with nothing but `user`/`password`/`database` already
requires an encrypted connection and refuses a cleartext password. Setting
`authPolicy = PgAuthPolicy::ANY` is what lets `PgEncryption::NONE` and a
cleartext password coexist; without it, a server asking for a cleartext
password on an unencrypted connection is refused.

!!! important
    There is no `tlsConfig` on `PostgresConfig`. Postgres negotiates encryption
    **in-band**: the client sends `SSLRequest` over the still-plaintext
    connection and only wraps it in TLS if the server agrees, the same
    pattern [In-band TLS upgrades](overview.md#in-band-tls-upgrades)
    describes for `SmtpEndpoint`'s `STARTTLS`. `encryption` is
    `PostgresEndpoint`'s own knob for that negotiation; the underlying
    primitive stays configured `EpTlsInsecure` internally so the in-band
    upgrade is the one deciding.

---

## Queries

```cpp
WFX_GET("/users", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    auto [status, rows] = co_await Db.Query(
        "SELECT id, email FROM users WHERE active = $1 LIMIT $2", true, 50);

    if(status != WFX::EpOk) {
        res.Status(502).SendText("database unavailable");
        co_return;
    }
    if(rows->Failed()) {
        res.Status(500).SendText(rows->Error().Message());
        co_return;
    }

    for(std::uint32_t i = 0; i < rows->RowCount(); ++i) {
        auto row = rows->At(i);
        const auto id    = row.Get<std::int64_t>("id");
        const auto email = row.Get<std::string_view>("email");
        // ...
    }

    co_return;
});
```

`Query` takes a SQL string with `$1`, `$2`, ... placeholders and any number of
typed C++ arguments, and returns the same `{status, out}` pair every
`SendPayload` does, see [Sending a request](overview.md#sending-a-request).
`out` is a `WFX::EndpointOutput<PgResult>`, following the same lifetime rule
described there.

There are two independent things worth checking, and it's easy to check only
one:

- **`status`** tells you whether a result exists at all. Anything other than
  `WFX::EpOk` means the connection didn't deliver one, pool exhausted,
  connect refused, TLS failed, the request timed out, and `rows` shouldn't be
  touched.
- **`rows->Failed()`** tells you whether the server answered with an
  `ErrorResponse` instead of results, a syntax error, a constraint violation,
  a permission failure. `rows->Error()` gives you a [`PgError`](#pgerror)
  with the SQLSTATE and message.

A query that fails with `rows->Failed()` still leaves the connection healthy
and pooled, the same way a `4xx`/`5xx` from an HTTP upstream doesn't mean the
connection itself is bad.

### `PgResult`

```cpp
std::uint32_t RowCount() const noexcept;
std::uint16_t ColumnCount() const noexcept;
bool Empty() const noexcept;               // RowCount() == 0
std::uint64_t AffectedRows() const noexcept; // rows affected, for INSERT/UPDATE/DELETE

const PgError& Error() const noexcept;
bool Failed() const noexcept;              // Error().IsSet()

PgRow At(std::uint32_t row) const noexcept;
PgRow operator[](std::uint32_t row) const noexcept; // same as At()

const PgColumn& Column(std::uint16_t col) const noexcept;
std::string_view ColumnName(std::uint16_t col) const noexcept;
std::int32_t IndexOf(std::string_view name) const noexcept; // -1 if not found

template <typename T> bool Matches(std::uint16_t col) const noexcept;
template <typename T> bool Matches(std::string_view name) const noexcept;
```

`AffectedRows()` is what `CommandComplete`'s tag reports (`"INSERT 0 5"`,
`"UPDATE 3"`), which for `INSERT`/`UPDATE`/`DELETE` is rows *affected*, not
rows *returned*, a statement with no `RETURNING` clause has `RowCount() == 0`
and `AffectedRows()` telling you what actually happened.

`Matches<T>()` reports whether `Get<T>()` on that column is something you can
confirm from the column's declared type. `Get<T>` itself doesn't consult it,
see [`PgRow::Get`](#pgrowget) below for why.

### `PgRow`

A cheap-to-copy cursor over one row, valid only while the `PgResult` it came
from is alive.

```cpp
std::uint16_t ColumnCount() const noexcept;
std::int32_t IndexOf(std::string_view name) const noexcept;

bool IsNull(std::uint16_t col) const noexcept;
bool IsNull(std::string_view name) const noexcept;

std::string_view Raw(std::uint16_t col) const noexcept; // undecoded wire bytes

template <typename T> T Get(std::uint16_t col) const noexcept;
template <typename T> T Get(std::string_view name) const noexcept;
```

#### `PgRow::Get`

```cpp
const auto id    = row.Get<std::int64_t>(0);
const auto email = row.Get<std::string_view>("email");
```

`Get<T>` decodes column `col` (by index or by name) as `T`, using whichever
[type mapping](#type-mapping) applies to `T`. It trusts the type you ask for
rather than checking it: enums, domains, and extension types are assigned an
OID per database, so no fixed table could recognize them, and rejecting a
column just because its exact OID isn't in a static list would make every one
of those types unreadable. Call `Matches<T>()` first if you need to confirm
the column's declared type before trusting the decode.

!!! warning "NULL and lifetime"
    Reading a `NULL` column yields a value-initialized `T` (`0`, an empty
    `std::string_view`, and so on). Call `IsNull()` first wherever a `NULL`
    and a real zero-ish value need to be told apart.

    Row data borrows the response buffer: a `std::string_view` read out of a
    row dies with the `EndpointOutput` that produced it, the same rule
    [`EndpointOutput`](overview.md#sending-a-request) always follows. Copy
    anything you need to outlive the handler.

### `PgError`

```cpp
bool IsSet() const noexcept;               // same as PgResult::Failed()
std::string_view SqlState() const noexcept;
std::string_view Message() const noexcept;
std::string_view Detail() const noexcept;
std::string_view Hint() const noexcept;
std::string_view Severity() const noexcept;
std::string_view ConstraintName() const noexcept;
std::string_view TableName() const noexcept;
std::string_view ColumnName() const noexcept;
std::string_view SchemaName() const noexcept;
std::int32_t Position() const noexcept;    // 1-based index into the failing query, 0 if unset
```

Every field Postgres's `ErrorResponse` can carry, exposed directly.
`SqlState()` is the one worth branching on programmatically (`"23505"` for a
unique-violation, `"40001"` for a serialization failure worth retrying); the
rest are for building a useful error message or log line.

---

## Parameters

```cpp
co_await Db.Query("INSERT INTO users (name, age, bio) VALUES ($1, $2, $3)",
                  "Alice", 30, WFX::PgNull{});
```

Every argument after the SQL string becomes `$1`, `$2`, ... in order, encoded
by its C++ type and sent through `Bind` as a length-prefixed binary value.
The SQL string itself is the literal you wrote; a parameter's value has no
way to change what gets parsed as SQL, since it never travels anywhere near
the statement text. That's what makes this client's queries structurally
resistant to the classic string-concatenation style of SQL injection.

| C++ argument | Binds as |
|---------------|----------|
| `WFX::PgNull{}` | SQL `NULL` |
| a string literal, `const char*`, or anything convertible to `std::string_view` | `text` |
| any integer type | `int2`/`int4`/`int8`, sized from the C++ type |
| `float` / `double` | `float4` / `float8` |
| `bool` | `bool` |
| `WFX::PgBytes` | `bytea` |
| `WFX::PgUuid` | `uuid` |
| `WFX::PgTimestamp` | `timestamptz` |
| `WFX::PgDate` | `date` |
| `WFX::PgTime` | `time` |
| `WFX::PgInterval` | `interval` |

`WFX::PgNumeric` has no encoder: binding an arbitrary-precision value would
mean this client owning a bignum implementation just to hand it back to
Postgres, so a `numeric` parameter binds as `text` instead and the server
parses it, the same as most clients that don't carry their own bignum type.

Arrays can be [read back](#arrays) from a result but not sent as a parameter,
there's no array encoder yet.

---

## Type mapping

Binary codecs exist for every type below; anything else decodes as `text` via
`std::string_view`, and `Matches<T>()` reports `false` for a type with no
dedicated codec.

| Postgres type | OID | C++ type |
|---------------|-----|----------|
| `bool` | 16 | `bool` |
| `bytea` | 17 | `WFX::PgBytes` |
| `int8` | 20 | any integer type, width-flexible (see below) |
| `int2` | 21 | any integer type |
| `int4` | 23 | any integer type |
| `text`, `varchar`, `bpchar`, `name`, `char`, `json`, `xml` | 25, 1043, 1042, 19, 18, 114, 142 | `std::string_view` |
| `oid` | 26 | any integer type wide enough to hold it unsigned |
| `float4` | 700 | `float`, or `double` (widens losslessly) |
| `float8` | 701 | `double` |
| `date` | 1082 | `WFX::PgDate` |
| `time` | 1083 | `WFX::PgTime` |
| `timestamp` | 1114 | `WFX::PgTimestamp` (treated as UTC, since it carries no zone) |
| `timestamptz` | 1184 | `WFX::PgTimestamp` |
| `interval` | 1186 | `WFX::PgInterval` |
| `numeric` | 1700 | `WFX::PgNumeric` (decode only, see [below](#pgnumeric)) |
| `uuid` | 2950 | `WFX::PgUuid` |
| `jsonb` | 3802 | `std::string_view` (the leading version byte is stripped for you) |

Integers widen but shouldn't narrow. `Get<T>`'s codec looks at the *column's*
wire width, not `T`'s, so a `smallint` column decodes correctly into an
`int64_t` you asked for. Going the other way, reading an `int8` column into
an `int16_t`, would lose data, which is exactly why `Get<T>` trusts your
choice of `T` instead of picking one for you. Pick `T` at least as wide as
the column actually is.

### `PgNumeric`

```cpp
struct PgNumeric {
    const char* digits = nullptr; // base-10000 digit array, ndigits entries
    std::int16_t ndigits = 0;
    std::int16_t weight = 0;      // base-10000 exponent of the first digit
    std::uint16_t sign = NUMERIC_SIGN_POSITIVE;
    std::int16_t dscale = 0;      // decimal digits after the point

    bool IsNan() const noexcept;
    bool IsInf() const noexcept;
    bool IsNegative() const noexcept;
    std::int16_t DigitAt(std::int16_t i) const noexcept;
    double ToDouble() const noexcept;
};
```

`numeric` is arbitrary precision, stored on the wire as a base-10000 digit
array, so no native C++ type holds it losslessly. `PgNumeric` exposes the raw
components as a view rather than picking a lossy conversion for you.
`ToDouble()` is there for when a `double`'s precision is genuinely enough for
your use case (a chart, a rough total): it's exact only while the value fits
a `double`'s 53-bit mantissa, which is why it's a separate opt-in call rather
than what `Get<double>` would do automatically. It also correctly reports
`NaN` and `+-Infinity`, both real `numeric` values Postgres can return.

### Arrays

There is no `Get<T[]>`. Read an array column as a `WFX::PgArrayView` instead,
through the same `Get<T>` every other type uses, then step through its
elements one at a time:

```cpp
const auto arr = row.Get<WFX::PgArrayView>("tags");

const char* cursor = arr.elements;
std::string_view elem;
bool isNull = false;

while(arr.NextElement(cursor, elem, isNull)) {
    if(isNull)
        continue;
    // elem is one element's raw bytes, decode it the same way its scalar
    // codec would (LoadBe32 for int4, etc.)
}
```

```cpp
struct PgArrayView {
    const char* elements = nullptr;
    std::uint32_t bytes = 0;
    std::int32_t ndim = 0;
    std::uint32_t elementOid = 0;
    std::int32_t dimLengths[6]{};
    std::int32_t lowerBounds[6]{};

    std::int32_t Count() const noexcept; // product of dimLengths

    // Steps one element starting from `cursor` (initialize it to .elements
    // before the first call). Returns false once every element has been
    // consumed or the data is truncated.
    bool NextElement(const char*& cursor, std::string_view& out, bool& isNull) const noexcept;
};
```

A malformed header, more than 6 dimensions, or any dimension with a negative
length, decodes to a value-initialized `PgArrayView` (`elements == nullptr`),
the same "NULL yields a value-initialized `T`" contract every other codec
follows. `elements != nullptr` is how you tell a genuinely empty array
(`Count() == 0` but still well-formed) apart from a decode failure.

Treat `Count()` as informational rather than a value to size a loop or
allocation from directly; `NextElement` walking the actual bytes is what
tells you how many elements are really there.

---

## Sessions & transactions

A single `Query()` call may land on any pooled connection, which is fine for
one-off statements but breaks anything where state lives on the connection: a
multi-statement transaction, a prepared statement you want named explicitly,
`SET`, an advisory lock. `Session()` pins one connection to you for as long
as you hold it, the Postgres application of
[Pinning a connection](overview.md#pinning-a-connection):

```cpp
WFX_POST("/transfer", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    auto tx = Db.Session();
    if(!tx.IsValid()) {
        res.Status(503).SendText("no connection available");
        co_return;
    }

    if((co_await tx.Begin()).first != WFX::EpOk) {
        res.Status(502).SendText("database unavailable");
        co_return;
    }

    co_await tx.Query("UPDATE accounts SET balance = balance - $1 WHERE id = $2", 100, 1);
    co_await tx.Query("UPDATE accounts SET balance = balance + $1 WHERE id = $2", 100, 2);

    auto [status, out] = co_await tx.Commit();
    if(status != WFX::EpOk || out->Failed()) {
        res.Status(502).SendText("commit failed");
        co_return;
    }

    res.SendText("ok");
    co_return;
});
```

`Query` on a `PgSession` has the same signature and return shape as
`PostgresEndpoint::Query`, just running on the one pinned connection instead
of whichever one the pool would have picked.

A transaction is one use of a session, not what a session is for: prepared
statements, `SET SESSION`, advisory locks, and anything else that's
connection-scoped state also need one connection held for as long as they
matter. `Begin()` is optional; without it, every statement still runs on the
same connection, just outside a transaction.

`Begin(WFX::PgIsolation isolation = WFX::PgIsolation::READ_COMMITTED)` takes
an isolation level: `READ_COMMITTED` (the default, and Postgres's own
default), `REPEATABLE_READ`, or `SERIALIZABLE`. Pass it as the one argument,
`co_await tx.Begin(WFX::PgIsolation::SERIALIZABLE)`.

!!! warning
    Always finish a transaction with `Commit()` or `Rollback()`. Letting `tx`
    go out of scope without one leaves the connection idle in a transaction,
    holding its row locks and blocking `VACUUM` across the database.
    `idleInTransactionTimeoutMs` (see [Tuning & limits](#tuning-limits)) is a
    server-side backstop if you want one, not a substitute for finishing your
    own transactions.

The pool stays correct either way: the next request to land on that
connection sends a `ROLLBACK` ahead of its own statement before doing
anything else, so an abandoned transaction doesn't leak into someone else's
query. This happens automatically.

### Savepoints

```cpp
auto tx = Db.Session();
co_await tx.Begin();
co_await tx.Query("INSERT INTO orders (id) VALUES ($1)", 1);

if((co_await tx.Savepoint("before_risky")).first == WFX::EpOk) {
    auto [status, out] = co_await tx.Query("INSERT INTO orders (id) VALUES ($1)", 1); // duplicate
    if(out->Failed())
        co_await tx.RollbackTo("before_risky"); // undo just this insert
    else
        co_await tx.ReleaseSavepoint("before_risky"); // keep it, drop the marker
}

co_await tx.Commit();
```

A savepoint name can't be a bind parameter, Postgres syntax has no
placeholder for an identifier, so this is the one place `PgSession` composes
a statement from your input instead of sending it as data. `name` is checked
against a plain-identifier grammar (letters, digits, underscores, not
starting with a digit, at most 63 characters, `wire.hpp`'s
`MAX_IDENTIFIER_LEN`) before it's composed into anything; a name that fails
that check doesn't reach the wire, the call returns `WFX::EpSerializeError`
immediately.

- **`Savepoint(name)`** marks a point to come back to.
- **`RollbackTo(name)`** undoes everything since the savepoint. The
  transaction stays open, and the savepoint itself survives to be rolled
  back to again.
- **`ReleaseSavepoint(name)`** drops the savepoint, keeping the work done
  since it was taken.

---

## Streaming large results

`Query()` (and `PgSession::Query`) materializes the whole result before
handing it back, which is wrong for a result set too large to hold in memory
at once. `Stream()` is the Postgres application of
[Streaming large responses](overview.md#streaming-large-responses): it reads
a chunk at a time, reusing one output object so peak memory tracks one chunk
rather than the total.

```cpp
WFX_GET("/export", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    auto stream = Db.Stream(500, "SELECT id, email FROM users");

    while(true) {
        auto chunk = co_await stream.Next();
        if(chunk.status != WFX::EpOk || chunk.done)
            break;

        for(std::uint32_t i = 0; i < chunk.data->RowCount(); ++i) {
            auto row = chunk.data->At(i);
            // borrowed until the next Next(), copy anything you need to keep
        }
    }

    co_return;
});
```

The first argument is `chunkRows`, how many rows one `Execute` asks for at a
time; the rest is the same SQL-plus-parameters shape `Query()` takes.
Postgres calls this a *portal*: the server keeps the query's cursor open
between rounds, and each `Next()` after the first re-serializes the request
to fetch the next batch through it, `WFX::EpParseChunkFetch` in
[Streaming large responses](overview.md#streaming-large-responses)'s terms.

A portal only survives past the `Sync` that ends its round while a
transaction is holding it open, so a stream that wasn't handed one already
open wraps itself in its own `BEGIN`/`COMMIT` automatically. Calling
`Stream()` on a `PgSession` that's already inside a transaction you started
with `Begin()` runs the stream inside that transaction instead, and leaves it
open when the stream ends, only closing the portal it opened for itself.

---

## Statement cache

Every connection keeps its own table of prepared statements, transparent to
you. An unnamed statement lives only until the next one replaces it, so
sending the same SQL as an unnamed statement every time makes Postgres parse
and plan it fresh each time; naming it keeps the plan on the server, and a
later request whose SQL and parameter types match sends `Bind` straight
through with no `Parse` at all.

SQL only earns a name after it's been seen `statementCacheMinUses` times
(default 2), not on first use, so a genuinely one-off statement never takes a
name away from something actually hot, and never pays for a `Parse` and
server-side plan storage it will only use once.

If a schema change invalidates a cached plan (`ALTER TABLE`, `DROP` and
recreate), Postgres reports it as SQLSTATE `0A000`; a session reset losing
the server's prepared statements reports `26000`. Either one moves a shared
epoch counter once, invalidating every connection's cache entries at once
rather than just the one that saw the error, and the next use of that SQL
anywhere re-`Parse`s automatically. The query that hit the stale plan still
fails once, its `ErrorResponse` surfaces normally through `Failed()`, and
every query after it, on every connection, recovers on its own without any
handling on your part.

Set `statementCacheSize = 0` to turn the cache off entirely and always send
unnamed statements, see [Tuning & limits](#tuning-limits).

---

## Query cancellation

Postgres cancels a running query over a second connection, quoting a backend
PID and secret key the server handed out during the original connection's
handshake; `CancelRequest` isn't a message the original connection can
carry. `PostgresEndpoint` wires this into
[`onAbort`](overview.md#onabort-cancellation): if the client that triggered a
query goes away before the reply comes back, WFX opens a
[side connection](overview.md#opening-a-side-connection), sends
`CancelRequest`, and leaves the original connection alone to finish naturally
and return to the pool.

This is the real implementation behind [`onAbort` cancellation](overview.md#onabort-cancellation)'s
own illustrative Postgres example. Nothing about it needs configuring beyond
`auxConnLimit` (see [Tuning & limits](#tuning-limits)), which caps how many
cancellations can be in flight at once. `auxConnLimit = 0` disables the
mechanism: `onAbort` still runs, but `OpenSideConnection()` fails
immediately since there's no capacity for it, so a query a client walked away
from still finishes normally on the server, just without anyone asking it to
stop early.

---

## Tuning & limits

```cpp
inline const auto Db = WFX::PostgresEndpoint{"db.internal:5432", WFX::PostgresConfig{
    .connLimit             = 8,
    .auxConnLimit          = 2,
    .connectTimeoutSeconds = 10,
    .requestTimeoutSeconds = 30,
    .user                  = "app",
    .password              = WFX::GetEnvString("PGPASSWORD"),
    .database              = "app",
    .applicationName       = "my-service",
}};
```

| Setting | Meaning |
|---------|---------|
| `connLimit` | Max simultaneous connections, per worker (default 8). Allocated exactly, not rounded up the way the raw primitive's own `connLimit` is, since a database's `max_connections` is a real ceiling worth respecting precisely. It's also per worker process: the server sees `connLimit * worker_processes` at most |
| `auxConnLimit` | Max simultaneous cancellation side connections, per worker (default 2), see [Query cancellation](#query-cancellation) |
| `connectTimeoutSeconds` | Budget for TCP connect + TLS + `StartupMessage` + auth combined (default 10) |
| `requestTimeoutSeconds` | Budget for one query round trip, measured on this side (default 30) |
| `idleTimeoutSeconds` | Idle pooled connections close after this long (default 300), kept under the ~350s idle cutoff common to proxies and NAT gateways, so a connection is closed here rather than silently dropped mid-use and failing on next use |
| `maxReconnectAttempts` / `reconnectBackoffBaseSeconds` / `reconnectBackoffMaxSeconds` | Same reconnect/backoff behavior as [the primitive](overview.md#reconnects-and-backoff) (defaults 3 / 1 / 30) |
| `prewarm` | Connections to open eagerly on startup (default 0) |
| `dnsRefreshSeconds` | `0` to follow the DNS record's own TTL, matching [the primitive](overview.md#dns-resolution-refresh) |
| `user` / `password` / `database` | Identity sent in the startup message. `password` is never logged |
| `applicationName` | Shows up in `pg_stat_activity`, useful for telling which service opened a connection |
| `encryption` / `authPolicy` | See [Encryption and authentication policy](#encryption-and-authentication-policy) |
| `searchPath` / `timeZone` | Sent as startup parameters, one round trip cheaper than issuing `SET` after connecting |
| `statementTimeoutMs` / `lockTimeoutMs` / `idleInTransactionTimeoutMs` | Server-side `statement_timeout` / `lock_timeout` / `idle_in_transaction_session_timeout`, `0` leaves each unset |
| `preferBinary` | Ask for binary-format columns where a codec exists (default `true`); has no effect on columns with no binary codec, which come back as text regardless |
| `maxMessageBytes` | Rejects any single incoming message whose declared length exceeds this (default 16 MiB), before it's used to size an allocation |
| `statementCacheSize` | Named statements kept per connection (default 64), `0` turns the cache off, see [Statement cache](#statement-cache) |
| `statementCacheMinUses` | Executions before SQL earns a name (default 2) |
