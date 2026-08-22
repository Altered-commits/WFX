#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# Hostile Postgres mock upstream for the WFX client audit
#
# One persona per fixed port, each driving WFX::PostgresEndpoint's onConnect handshake
# (SSLRequest, StartupMessage, SCRAM-SHA-256 or cleartext auth) and its extended query
# protocol (Parse/Bind/Describe/Execute/Sync) down one specific hostile path. Connection
# and handshake faults are chosen per persona through --listen options, the same shape
# smtp_upstream.py uses. Faults that only make sense once a session is already open (a
# malformed RowDescription, a column-count mismatch, an oversized numeric) are chosen per
# query instead, by SQL text: any query containing PGAUDIT_<NAME> gets that fault's
# response rather than the default canned rows. See client_audit.py for what each persona
# and each PGAUDIT_ query proves.
#
# Control plane: a small line-based protocol on a dedicated port, not the wire protocol,
# since every real port speaks Postgres. "STATS <name>\n" answers with one JSON line.

import argparse
import base64
import hashlib
import hmac
import json
import secrets
import socket
import ssl
import struct
import sys
import threading
import time

_lock = threading.Lock()
_stats = {}  # name -> {"handshakes", "hs_fail", "auth_ok", "auth_fail", "queries", "cancels",
             #          "last_sql", "connect_attempts", "raw_startup"}

def _stat_init(name):
    return _stats.setdefault(name, {"handshakes": 0, "hs_fail": 0, "auth_ok": 0, "auth_fail": 0,
                                    "queries": 0, "cancels": 0, "last_sql": "", "connect_attempts": 0,
                                    "raw_startup": ""})

def _bump(name, key, n=1):
    with _lock:
        _stat_init(name)[key] += n

# -----------------------------------------------------------------------
# Connection plumbing
# -----------------------------------------------------------------------

class ConnClosed(Exception):
    pass

class ByteReader:
    """Buffered exact-length reader over a socket that may be swapped mid-connection
    (SSL upgrade), the wire equivalent of smtp_upstream.py's LineReader."""
    def __init__(self, sock):
        self.sock = sock
        self.buf = b""

    def set_sock(self, sock):
        self.sock = sock

    def read(self, n):
        while len(self.buf) < n:
            try:
                d = self.sock.recv(65536)
            except OSError:
                raise ConnClosed()
            if not d:
                raise ConnClosed()
            self.buf += d
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

# -----------------------------------------------------------------------
# Protocol constants, mirroring include/wfx/endpoint/postgres/protocol.hpp
# -----------------------------------------------------------------------
FE_BIND, FE_CLOSE, FE_DESCRIBE, FE_EXECUTE, FE_PARSE = b"B", b"C", b"D", b"E", b"P"
FE_PASSWORD, FE_QUERY, FE_SYNC, FE_TERMINATE = b"p", b"Q", b"S", b"X"

BE_AUTHENTICATION, BE_BACKEND_KEY_DATA = b"R", b"K"
BE_BIND_COMPLETE, BE_CLOSE_COMPLETE, BE_PARSE_COMPLETE = b"2", b"3", b"1"
BE_COMMAND_COMPLETE, BE_DATA_ROW, BE_ERROR_RESPONSE = b"C", b"D", b"E"
BE_EMPTY_QUERY_RESPONSE, BE_NO_DATA, BE_NOTICE_RESPONSE = b"I", b"n", b"N"
BE_PARAMETER_STATUS, BE_PORTAL_SUSPENDED, BE_READY_FOR_QUERY = b"S", b"s", b"Z"
BE_ROW_DESCRIPTION = b"T"
BE_COPY_OUT_RESPONSE, BE_NEGOTIATE_PROTOCOL, BE_NOTIFICATION_RESPONSE = b"H", b"v", b"A"

AUTH_OK, AUTH_KERBEROS_V5, AUTH_CLEARTEXT_PASSWORD = 0, 2, 3
AUTH_MD5_PASSWORD, AUTH_GSS, AUTH_GSS_CONTINUE, AUTH_SSPI = 5, 7, 8, 9
AUTH_SASL, AUTH_SASL_CONTINUE, AUTH_SASL_FINAL = 10, 11, 12

PROTOCOL_3_0 = 196608
SSL_REQUEST_CODE = 80877103
CANCEL_REQUEST_CODE = 80877102

OID_INT4, OID_TEXT, OID_NUMERIC, OID_INT4_ARRAY = 23, 25, 1700, 1007
FORMAT_BINARY = 1

# -----------------------------------------------------------------------
# Message framing, backend side. Every typed message is [1 byte type][int32
# length][payload], the length covering itself but not the type byte.
# -----------------------------------------------------------------------
def _frame(type_byte, payload):
    return type_byte + struct.pack(">I", len(payload) + 4) + payload

