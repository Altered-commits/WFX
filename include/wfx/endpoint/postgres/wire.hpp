// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_ENDPOINT_POSTGRES_WIRE_HPP
#define WFX_INC_WFX_ENDPOINT_POSTGRES_WIRE_HPP

// -----------------------------------------------------------------------
// Request encoding and response parsing, the two halves of EndpointDesc.
//
// One request becomes the extended query sequence, which is what lets
// parameters travel as typed values instead of being pasted into SQL:
//
//   Parse     name the statement and declare its parameter types
//   Bind      supply the values and pick the result format
//   Describe  ask for the RowDescription that types the columns
//   Execute   run it
//   Sync      close the exchange and ask for ReadyForQuery
//
// Parsing leaves any trailing partial message untouched. The engine keeps
// unconsumed bytes and redelivers them, and every message is length-prefixed,
// so nothing needs buffering on this side.
// -----------------------------------------------------------------------

#include "connection.hpp"
#include "result.hpp"
#include "types.hpp"

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace WFX::Postgres::Detail {

// Any scalar codec writes at most a length prefix plus sixteen bytes. PgUuid
// and PgInterval sit exactly on this, so widening either one means raising it.
inline constexpr std::uint64_t PG_MAX_SCALAR_PARAM = 4 + 16;

// Postgres refuses a field larger than this, and staying under it keeps every
// length that reaches the wire inside a positive int32
inline constexpr std::uint64_t PG_MAX_PARAM_BYTES = 1024u * 1024u * 1024u;

// Bind takes either one format code covering every value, or one per value.
// The uniform form is what this always sends.
inline constexpr std::int16_t FORMAT_COUNT_UNIFORM = 1;

// Longest identifier the server accepts, NAMEDATALEN minus its terminator
inline constexpr std::size_t MAX_IDENTIFIER_LEN = 63;

// A portal is destroyed by the Sync that ends its round unless a transaction is
// holding it, which makes these the prologue and epilogue of every stream that
// was not handed one already open
inline constexpr std::string_view SQL_STREAM_BEGIN = "BEGIN";
inline constexpr std::string_view SQL_STREAM_COMMIT = "COMMIT";

// A parameter cannot stand in for an identifier, so savepoint statements carry
// their target as text. Serialize composes it, and the name is checked against
// the identifier grammar before it gets there.
enum class PgStatementKind : std::uint8_t {
    QUERY,
    SAVEPOINT,
    ROLLBACK_TO,
    RELEASE_SAVEPOINT,
};

inline bool IsValidIdentifier(std::string_view name) noexcept
{
    if(name.empty() || name.size() > MAX_IDENTIFIER_LEN)
        return false;

    for(std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        const bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        const bool digit = c >= '0' && c <= '9';

        // Digits are fine except in the first position, where they would make
        // it a number rather than a name
        if(!alpha && !(digit && i > 0))
            return false;
    }

    return true;
}

// -----------------------------------------------------------------------
// Upper bound on a parameter's encoded size, so the buffer is sized in one
// pass instead of encoding twice to measure
// -----------------------------------------------------------------------
template <typename T> inline std::uint64_t ParamUpperBound([[maybe_unused]] const T& v) noexcept
{
    if constexpr(std::is_same_v<std::decay_t<T>, PgNull>)
        return 4;
    else if constexpr(std::is_convertible_v<T, std::string_view>)
        return 4 + std::string_view{v}.size();
    // Not convertible to string_view, and unlike every other non-text type it has no fixed width
    else if constexpr(std::is_same_v<std::decay_t<T>, PgBytes>)
        return 4 + static_cast<std::uint64_t>(v.size);
    else
        return PG_MAX_SCALAR_PARAM;
}

// -----------------------------------------------------------------------
// PgRequest
//
// Owns its encoded parameters, because SendPayloadAwaitable takes the request
// by value and then moves it: a string moves as a pointer swap, where an
// inline buffer would be copied byte for byte twice over.
//
// sql and paramOids stay views. SQL is normally a literal and the OID array
// is a static per-instantiation constant, so both outlive the request.
// -----------------------------------------------------------------------
struct PgRequest {
    std::string_view sql;
    const std::uint32_t* paramOids = nullptr;
    WFX::String params; // each value already length-prefixed, as Bind wants

    // Rows one Execute may return, which is what makes this a stream. Zero
    // fetches the whole result in one round, as an ordinary query.
    std::uint32_t streamRows = 0;

    std::uint16_t paramCount = 0;

    // Ask for binary columns, subject to the endpoint also preferring them
    bool binaryResults = true;

    // Cleared when the parameters were too large to encode, which makes
    // Serialize refuse the request rather than send it without its values
    bool encoded = true;

    // Sent on a reserved connection, so an open transaction on it is the
    // caller's and must be left alone
    bool pinned = false;

    // For the savepoint kinds, sql carries the savepoint name instead of a
    // statement, and Serialize builds the statement around it
    PgStatementKind kind = PgStatementKind::QUERY;
};

// -----------------------------------------------------------------------
// Encodes every parameter into the request in a single pass
// -----------------------------------------------------------------------
template <typename... Ts> inline bool EncodeParams(PgRequest& req, const Ts&... params)
{
    if constexpr(sizeof...(Ts) == 0)
        return true;
    else {
        std::uint64_t needed = 0;
        ((needed += ParamUpperBound(params)), ...);

        // Past this a length no longer fits a positive int32, and Bind reads a
        // negative length as SQL NULL, so an oversized value would bind as null
        // instead of failing
        if(needed > PG_MAX_PARAM_BYTES)
            return false;

        req.params.resize(needed);

        PgWriter w(req.params.data(), static_cast<std::uint32_t>(needed));
        (EncodeParam(w, params), ...);

        if(!w.Ok()) {
            req.params.clear();
            return false;
        }

        // The bound is exact for strings and generous for scalars, so the
        // unused tail is trimmed rather than sent
        req.params.resize(w.Pos());
        return true;
    }
}

// -----------------------------------------------------------------------
// Serialize
// -----------------------------------------------------------------------
inline Shared::SerializeResult Serialize(void* slotStateVoid, const void* reqVoid, char* buf, std::uint32_t bufLen,
                                         std::uint32_t* written, std::uint64_t* /*streamKey*/) noexcept
{
    auto* state = static_cast<SlotState*>(slotStateVoid);
    const auto& req = *static_cast<const PgRequest*>(reqVoid);

    if(!req.encoded)
        return Shared::SerializeResult::ERROR;

    const std::int16_t resultFormat = (req.binaryResults && state->options->preferBinary) ? FORMAT_BINARY : FORMAT_TEXT;

    PgWriter w(buf, bufLen);

    // Releasing a reservation only unpins the connection, so a slot can come
    // back still inside a transaction. Assigned rather than accumulated because
    // Serialize runs again when the buffer was too small.
    state->expectedReady = 1;
    state->pendingStmt = STMT_NONE;
    state->streamRows = req.streamRows;

    // An abandoned stream leaves a phase behind, which the next stream on this
    // slot would read as a portal it can fetch from
    if(state->streamRows == 0)
        state->streamPhase = PgStreamPhase::NONE;

    // The engine hands the same request back to fetch each further chunk, so a
    // phase past NONE means the portal is already open and this round only has
    // to move it along
    const bool continuing = state->streamPhase != PgStreamPhase::NONE;

    if(continuing) {
        // The chunk the caller is holding was parsed into the same result this
        // round will fill, so its rows go once the reply starts arriving
        state->streamRowsStale = true;

        if(state->streamPhase == PgStreamPhase::FETCH) {
            w.Begin(FE_EXECUTE);
            w.CStr(STREAM_PORTAL);
            w.I32(static_cast<std::int32_t>(state->streamRows));
            w.End();

            w.Begin(FE_SYNC);
            w.End();
        }
        // Committing a transaction the caller opened would end work that is
        // none of the stream's business, so only the portal goes in that case
        else if(state->streamOwnsTx) {
            w.Begin(FE_QUERY);
            w.CStr(SQL_STREAM_COMMIT);
            w.End();
        }
        else {
            w.Begin(FE_CLOSE);
            w.U8('P');
            w.CStr(STREAM_PORTAL);
            w.End();

            w.Begin(FE_SYNC);
            w.End();
        }

        if(!w.Ok())
            return Shared::SerializeResult::BUFFER_TOO_SMALL;

        *written = w.Pos();
        return Shared::SerializeResult::OK;
    }

    state->streamRowsStale = false;

    // Only a slot taken from the pool gets cleaned. A pinned request may be
    // deliberately mid-transaction, and rolling that back would end the very
    // transaction the caller is running.
    if(!req.pinned && state->txStatus != TX_IDLE) {
        w.Begin(FE_QUERY);
        w.CStr("ROLLBACK");
        w.End();

        state->expectedReady = 2;
    }

    // Savepoints go out as a simple query, since there is nothing to bind and
    // the name has already been checked against the identifier grammar
    if(req.kind != PgStatementKind::QUERY) {
        w.Begin(FE_QUERY);

        switch(req.kind) {
            case PgStatementKind::SAVEPOINT:
                w.Str("SAVEPOINT ");
                break;
            case PgStatementKind::ROLLBACK_TO:
                w.Str("ROLLBACK TO SAVEPOINT ");
                break;
            default:
                w.Str("RELEASE SAVEPOINT ");
                break;
        }

        w.Str(req.sql);
        w.U8(0);
        w.End();

        if(!w.Ok())
            return Shared::SerializeResult::BUFFER_TOO_SMALL;

        *written = w.Pos();
        return Shared::SerializeResult::OK;
    }

    // Nothing outside a transaction holds a portal open past the Sync that ends
    // its round, so a stream that was not handed one already open starts its own
    if(state->streamRows != 0) {
        state->streamOwnsTx = !req.pinned || state->txStatus == TX_IDLE;

        if(state->streamOwnsTx) {
            w.Begin(FE_QUERY);
            w.CStr(SQL_STREAM_BEGIN);
            w.End();

            ++state->expectedReady;
        }
    }

    // A named statement keeps its plan on the server, so a request that has
    // been here before binds straight to the name and sends no Parse
    PgStatementLookup stmt;
    if(state->options->cacheControl)
        stmt = state->stmtCache.Acquire(req.sql, req.paramOids, req.paramCount, state->options->cacheControl->epoch);

    // Closing a name the server never prepared is not an error, so the flag is
    // the only guard this needs
    if(stmt.close) {
        w.Begin(FE_CLOSE);
        w.U8('S');
        w.CStr(stmt.name);
        w.End();
    }

    if(stmt.parse) {
        state->pendingStmt = stmt.index;

        w.Begin(FE_PARSE);
        w.CStr(stmt.name);
        w.CStr(req.sql);
        w.I16(static_cast<std::int16_t>(req.paramCount));
        for(std::uint16_t i = 0; i < req.paramCount; ++i)
            w.I32(static_cast<std::int32_t>(req.paramOids[i]));
        w.End();
    }

    // A one-round request leaves its portal unnamed so Sync takes it and there
    // is nothing to clean up. A stream has to name one to keep fetching from it.
    const std::string_view portal = state->streamRows != 0 ? STREAM_PORTAL : std::string_view{};

    w.Begin(FE_BIND);
    w.CStr(portal);
    w.CStr(stmt.name);
    w.I16(FORMAT_COUNT_UNIFORM);
    w.I16(FORMAT_BINARY); // every parameter is encoded binary
    w.I16(static_cast<std::int16_t>(req.paramCount));
    w.Str(req.params); // already length-prefixed per value
    w.I16(FORMAT_COUNT_UNIFORM);
    w.I16(resultFormat);
    w.End();

    // Without this the server returns rows with no description, leaving every
    // column untyped. A stream is described only here, on the round that opens
    // the portal, so the column table has to outlive the chunk it arrived with.
    w.Begin(FE_DESCRIBE);
    w.U8('P');
    w.CStr(portal);
    w.End();

    w.Begin(FE_EXECUTE);
    w.CStr(portal);
    w.I32(static_cast<std::int32_t>(state->streamRows)); // zero is no row limit
    w.End();

    w.Begin(FE_SYNC);
    w.End();

    if(!w.Ok())
        return Shared::SerializeResult::BUFFER_TOO_SMALL;

    *written = w.Pos();
    return Shared::SerializeResult::OK;
}

// -----------------------------------------------------------------------
// A schema change can leave a prepared plan producing the wrong result type,
// and a session reset on the server can drop the statements outright. Both
// land here as a SQLSTATE, and both mean every connection is holding names it
// can no longer trust, so the epoch moves and each entry re-parses on its next
// use. 0A000 also covers unrelated unsupported operations, which at worst
// costs one extra Parse per cached statement.
// -----------------------------------------------------------------------
inline void InvalidateCachedPlans(SlotState* state, std::string_view sqlState) noexcept
{
    if(!state->options->cacheControl)
        return;

    if(sqlState == SQLSTATE_FEATURE_NOT_SUPPORTED || sqlState == SQLSTATE_INVALID_STATEMENT_NAME)
        state->options->cacheControl->Invalidate();
}

// -----------------------------------------------------------------------
// Parse
//
// An ErrorResponse does not end the exchange: ReadyForQuery still follows and
// the slot stays usable. The error is recorded and the request completes, so
// the caller checks Failed() rather than losing the connection.
// -----------------------------------------------------------------------
inline Shared::ParseResult Parse(void* slotStateVoid, void* /*parseState*/, const char* buf, std::uint32_t len,
                                 std::uint32_t* consumed, void* outObj, bool isEof,
                                 std::uint64_t* /*completedKey*/) noexcept
{
    auto* state = static_cast<SlotState*>(slotStateVoid);
    auto& res = *static_cast<PgResult*>(outObj);

    // The chunk handed over last round was still borrowed while the next round
    // went out, so this is the first moment its rows can go. The columns stay:
    // only the opening round describes the portal.
    if(state->streamRowsStale) {
        res.ResetRows();
        state->streamRowsStale = false;
    }

    std::uint32_t pos = 0;
    PgMessage msg;

    while(true) {
        const FrameStatus fs = FrameMessage(buf, len, pos, msg, state->options->maxMessageBytes);

        if(fs == FrameStatus::MALFORMED) {
            *consumed = pos;
            return Shared::ParseResult::ERROR;
        }

        if(fs == FrameStatus::NEED_MORE) {
            *consumed = pos;

            // isEof forbids asking for more, the peer has stopped sending
            return isEof ? Shared::ParseResult::ERROR : Shared::ParseResult::INCOMPLETE;
        }

        switch(msg.type) {
            case BE_ROW_DESCRIPTION:
                if(!res.SetDescription({msg.payload, msg.len})) {
                    *consumed = pos;
                    return Shared::ParseResult::ERROR;
                }
                break;

            case BE_DATA_ROW:
                if(!res.AppendRow({msg.payload, msg.len})) {
                    *consumed = pos;
                    return Shared::ParseResult::ERROR;
                }
                break;

            case BE_COMMAND_COMPLETE: {
                PgFieldReader r{msg};
                res.SetCommandTag(r.CStr());

                // The portal ran out, so the next round ends the transaction
                // holding it. Already closing means this is that round's own
                // tag, which retires the stream instead. A prologue statement
                // reports one too, and expectedReady is what tells them apart.
                if(state->streamRows != 0 && state->expectedReady == 1)
                    state->streamPhase =
                        state->streamPhase == PgStreamPhase::CLOSE ? PgStreamPhase::DONE : PgStreamPhase::CLOSE;
                break;
            }

            case BE_ERROR_RESPONSE:
                res.SetError({msg.payload, msg.len});
                InvalidateCachedPlans(state, res.Error().SqlState());
                break;

            case BE_READY_FOR_QUERY: {
                PgFieldReader r{msg};
                state->txStatus = static_cast<char>(r.U8());

                // A prepended rollback produces its own ReadyForQuery, which
                // belongs to the cleanup rather than to this request
                if(state->expectedReady > 1) {
                    --state->expectedReady;

                    // The rollback runs before the statement, so anything it
                    // reported is not this request's result
                    res.Reset();
                    break;
                }

                // A Parse that went out without its ParseComplete coming back
                // failed, and the Close beside it already took the old
                // statement, so the name now holds nothing
                if(state->pendingStmt != STMT_NONE) {
                    state->stmtCache.MarkFailed(state->pendingStmt);
                    state->pendingStmt = STMT_NONE;
                }

                // A stream keeps the request in flight: the caller borrows this
                // chunk and asks for the next, until the closing round lands
                if(state->streamRows != 0) {
                    // A failed round cannot be fetched from again, so the next
                    // one closes the portal, and a failed close gives up. The
                    // chunk still goes out: its error is the only copy.
                    if(res.Failed())
                        state->streamPhase =
                            state->streamPhase == PgStreamPhase::CLOSE ? PgStreamPhase::DONE : PgStreamPhase::CLOSE;

                    if(state->streamPhase != PgStreamPhase::DONE) {
                        *consumed = pos;
                        return Shared::ParseResult::CHUNK_READY_FETCH;
                    }
                }

                state->streamRows = 0;
                state->streamPhase = PgStreamPhase::NONE;
                state->streamOwnsTx = false;

                // ReadyForQuery is the definition of a reusable slot, so the
                // connection goes back to the pool even when the query failed
                *consumed = pos;
                return Shared::ParseResult::COMPLETE_KEEP_ALIVE;
            }

            case BE_PARSE_COMPLETE:
                if(state->pendingStmt != STMT_NONE) {
                    state->stmtCache.MarkParsed(state->pendingStmt, state->options->cacheControl->epoch);
                    state->pendingStmt = STMT_NONE;
                }
                break;

            // The row limit stopped this Execute short, so the portal still has
            // rows and the next round asks for them
            case BE_PORTAL_SUSPENDED:
                if(state->streamRows != 0 && state->expectedReady == 1)
                    state->streamPhase = PgStreamPhase::FETCH;
                break;

            // A stream on a transaction it does not own closes its portal rather
            // than committing, and this is that close landing. The statement
            // cache closes names too, which the phase is what excludes.
            case BE_CLOSE_COMPLETE:
                if(state->streamRows != 0 && state->streamPhase == PgStreamPhase::CLOSE)
                    state->streamPhase = PgStreamPhase::DONE;
                break;

            // Acknowledgements and informational messages carry nothing the
            // caller needs
            case BE_BIND_COMPLETE:
            case BE_NO_DATA:
            case BE_EMPTY_QUERY_RESPONSE:
            case BE_PARAMETER_STATUS:
            case BE_NOTICE_RESPONSE:
            case BE_PARAMETER_DESCRIPTION:
                break;

            default:
                *consumed = pos;
                return Shared::ParseResult::ERROR;
        }
    }
}

} // namespace WFX::Postgres::Detail

#endif // WFX_INC_WFX_ENDPOINT_POSTGRES_WIRE_HPP
