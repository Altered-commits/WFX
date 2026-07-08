# WFX TLS Audit

Adversarial audit of WFX's **outbound HttpEndpoint over TLS**: the surface the
endpoint audit never touches (it runs the client plaintext against a literal-IP
mock). This drives `WFX::HttpEndpoint` with `EpTlsRequire` against a small,
hostile TLS mock and attacks the client's certificate/protocol verification with a
man-in-the-middle mindset: an untrusted cert, a hostname-mismatched cert, an
expired cert, and a protocol-downgraded server. The client **must refuse every
one of them**: accepting any is a MitM hole.

```
harness --(TLS)--> WFX (HTTPS) /call --(TLS)--> hostile mock personality
```

WFX itself runs as an HTTPS server (`--use-https`): a TLS *endpoint* only drives
the outbound client's SSL path when the WFX server is HTTPS too, so the harness
talks TLS to WFX for everything, including `/health`.

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
| `tls12` | 8447 | valid cert, server capped at TLS 1.2 | **refuse** (client requires 1.3) |

`good` also does double duty as the backend for every non-cert test below: the
HTTP framing/desync corpus replayed **over TLS** (smuggling defenses must still
hold under encryption), request-timeout under TLS, request-serialization injection
(CR/LF/NUL), and a raw **truncation** attack (RST with no `close_notify`) that must
never be delivered as a complete response.

## Run

```bash
cd tests/tls_audit

python3 tls_audit.py                      # everything
python3 tls_audit.py --phase verify       # just the cert-refusal crown jewels
python3 tls_audit.py --wfx-logs all       # stream every WFX log line
python3 tls_audit.py --list-phases        # list phases and exit
python3 tls_audit.py --ci                 # no colors, GitHub Actions log groups and error annotations
```

Phases: `handshake`, `verify`, `protocol`, `framing`, `desync`, `inject`, `resource`.

### What it does

1. Generates certs into `certs/` (gitignored): `good`/`wronghost` via **mkcert**,
   `selfsigned` via **openssl** (self-signed), `expired` via **openssl** signed by
   the mkcert CA with `-days -1` (backdated `notAfter`; portable across OpenSSL
   versions, unlike the `-not_before`/`-not_after` flags which are OpenSSL-3+ only).
   `tls12` reuses the `good` cert with the mock server capped at TLS 1.2.
2. Pins the WFX client's `ca_cert_path` directly at the mkcert root CA file
   (instead of leaving it empty and relying on `mkcert -install` having reached
   the OS's OpenSSL trust store, which needs sudo on Linux and may silently not
   happen). This makes `good`'s "must accept" outcome deterministic without
   weakening any refusal test: hostname/expiry/downgrade checks are independent
   of the trust anchor.
3. Boots the mock (one TLS listener per persona) and the app (`wfx run --use-https
   --https-port-override`), tailing WFX's worker/master logs and crash dumps
   **live** so a boot-time SIGSEGV prints as it happens, before revival truncates
   the log file. The mock's own stdout/stderr is streamed too (prefixed `[mock]`),
   so a listener that fails to bind or load its cert is visible immediately instead
   of silently masquerading as a WFX bug.
4. Runs the handshake / verify / protocol / framing / desync / inject / resource
   phases over TLS.

### Exit codes

| Code | Meaning |
|------|---------|
| `0` | all pass |
| `1` | a non-security vector failed, or the worker died mid-run |
| `2` | **SECURITY**: a bad cert, a protocol downgrade, a smuggle, or a truncation was accepted |
| `3` | **boot crash**: WFX never answered `/health` within the ready timeout; see `[wfx-crash:*]` above the report |

## Requirements

- `wfx` and `mkcert` on `PATH` (run `mkcert -install` once), `openssl`, Python 3.8+, Linux.