def _send(sock, opts, data):
    delay = opts.get("slow_trickle")
    if delay:
        for i in range(0, len(data), 1):
            sock.sendall(data[i:i + 1])
            time.sleep(delay)
    else:
        sock.sendall(data)

def _send_raw_type(sock, opts, type_byte):
    """A message with a type byte the client's switch cannot recognize."""
    _send(sock, opts, _frame(type_byte, b""))

def read_msg(reader):
    header = reader.read(5)
    type_byte = header[0:1]
    length = struct.unpack(">I", header[1:5])[0]
    payload = reader.read(length - 4) if length > 4 else b""
    return type_byte, payload

def cstr(s):
    return (s.encode() if isinstance(s, str) else s) + b"\x00"

# vvv Backend message builders vvv
def auth_ok():
    return _frame(BE_AUTHENTICATION, struct.pack(">i", AUTH_OK))

def auth_kind(kind, extra=b""):
    return _frame(BE_AUTHENTICATION, struct.pack(">i", kind) + extra)

def auth_sasl(mechanisms):
    body = b"".join(cstr(m) for m in mechanisms) + b"\x00"
    return _frame(BE_AUTHENTICATION, struct.pack(">i", AUTH_SASL) + body)

def auth_sasl_continue(data):
    return _frame(BE_AUTHENTICATION, struct.pack(">i", AUTH_SASL_CONTINUE) + data)

def auth_sasl_final(data):
    return _frame(BE_AUTHENTICATION, struct.pack(">i", AUTH_SASL_FINAL) + data)

def backend_key_data(pid, key):
    return _frame(BE_BACKEND_KEY_DATA, struct.pack(">ii", pid, key))

def parameter_status(key, value):
    return _frame(BE_PARAMETER_STATUS, cstr(key) + cstr(value))

def ready_for_query(status=b"I"):
    return _frame(BE_READY_FOR_QUERY, status)

def error_response(sqlstate, message, severity=b"ERROR"):
    body = b"S" + cstr(severity) + b"V" + cstr(severity) + b"C" + cstr(sqlstate) + \
        b"M" + cstr(message) + b"\x00"
    return _frame(BE_ERROR_RESPONSE, body)

def notice_response(message):
    body = b"S" + cstr("NOTICE") + b"C" + cstr("00000") + b"M" + cstr(message) + b"\x00"
    return _frame(BE_NOTICE_RESPONSE, body)

def parse_complete():
    return _frame(BE_PARSE_COMPLETE, b"")

def bind_complete():
    return _frame(BE_BIND_COMPLETE, b"")

def close_complete():
    return _frame(BE_CLOSE_COMPLETE, b"")

def no_data():
    return _frame(BE_NO_DATA, b"")

def portal_suspended():
    return _frame(BE_PORTAL_SUSPENDED, b"")

def command_complete(tag):
    return _frame(BE_COMMAND_COMPLETE, cstr(tag))

def row_description(columns, column_count_override=None):
    """columns: list of (name, type_oid, type_size)."""
    n = len(columns) if column_count_override is None else column_count_override
    body = struct.pack(">h", n)
    for name, oid, size in columns:
        body += cstr(name) + struct.pack(">ihihih", 0, 0, oid, size, -1, FORMAT_BINARY)
    return _frame(BE_ROW_DESCRIPTION, body)

def data_row(values, field_count_override=None):
    """values: list of bytes, or None for SQL NULL."""
    n = len(values) if field_count_override is None else field_count_override
    body = struct.pack(">h", n)
    for v in values:
        body += struct.pack(">i", -1) if v is None else struct.pack(">i", len(v)) + v
    return _frame(BE_DATA_ROW, body)

# -----------------------------------------------------------------------
# SCRAM-SHA-256 server side (RFC 5802 / RFC 7677), matching auth.hpp's client
# -----------------------------------------------------------------------
SCRAM_USER, SCRAM_PASSWORD = "audituser", "audit-pass-123"

def _pbkdf2(password, salt, iterations):
    return hashlib.pbkdf2_hmac("sha256", password.encode(), salt, iterations, dklen=32)

def _hmac(key, msg):
    return hmac.new(key, msg.encode() if isinstance(msg, str) else msg, hashlib.sha256).digest()

def _sha256(data):
    return hashlib.sha256(data).digest()

def _scram_attr(msg, key):
    for part in msg.split(","):
        if len(part) >= 2 and part[0] == key and part[1] == "=":
            return part[2:]
    return None

