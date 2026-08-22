# WFX interop audit

Real-upstream interoperability testing for the three protocol clients WFX ships:
`WFX::PostgresEndpoint`, `WFX::SmtpEndpoint` and `WFX::HttpEndpoint`.

Where `tests/client_audit` is adversarial (hand-rolled mocks, hostile bytes on
purpose, exit code `2` for a security finding), this suite is the opposite: it
proves the clients actually interoperate with real, spec-compliant servers,
every leg on real TLS, the way a real deployment would run them. Nothing here
sends malformed input; there is no security exit code, only pass or fail.

Postgres and SMTP are real Docker containers. SMTP runs twice: once offering
`AUTH PLAIN` (the client's preferred mechanism, so that's the path exercised
against it) and once offering only `AUTH LOGIN`, which is what actually forces
the client's LOGIN fallback path rather than asserting it exists. The HTTP leg
doesn't use a container at all: `WFX::HttpEndpoint` is driven against a second,
ordinary WFX server (`upstream/`) running real HTTPS through WFX's own
`[SSL] cert_path`/`key_path`, so that leg proves WFX's own client and server
genuinely interoperate, not that the client can talk to somebody else's.

The raw `WFX::Endpoint<>` primitive is audited separately, in
`tests/endpoint_audit`; hostile/malformed-input coverage for these same three
clients is `tests/client_audit`. This suite only asks "does this actually work
against the real thing."

---

## Architecture

```
interop_audit.py --HTTP--> WFX app /pg/*     --Postgres(TLS)--> postgres:16-alpine (Docker)
                 --HTTP--> WFX app /smtp/*    --SMTP(STARTTLS)-> smtp4dev, smtp4dev-loginonly (Docker)
                 --HTTP--> WFX app /http/call --HTTPS----------> upstream/ (a second real WFX server)
                 +-------- smtp4dev's own REST API, to confirm real delivery -----+
```

The audit drives `app/`'s routes the same way `client_audit` drives its own
app: one inbound request in, one outbound protocol call out, the outcome
reflected back as JSON. For SMTP, the harness never just trusts what the
client claims happened — it independently confirms delivery through smtp4dev's
own REST API (`/api/messages`), including whether the message was actually
received over a secure connection.

**`app/`** is the client under test, a WFX project scaffolded with `wfx new`.
Its Postgres, SMTP and HTTP endpoint configs are deliberately tuned like a
real deployment would tune them (real connection pool sizes, real
reconnect/backoff policy, `SCRAM_ONLY`/`REQUIRED` encryption, session-level
statement/lock/idle-in-transaction timeouts, `EpTlsRequire`, never
`EpTlsInsecure`), not left at toy defaults. See `app/src/main.cpp`'s own
route comments for exactly what each one exercises: real transactions
(commit *and* rollback, checked from a second pooled connection afterward, so
it's real atomicity rather than a mock's word for it), savepoints, chunked
streaming via `PostgresEndpoint::Stream`, multi-recipient SMTP delivery,
RFC 5321 dot-stuffing, `RSET` mid-transaction, both the step-by-step
`SmtpTransaction` API and the single-call `SendMail` wrapper, and every HTTP
verb, status code, a real chunked response, and a client-side timeout against
a genuinely slow (not hostile) real server.

**`upstream/`** is not a mock. It's an ordinary WFX server, real HTTPS
enabled, with a handful of routes (`/get`, `/echo`, `/status/<code:uint>`,
`/delay/<ms:uint>`, `/stream/<n:uint>` using WFX's own
`FlushStart`/`Write`/`Flush`/`FlushEnd`, `/basic-auth`) shaped to give
`HttpEndpoint` real verbs, real status codes, a real chunked response and a
real slow response to exercise its timeout against.

