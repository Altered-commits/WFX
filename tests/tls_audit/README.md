# WFX TLS Audit

Adversarial audit of both directions of WFX's TLS trust: the **outbound
HttpEndpoint client** (the surface `endpoint_audit` never touches, since it runs
the client plaintext against a literal-IP mock) and WFX's **own inbound mTLS**
(`client_ca_path`, untested anywhere else in this repo).

Outbound: drives `WFX::HttpEndpoint` with `EpTlsRequire` against a small, hostile
TLS mock and attacks the client's certificate/protocol verification with a
man-in-the-middle mindset: an untrusted cert, a hostname-mismatched cert, an
expired cert, and a protocol-downgraded server. The client **must refuse every
one of them**: accepting any is a MitM hole.

Inbound (`mtls` phase): the audit itself becomes the client and dials WFX
directly, which runs this whole suite with `client_ca_path` set. A connection
with no client cert, an untrusted-CA cert, a self-signed cert, an expired cert,
or a leaf whose intermediate WFX was never given **must all be refused** -
accepting any of them turns "client cert required" into decoration.

## Scope: this suite vs `tests/endpoint_audit`

**This suite owns the certificate and trust surface**, and is the only audit that
needs `mkcert` plus a CA-trust step. `endpoint_audit` owns everything else about
the outbound client and stays plaintext; see its README's "Scope" section for
the full split, including why `UpgradeToTLS` is divided between the two suites.
Short version: a vector lands here if it needs a handshake to actually
*succeed* (pre-upgrade plaintext discarded at the trust boundary, refusing to
double-wrap an already-secure slot); it lives in `endpoint_audit` if the
handshake only needs to be *attempted*.

Note the `upgrade` persona below is the one listener here that does **not** start
in TLS: it speaks plaintext until the client asks to upgrade, which is the entire
point of the boundary test.

```
audit --(TLS)--> WFX (HTTPS) /call --(TLS)--> hostile mock personality
```

WFX itself runs as an HTTPS server (`--use-https`), so the audit talks TLS to
WFX for everything, including `/health`. That is no longer a *requirement* for
the outbound client: the client-side TLS context is created on demand and is
independent of what the server itself speaks (`endpoint_audit` drives
`UpgradeToTLS` against a plaintext WFX). It is kept here deliberately, so a
single run exercises inbound and outbound TLS at once.

## The crown jewels (SECURITY)

Refusal is proven twice for each hostile persona: the outbound call errors **and**
the mock recorded a *failed* TLS handshake (the client bailed at the TLS layer,
never completing it).

| Persona | Port | Cert | Client must |
|---|---|---|---|
| `good` | 8443 | mkcert-trusted, SAN matches | **accept** |
| `selfsigned` | 8444 | self-signed, unknown CA | **refuse** (untrusted) |
| `wronghost` | 8445 | trusted CA, SAN = `evil.example` | **refuse** (hostname mismatch) |
| `expired` | 8446 | trusted CA, `notAfter` in the past | **refuse** (expired) |
| `upgrade` | 8448 | mkcert-trusted, **plaintext until asked** | **accept**, and discard pre-upgrade bytes |
| `tls12` | 8447 | valid cert, server capped at TLS 1.2 | **refuse** (client requires 1.3) |

`good` also does double duty as the backend for every non-cert test below: the
HTTP framing/desync corpus replayed **over TLS** (smuggling defenses must still
hold under encryption), request-timeout under TLS, request-serialization injection
(CR/LF/NUL), a raw **truncation** attack (RST with no `close_notify`) that must
never be delivered as a complete response, and (`resumption` phase) verifying the
outbound client actually resumes a cached TLS session after a forced reconnect,
not just that it completes handshakes.

### Inbound (`mtls` phase)

The whole suite runs with `client_ca_path` set, so this table is what the audit
itself presents back to WFX. `client-good` is the default for every call in
every other phase (see `tls_send()`) - without it, this suite's entire non-mtls
corpus would fail at the handshake, not just this phase.

| Client cert | Signed by | WFX must |
|---|---|---|
| `client-good` | trusted CA (mkcert root) | **accept** |
| *(none)* | - | **refuse** (`SSL_VERIFY_FAIL_IF_NO_PEER_CERT`) |
| `client-otherca` | a different, untrusted CA | **refuse** |
| `client-selfsigned` | itself | **refuse** |
| `client-expired` | trusted CA, `notAfter` in the past | **refuse** |
| `client-viaint` (leaf only) | an intermediate WFX was never given | **refuse** (can't build the chain) |
| `client-viaint` + intermediate | same intermediate, sent alongside the leaf | **accept** |

## Run

```bash
cd tests/tls_audit

python3 tls_audit.py                      # everything
python3 tls_audit.py --phase verify       # just the cert-refusal crown jewels
python3 tls_audit.py --wfx-logs all       # stream every WFX log line
python3 tls_audit.py --list-phases        # list phases and exit
python3 tls_audit.py --ci                 # no colors, GitHub Actions log groups and error annotations
```

Phases: `handshake`, `verify`, `protocol`, `mtls`, `framing`, `desync`, `inject`, `resource`, `resumption`, `upgrade`.

### What it does

1. Generates certs into `certs/` (gitignored) for every persona in the table
   above, plus the `client-*` certs for the `mtls` phase. See `ensure_certs()`
   in `tls_audit.py` for exactly how each one is built and why.
2. Pins the WFX client's `outbound_ca_path` (and `client_ca_path`, for inbound
   mTLS) directly at the mkcert root CA, so `good`'s "must accept" outcome is
   deterministic instead of depending on `mkcert -install` having reached the
   OS trust store.
3. Boots the mock (one TLS listener per persona) and the app
   (`wfx run --use-https --https-port-override`), streaming both sides' logs
   live so a boot-time crash is visible immediately instead of looking like a
   flaky test.
4. Runs the `handshake`/`verify`/`protocol`/`mtls`/`framing`/`desync`/`inject`/
   `resource`/`resumption`/`upgrade` phases over TLS.

### Exit codes

| Code | Meaning |
|------|---------|
| `0` | all pass |
| `1` | a non-security vector failed, or the worker died mid-run |
| `2` | **SECURITY**: a bad cert, a protocol downgrade, a smuggle, or a truncation was accepted |
| `3` | **boot crash**: WFX never answered `/health` within the ready timeout; see `[wfx-crash:*]` above the report |

## Requirements

- `wfx` and `mkcert` on `PATH` (run `mkcert -install` once), `openssl`, Python 3.8+, Linux.