class ScramServer:
    """Holds the state needed across the three SCRAM messages, with a mangle hook
    at every point a hostile server can lie."""
    def __init__(self, opts):
        self.opts = opts
        self.client_first_bare = None
        self.client_nonce = None
        self.server_nonce = None
        self.salt = secrets.token_bytes(16)
        self.iterations = 4096

    def client_first(self, payload):
        # payload: CStr(mechanism) + int32(len) + client-first-message
        pos = payload.index(b"\x00") + 1
        n = struct.unpack(">i", payload[pos:pos + 4])[0]
        msg = payload[pos + 4:pos + 4 + n].decode()
        # gs2-header is always the fixed "n,," (no channel binding, no authzid), so the
        # bare message is everything after those three characters
        bare = msg[3:]
        self.client_first_bare = bare
        self.client_nonce = _scram_attr(bare, "r")

    def server_first(self):
        mangle = self.opts.get("mangle")
        nonce = self.client_nonce + secrets.token_hex(9) if mangle != "scram_nonce" else "not-a-valid-extension"
        self.server_nonce = nonce

        iterations = self.iterations
        if mangle == "scram_iterations_zero":
            iterations = 0
        elif mangle == "scram_iterations_over":
            iterations = 2_000_000

        salt = self.salt if mangle != "scram_salt" else b"\x00"  # decodes, but absurdly short
        self.iterations = iterations

        msg = "r=%s,s=%s,i=%d" % (nonce, base64.b64encode(salt).decode(), iterations)
        return msg.encode()

    def client_final(self, payload, server_first_msg):
        msg = payload.decode()
        proof_b64 = _scram_attr(msg, "p")
        proof = base64.b64decode(proof_b64) if proof_b64 else b""
        without_proof = msg.rsplit(",p=", 1)[0]

        salted = _pbkdf2(SCRAM_PASSWORD, self.salt, self.iterations)
        client_key = _hmac(salted, "Client Key")
        stored_key = _sha256(client_key)
        server_key = _hmac(salted, "Server Key")

        auth_message = "%s,%s,%s" % (self.client_first_bare, server_first_msg.decode(), without_proof)
        client_signature = _hmac(stored_key, auth_message)
        server_signature = _hmac(server_key, auth_message)

        derived_key = bytes(a ^ b for a, b in zip(proof, client_signature))
        ok = secrets.compare_digest(_sha256(derived_key), stored_key)

        if self.opts.get("mangle") == "scram_signature":
            server_signature = b"\x00" * len(server_signature)

        return ok, server_signature

# -----------------------------------------------------------------------
# PGAUDIT_* magic queries: faults that only make sense inside an open session.
# Each handler returns (messages, tag) for a non-streaming result, or None to
# fall through to the default good response.
# -----------------------------------------------------------------------
def _default_rows():
    return [row_description([("id", OID_INT4, 4), ("name", OID_TEXT, -1)]),
            data_row([struct.pack(">i", 1), b"ok"])], "SELECT 1"

def _magic_numeric_extreme():
    # ndigits=1, weight=1000 (far past NUMERIC_POW10K_MAX=38), digit=5000, positive, dscale=0
    numeric = struct.pack(">hhHh", 1, 1000, 0x0000, 0) + struct.pack(">h", 5000)
    return [row_description([("n", OID_NUMERIC, -1)]), data_row([numeric])], "SELECT 1"

def _magic_column_mismatch():
    # RowDescription declares 2 columns, DataRow claims 3
    return [row_description([("a", OID_INT4, 4), ("b", OID_INT4, 4)]),
            data_row([struct.pack(">i", 1), struct.pack(">i", 2), struct.pack(">i", 3)],
                     field_count_override=3)], "SELECT 1"

def _magic_rowdesc_negative():
    return [_frame(BE_ROW_DESCRIPTION, struct.pack(">h", -1))], "SELECT 1"

def _magic_datarow_first():
    # No RowDescription at all this round, straight to a nonzero-count DataRow
    return [data_row([struct.pack(">i", 1)])], "SELECT 1"

def _magic_array_overflow():
    # int4[], 6 dims of 50000 each: the product overflows int32, no element bytes needed
    hdr = struct.pack(">iii", 6, 0, OID_INT4)
    for _ in range(6):
        hdr += struct.pack(">ii", 50000, 1)
    return [row_description([("a", OID_INT4_ARRAY, -1)]), data_row([hdr])], "SELECT 1"

def _magic_array_baddim():
    hdr = struct.pack(">iii", 7, 0, OID_INT4)  # over MAX_ARRAY_DIMS (6)
    return [row_description([("a", OID_INT4_ARRAY, -1)]), data_row([hdr])], "SELECT 1"

def _magic_array_negdim():
    hdr = struct.pack(">iii", 1, 0, OID_INT4) + struct.pack(">ii", -1, 1)
    return [row_description([("a", OID_INT4_ARRAY, -1)]), data_row([hdr])], "SELECT 1"