**`docker-compose.yml`** brings up `postgres` (SSL on, its own cert) and two
`smtp4dev` instances (STARTTLS, `AuthenticationRequired`, one PLAIN+LOGIN, one
LOGIN-only). See `interop_audit.py`'s `ensure_certs()` for how the one shared
self-signed cert (and Postgres's own permission-fixed copy) gets generated.

---

## Run

```bash
cd tests/interop_audit

python3 interop_audit.py                  # generates certs, brings up Docker, runs everything
python3 interop_audit.py --phase smtp     # one phase
python3 interop_audit.py --list-phases    # what phases exist
python3 interop_audit.py --no-docker      # you already ran `docker compose up -d` yourself
python3 interop_audit.py --keep-docker    # leave containers running after this run
```

One command is the whole setup: `python3 interop_audit.py` generates the TLS
cert (idempotent, skipped once `certs/` exists), fixes its permissions for
Postgres, runs `docker compose up -d --wait`, patches `outbound_ca_path` /
`cert_path` / `key_path` in the two apps' `wfx.local.toml`, boots the HTTPS
upstream and the app under test, drives every phase, and prints a per-phase
report. `docker compose up -d` by hand works too, once `certs/` already exists
from a prior run.

Containers are torn down (`docker compose down -v`) after every run by
default, so nothing keeps running in the background once the suite exits.
`--keep-docker` skips that, for someone iterating on this suite repeatedly who
doesn't want to pay image-pull/`initdb` cost again on the next run.
`--no-docker` (assumes Docker's already up) leaves teardown alone too, since
that flag means you're managing the containers yourself.

> The config-patching step edits `app/config/wfx.local.toml` and
> `upstream/config/wfx.local.toml` in place, the same way `client_audit` edits
> its own `outbound_ca_path`. Don't commit those lines with a real path in
> them: checked in, they point every other checkout at a cert that isn't
> there. Leave `cert_path` / `key_path` / `outbound_ca_path` as `"..."` / `""`.

### Requirements

- `wfx` and `openssl` on `PATH` (or `--wfx /path/to/wfx`)
- Docker with the `docker compose` plugin (not the standalone `docker-compose`)
- Python 3.8+, standard library only
- Linux

### Flags

| Flag | Default | Description |
|------|---------|-------------|
| `--host` | `127.0.0.1` | Host for WFX and every upstream |
| `--port` | `8080` | WFX inbound port (the app under test) |
| `--wfx` | `wfx` | Path to the wfx binary |
| `--app-dir` | `app` | App directory passed to `wfx run` |
| `--ready-timeout` | `30` | Seconds to wait for `/health` |
| `--phase` | `all` | Run a single phase |
| `--list-phases` | n/a | Print phases and exit |
| `--wfx-logs` | `important` | Stream WFX logs live: `off`, `crash`, `important`, `all` |
| `--ci` | off | No colors, GitHub Actions `::group::` and `::error::` commands |
| `--no-docker` | off | Skip `docker compose up`, assume it's already running (also skips teardown) |
| `--keep-docker` | off | Skip `docker compose down -v` after the run |

### Exit codes

| Code | Meaning |
|------|---------|
| `0` | Everything passed |
| `1` | A correctness failure, or the WFX worker died during the run |
| `3` | WFX never answered `/health`, so nothing was exercised |

---

## Phases

Run `python3 interop_audit.py --list-phases` to get this list straight from
the script.

| Phase | What it proves |
|-------|----------------|
| **postgres** | Real SCRAM auth over real TLS; int/bool/text decode; a real `INSERT ... RETURNING`; a committed transaction visible from a second pooled connection and a rolled-back one that isn't, real atomicity; all three isolation levels accepted; `SAVEPOINT`/`ROLLBACK TO`/`COMMIT` leaving only the pre-savepoint row; a chunked read via `Stream` across many chunks; the statement cache staying correct across repeated executions |
| **smtp** | A full STARTTLS + AUTH PLAIN transaction, and the same against a LOGIN-only server so the fallback path is genuinely forced, not assumed; delivery confirmed through smtp4dev's own API, including `secureConnection`; multi-recipient delivery (`RCPT TO` called more than once); RFC 5321 dot-stuffing round-tripping through a real MTA; the `SendMail` single-call wrapper as its own code path; `RSET` mid-transaction followed by a working fresh transaction on the same connection |
| **http** | Every verb (`GET`/`POST`/`PUT`/`PATCH`/`DELETE`) against a real server; a representative status-code matrix; Basic-Auth accepted and refused correctly; a real chunked response fully reassembled; a client-side request timeout firing against a genuinely slow (not hostile) real server |