def _magic_array_elements():
    # int4[3] = {10, 20, NULL}: a genuinely well-formed array, actual element bytes
    # included, the good path none of the other ARRAY_ vectors exercise
    hdr = struct.pack(">iii", 1, 0, OID_INT4) + struct.pack(">ii", 3, 1)
    elems = struct.pack(">ii", 4, 10) + struct.pack(">ii", 4, 20) + struct.pack(">i", -1)
    return [row_description([("a", OID_INT4_ARRAY, -1)]), data_row([hdr + elems])], "SELECT 1"

def _magic_array_truncated():
    # Declares one element 100 bytes long but supplies only 2, so NextElement has to
    # refuse the walk rather than read past what actually arrived
    hdr = struct.pack(">iii", 1, 0, OID_INT4) + struct.pack(">ii", 1, 1)
    elems = struct.pack(">i", 100) + b"AB"
    return [row_description([("a", OID_INT4_ARRAY, -1)]), data_row([hdr + elems])], "SELECT 1"

MAGIC_QUERIES = {
    "PGAUDIT_NUMERIC_EXTREME": _magic_numeric_extreme,
    "PGAUDIT_COLUMN_MISMATCH": _magic_column_mismatch,
    "PGAUDIT_ROWDESC_NEG": _magic_rowdesc_negative,
    "PGAUDIT_DATAROW_FIRST": _magic_datarow_first,
    "PGAUDIT_ARRAY_OVERFLOW": _magic_array_overflow,
    "PGAUDIT_ARRAY_BADDIM": _magic_array_baddim,
    "PGAUDIT_ARRAY_NEGDIM": _magic_array_negdim,
    "PGAUDIT_ARRAY_ELEMENTS": _magic_array_elements,
    "PGAUDIT_ARRAY_TRUNCATED": _magic_array_truncated,
}

STREAM_ROWS = 5

def _stream_dataset():
    return [data_row([struct.pack(">i", i)]) for i in range(1, STREAM_ROWS + 1)]

# -----------------------------------------------------------------------
# Per-connection session state and the extended-query batch driver
# -----------------------------------------------------------------------
class Session:
    def __init__(self):
        self.in_tx = False
        self.portals = {}   # portal name -> list of remaining DataRow messages
        self.prepared = {}  # statement name -> sql text, so a Bind that skips Parse
                            # (the real client's statement cache reusing a named
                            # statement) still resolves to the query it names

def handle_simple_query(sock, opts, sql, session):
    text = sql.decode(errors="replace").rstrip("\x00").strip().upper()
    if text.startswith("BEGIN"):
        session.in_tx = True
        tag = "BEGIN"
    elif text.startswith("COMMIT"):
        session.in_tx = False
        tag = "COMMIT"
    elif text.startswith("ROLLBACK TO SAVEPOINT"):
        tag = "ROLLBACK"
    elif text.startswith("ROLLBACK"):
        session.in_tx = False
        tag = "ROLLBACK"
    elif text.startswith("RELEASE SAVEPOINT"):
        tag = "RELEASE"
    elif text.startswith("SAVEPOINT"):
        tag = "SAVEPOINT"
    else:
        tag = "SELECT 0"
    _send(sock, opts, command_complete(tag))
    _send(sock, opts, ready_for_query(b"T" if session.in_tx else b"I"))

def handle_extended_batch(sock, opts, msgs, session, name):
    by_type = {}
    for t, payload in msgs:
        by_type.setdefault(t, []).append(payload)

    sql = b""
    for p in by_type.get(FE_PARSE, []):
        pos = p.index(b"\x00")
        stmt_name = p[:pos]
        sql = p[pos + 1:p.index(b"\x00", pos + 1)]
        session.prepared[stmt_name] = sql

    # The real client's statement cache reuses an already-prepared name after
    # statementCacheMinUses executions, sending Bind straight through with no Parse
    # in this batch at all. The SQL text still has to resolve to the same query.
    if FE_PARSE not in by_type and FE_BIND in by_type:
        bind_payload = by_type[FE_BIND][0]
        p0 = bind_payload.index(b"\x00")
        p1 = bind_payload.index(b"\x00", p0 + 1)
        sql = session.prepared.get(bind_payload[p0 + 1:p1], b"")

    # Recorded verbatim so the harness can prove a hostile parameter value never
    # altered the SQL text the server actually parsed, the core anti-injection claim
    if sql:
        with _lock:
            _stat_init(name)["last_sql"] = sql.decode(errors="replace")

    execute_payload = by_type.get(FE_EXECUTE, [None])[0]
    close_payload = by_type.get(FE_CLOSE, [None])[0]

    _bump(name, "queries")

    # A fetch round is Execute (+ Sync) with no Bind: it reuses an already-open
    # portal rather than opening a new one. A round that skipped Parse because the
    # statement cache already had it prepared still has a Bind, so FE_BIND is the
    # discriminator here, not FE_PARSE.
    if FE_BIND not in by_type and execute_payload is not None:
        pos = execute_payload.index(b"\x00")
        portal_name = execute_payload[:pos]
        limit = struct.unpack(">i", execute_payload[pos + 1:pos + 5])[0]
        rows = session.portals.get(portal_name, [])
        chunk, rest = (rows, []) if limit == 0 else (rows[:limit], rows[limit:])
        for r in chunk:
            _send(sock, opts, r)
        if rest:
            session.portals[portal_name] = rest
            _send(sock, opts, portal_suspended())
        else:
            session.portals.pop(portal_name, None)
            _send(sock, opts, command_complete("SELECT %d" % (STREAM_ROWS - len(rest))))
        _send(sock, opts, ready_for_query(b"T" if session.in_tx else b"I"))
        return

    # A close round: closing the named portal, nothing else in the batch
    if FE_BIND not in by_type and close_payload is not None:
        _send(sock, opts, close_complete())
        _send(sock, opts, ready_for_query(b"T" if session.in_tx else b"I"))
        return

    # A full round: Parse, Bind, Describe, Execute, Sync
    if opts.get("never_reply") == "query":
        _send(sock, opts, parse_complete())
        _send(sock, opts, bind_complete())
        time.sleep(3600)
        return

    # Fires on exactly the Nth extended-query round of a connection, not every round
    # from N onward, so the caller can see the query before it succeed and the query
    # after it succeed too. That before/after pair is what proves the client recovered
    # from the error rather than merely surviving it.
    force = opts.get("force_error_at")
    if force is not None:
        at, sqlstate, msg = force.split(":", 2)
        if _stat_init(name)["queries"] == int(at):
            _send(sock, opts, parse_complete())
            _send(sock, opts, bind_complete())
            _send(sock, opts, error_response(sqlstate.encode(), msg))
            _send(sock, opts, ready_for_query(b"T" if session.in_tx else b"I"))
            return

    _send(sock, opts, parse_complete())
    _send(sock, opts, bind_complete())

    text = sql.decode(errors="replace")
    magic = next((MAGIC_QUERIES[k] for k in MAGIC_QUERIES if k in text), None)

    if "PGAUDIT_UNKNOWN_TYPE" in text:
        _send_raw_type(sock, opts, b"\x01")
        _send(sock, opts, ready_for_query(b"T" if session.in_tx else b"I"))
        return

    if "PGAUDIT_HUGE_ROW" in text:
        n = int(opts.get("huge_bytes", 20 * 1024 * 1024))
        _send(sock, opts, row_description([("blob", OID_TEXT, -1)]))
        _send(sock, opts, data_row([b"A" * n]))
        _send(sock, opts, command_complete("SELECT 1"))
        _send(sock, opts, ready_for_query(b"T" if session.in_tx else b"I"))
        return

    # Protocol-confusion class: messages that only ever belong to a feature this
    # client has no code path for (COPY, protocol renegotiation, LISTEN/NOTIFY), or
    # that only ever belong to the connection handshake. wire.hpp's Parse has no case
    # for any of these mid-query, so they fall to its default: ERROR. Each one is its
    # own query so a wrongly-accepted one cannot mask a correctly-rejected one after it.
    if "PGAUDIT_COPY_UNSOLICITED" in text:
        _send(sock, opts, _frame(BE_COPY_OUT_RESPONSE, struct.pack(">bh", 0, 0)))
        _send(sock, opts, ready_for_query(b"T" if session.in_tx else b"I"))
        return

    if "PGAUDIT_NEGOTIATE_PROTO" in text:
        _send(sock, opts, _frame(BE_NEGOTIATE_PROTOCOL, struct.pack(">ii", 196608, 0)))
        _send(sock, opts, ready_for_query(b"T" if session.in_tx else b"I"))
        return

    if "PGAUDIT_NOTIFY_UNSOLICITED" in text:
        _send(sock, opts, _frame(BE_NOTIFICATION_RESPONSE,
                                 struct.pack(">i", 4242) + cstr("evil_channel") + cstr("evil_payload")))
        _send(sock, opts, ready_for_query(b"T" if session.in_tx else b"I"))
        return

    if "PGAUDIT_BACKENDKEY_MIDQUERY" in text:
        _send(sock, opts, backend_key_data(9999, 9999))
        _send(sock, opts, ready_for_query(b"T" if session.in_tx else b"I"))
        return

    # A non-streaming request (state->streamRows == 0 on the client) getting
    # PortalSuspended instead of CommandComplete. wire.hpp's case only acts when
    # streamRows != 0, so this has to land as a no-op the client just keeps reading
    # past, not a hang or a misread of what came next
    if "PGAUDIT_PORTAL_SUSPENDED_NOSTREAM" in text:
        _send(sock, opts, row_description([("id", OID_INT4, 4)]))
        _send(sock, opts, data_row([struct.pack(">i", 1)]))
        _send(sock, opts, portal_suspended())
        _send(sock, opts, command_complete("SELECT 1"))
        _send(sock, opts, ready_for_query(b"T" if session.in_tx else b"I"))
        return

    # CVE-2024-10977 class at the query level rather than the handshake: the message
    # text a hostile server controls has to survive being carried through PgError and
    # reflected in a JSON response without corrupting that response or executing
    # anywhere (a terminal escape sequence, embedded CR/LF, embedded NUL)
    if "PGAUDIT_ERROR_CONTROLCHARS" in text:
        _send(sock, opts, error_response(b"XX000", "line1\r\nline2\x1b[31m\x00tail"))
        _send(sock, opts, ready_for_query(b"T" if session.in_tx else b"I"))
        return

    # CommandComplete tag with a run of digits past uint64 range. SetCommandTag
    # (result.hpp) hands the trailing digit run to DecodeText<uint64_t>, which uses
    # std::from_chars and leaves the output untouched on out-of-range rather than
    # wrapping, so this has to come back as 0, not a wrapped or garbage count
    if "PGAUDIT_HUGE_TAG" in text:
        _send(sock, opts, row_description([("id", OID_INT4, 4)]))
        _send(sock, opts, data_row([struct.pack(">i", 1)]))
        _send(sock, opts, command_complete("UPDATE " + "9" * 40))
        _send(sock, opts, ready_for_query(b"T" if session.in_tx else b"I"))
        return

    if "PGAUDIT_STREAM" in text and execute_payload is not None:
        pos = execute_payload.index(b"\x00")
        portal_name = execute_payload[:pos]
        limit = struct.unpack(">i", execute_payload[pos + 1:pos + 5])[0]
        rows = _stream_dataset()
        _send(sock, opts, row_description([("n", OID_INT4, 4)]))
        chunk, rest = (rows, []) if limit == 0 else (rows[:limit], rows[limit:])
        for r in chunk:
            _send(sock, opts, r)
        if rest:
            session.portals[portal_name] = rest
            _send(sock, opts, portal_suspended())
        else:
            _send(sock, opts, command_complete("SELECT %d" % len(rows)))
        _send(sock, opts, ready_for_query(b"T" if session.in_tx else b"I"))
        return

    if magic:
        messages, tag = magic()
        for m in messages:
            _send(sock, opts, m)
        _send(sock, opts, command_complete(tag))
        _send(sock, opts, ready_for_query(b"T" if session.in_tx else b"I"))
        return

    messages, tag = _default_rows()
    for m in messages:
        _send(sock, opts, m)
    _send(sock, opts, command_complete(tag))
    _send(sock, opts, ready_for_query(b"T" if session.in_tx else b"I"))

def query_loop(sock, reader, opts, name):
    session = Session()
    while True:
        msgs = []
        while True:
            t, payload = read_msg(reader)
            if t == FE_TERMINATE:
                return
            if t == FE_QUERY:
                handle_simple_query(sock, opts, payload, session)
                continue
            msgs.append((t, payload))
            if t == FE_SYNC:
                break
        if msgs:
            handle_extended_batch(sock, opts, msgs, session, name)

# -----------------------------------------------------------------------
# Connection driver: SSL negotiation, StartupMessage, authentication
# -----------------------------------------------------------------------
def _fault(sock, opts, stage):
    """Runs whichever fault primitive is armed for this stage. Returns True if
    the caller should stop driving the connection."""
    if opts.get("flood_at") == stage:
        while True:
            _send(sock, opts, notice_response("flood"))
    if opts.get("huge_at") == stage:
        n = int(opts.get("huge_bytes", 20 * 1024 * 1024))
        _send(sock, opts, notice_response("A" * n))
        return True
    if opts.get("unknown_type_at") == stage:
        _send_raw_type(sock, opts, b"\x01")
        return True
    if opts.get("error_at") == stage:
        # CVE-2024-10977 class: a server not yet trusted (pre-auth, pre-TLS-verify)
        # feeding the client arbitrary bytes through an error message. The client
        # has to abort cleanly rather than surface this content anywhere sensitive
        _send(sock, opts, error_response(b"XX000", "attacker-controlled: \x1b[31mfake\x1b[0m\r\ntext"))
        sock.close()
        return True
    if opts.get("never_reply") == stage:
        time.sleep(3600)
        return True
    if opts.get("drop_after") == stage:
        sock.close()
        return True
    return False

def serve_conn(raw_sock, name, opts):
    sock = raw_sock
    reader = ByteReader(sock)
    sock.settimeout(opts.get("timeout", 60.0))

    try:
        header = reader.read(8)
        length, code = struct.unpack(">ii", header)

        if code == CANCEL_REQUEST_CODE:
            _bump(name, "cancels")
            sock.close()
            return

        # fail_first_connect: refuses only the first TCP connection this persona ever
        # sees, then behaves normally. Proves a reconnect gets a genuinely fresh
        # SlotState rather than anything surviving from the failed attempt
        # (CVE-2018-10915 class: stale client-side state carried across connections)
        if opts.get("fail_first_connect"):
            with _lock:
                attempt = _stat_init(name)["connect_attempts"] + 1
                _stats[name]["connect_attempts"] = attempt
            if attempt == 1:
                sock.close()
                return

        if code == SSL_REQUEST_CODE:
            verdict = opts.get("ssl_verdict", "S")
            if verdict == "garbage":
                _send(sock, opts, b"?")
                sock.close()
                return
            if verdict == "multibyte":
                # CVE-2021-23214 class: extra plaintext right after the verdict, spliced
                # in before the TLS handshake even starts
                _send(sock, opts, b"S" + b"MAIL FROM:<mitm@evil>\r\n")
            else:
                _send(sock, opts, verdict.encode())

            if verdict != "S":
                sock.close()
                return

            if _fault(sock, opts, "ssl_verdict"):
                return

            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(certfile=opts["cert"], keyfile=opts["key"])
            try:
                sock = ctx.wrap_socket(raw_sock, server_side=True)
            except (ssl.SSLError, OSError) as e:
                _bump(name, "hs_fail")
                print("pg mock '%s' TLS handshake failed: %r" % (name, e), flush=True)
                return
            _bump(name, "handshakes")
            sock.settimeout(opts.get("timeout", 60.0))
            reader.set_sock(sock)

            header = reader.read(8)
            length, code = struct.unpack(">ii", header)

        if code != PROTOCOL_3_0:
            sock.close()
            return

        params_raw = reader.read(length - 8)  # already consumed 4(len)+4(code)
        # Kept as hex, so an embedded NUL in a startup field (which would desync the
        # C-string parameters that follow it) survives round-tripping through JSON
        with _lock:
            _stat_init(name)["raw_startup"] = params_raw.hex()

        if _fault(sock, opts, "startup"):
            return

        # vvv Authentication vvv
        wrong = opts.get("wrong_auth")
        if wrong == "md5":
            _send(sock, opts, auth_kind(AUTH_MD5_PASSWORD, b"\x00\x00\x00\x00"))
            sock.close()
            return
        if wrong == "gss":
            _send(sock, opts, auth_kind(AUTH_GSS))
            sock.close()
            return
        if wrong == "sspi":
            _send(sock, opts, auth_kind(AUTH_SSPI))
            sock.close()
            return
        if wrong == "cleartext":
            _send(sock, opts, auth_kind(AUTH_CLEARTEXT_PASSWORD))
            t, payload = read_msg(reader)
            ok = t == FE_PASSWORD and payload.rstrip(b"\x00").decode() == SCRAM_PASSWORD
            if ok and opts.get("allow_cleartext"):
                _bump(name, "auth_ok")
                _send(sock, opts, auth_ok())
            else:
                _bump(name, "auth_fail")
                _send(sock, opts, error_response(b"28P01", "password authentication failed"))
                sock.close()
                return
        else:
            mechs = opts.get("scram_offer")
            if mechs == "empty":
                _send(sock, opts, auth_sasl([]))
            elif mechs == "plus_only":
                _send(sock, opts, auth_sasl(["SCRAM-SHA-256-PLUS"]))
            elif mechs == "garbage":
                _send(sock, opts, auth_sasl(["NOT-A-MECHANISM"]))
            elif mechs == "mixed":
                # The common real-world shape: a server that supports channel binding
                # offers both. WFX has no channel-binding support, so the only correct
                # behaviour is picking plain SCRAM-SHA-256 out of the pair and
                # continuing, not refusing the whole exchange
                _send(sock, opts, auth_sasl(["SCRAM-SHA-256-PLUS", "SCRAM-SHA-256"]))
            else:
                _send(sock, opts, auth_sasl(["SCRAM-SHA-256"]))

            if mechs in ("empty", "plus_only", "garbage"):
                sock.close()
                return

            if _fault(sock, opts, "auth_challenge"):
                return

            t, payload = read_msg(reader)
            if t != FE_PASSWORD:
                sock.close()
                return

            scram = ScramServer(opts)
            scram.client_first(payload)
            server_first_msg = scram.server_first()
            _send(sock, opts, auth_sasl_continue(server_first_msg))

            if _fault(sock, opts, "auth_final"):
                return

            t, payload = read_msg(reader)
            if t != FE_PASSWORD:
                sock.close()
                return

            client_ok, server_signature = scram.client_final(payload, server_first_msg)

            # The server side of the exchange failing for its own reasons (expired
            # role, disabled account) rather than a bad proof. RFC 5802 spells this as
            # e=<reason> in place of v=<signature>, on the same AuthenticationSASLFinal
            # step; auth.hpp's ServerFinalIsError is the client's counterpart, never
            # exercised until now
            if opts.get("mangle") == "scram_server_error":
                _send(sock, opts, auth_sasl_final(b"e=invalid-authorization-message"))
                sock.close()
                return

            if not client_ok:
                _bump(name, "auth_fail")
                _send(sock, opts, error_response(b"28P01", "SCRAM verification failed"))
                sock.close()
                return

            final_msg = ("v=" + base64.b64encode(server_signature).decode()).encode()
            _send(sock, opts, auth_sasl_final(final_msg))
            _bump(name, "auth_ok")
            _send(sock, opts, auth_ok())

        if _fault(sock, opts, "backendkeydata"):
            return

        _send(sock, opts, parameter_status("server_version", "16.0 (wfx-mock)"))
        _send(sock, opts, parameter_status("client_encoding", "UTF8"))
        _send(sock, opts, backend_key_data(int(opts.get("pid", 4242)), int(opts.get("secret", 1337))))

        if _fault(sock, opts, "ready"):
            return

        _send(sock, opts, ready_for_query(b"I"))

        query_loop(sock, reader, opts, name)

    except (ConnClosed, OSError, ssl.SSLError, struct.error, IndexError, ValueError):
        return
    finally:
        try:
            sock.close()
        except OSError:
            pass

def listener(host, port, name, opts):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(256)
    print("pg mock '%s' on %s:%d" % (name, host, port), flush=True)
    while True:
        try:
            conn, _ = srv.accept()
        except OSError:
            break
        threading.Thread(target=serve_conn, args=(conn, name, opts), daemon=True).start()

# -----------------------------------------------------------------------
# Control plane: line-based, not the wire protocol, since every real port
# speaks Postgres
# -----------------------------------------------------------------------
def handle_control(conn):
    try:
        conn.settimeout(5.0)
        buf = b""
        while b"\n" not in buf:
            d = conn.recv(4096)
            if not d:
                return
            buf += d
        line = buf.split(b"\n", 1)[0].decode("latin-1", "replace").strip()
        parts = line.split(" ", 1)
        cmd = parts[0] if parts else ""

        with _lock:
            if cmd == "STATS" and len(parts) > 1:
                reply = json.dumps(_stat_init(parts[1]))
            elif cmd == "RESET" and len(parts) > 1:
                _stats.pop(parts[1], None)
                reply = '{"ok":true}'
            else:
                reply = '{"error":"unknown command"}'

        conn.sendall((reply + "\n").encode())
    except OSError:
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass

def control_listener(host, port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(64)
    print("pg mock control on %s:%d" % (host, port), flush=True)
    while True:
        try:
            conn, _ = srv.accept()
        except OSError:
            break
        threading.Thread(target=handle_control, args=(conn,), daemon=True).start()

def parse_listen(spec):
    kv = {}
    for part in spec.split(","):
        if "=" in part:
            k, v = part.split("=", 1)
            kv[k.strip()] = v.strip()
    return kv

_FLOAT_KEYS = ("slow_trickle", "timeout")

def coerce_opts(kv):
    opts = dict(kv)
    for k in _FLOAT_KEYS:
        if k in opts:
            opts[k] = float(opts[k])
    return opts

def main():
    ap = argparse.ArgumentParser(description="WFX client_audit hostile Postgres mock")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--control-port", type=int, required=True)
    ap.add_argument("--listen", action="append", required=True,
                    help="name=..,port=..,cert=..,key=..[,drop_after=..][,...]")
    args = ap.parse_args()

    threads = [threading.Thread(target=control_listener, args=(args.host, args.control_port), daemon=True)]
    threads[0].start()

    for spec in args.listen:
        kv = parse_listen(spec)
        opts = coerce_opts(kv)
        name = kv.get("name", "l%s" % kv["port"])
        t = threading.Thread(target=listener, args=(args.host, int(kv["port"]), name, opts), daemon=True)
        t.start()
        threads.append(t)

    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass

if __name__ == "__main__":
    main()
