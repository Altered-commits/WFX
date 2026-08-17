// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_ENDPOINT_POSTGRES_RESULT_HPP
#define WFX_INC_WFX_ENDPOINT_POSTGRES_RESULT_HPP

// -----------------------------------------------------------------------
// Result sets, rows and errors.
//
// A receive buffer is only valid for the parse call it arrives in, and a
// result spans several of them, so a buffered result owns the bytes it keeps.
// Three things keep that cheap:
//
//   Only field bytes are copied, never the length prefixes that were already
//   parsed into the field table.
//
//   Every field's position is recorded once, into one flat table indexed by
//   row * columns, so access is a lookup rather than a walk. A negative
//   length is SQL NULL, the same encoding the wire uses, so there is no
//   separate null bitmap.
//
//   Reset() keeps capacity, so a result recycled per slot stops allocating
//   entirely once its buffers have grown to the shape of the workload.
// -----------------------------------------------------------------------

#include "types.hpp"
#include "wfx/memory.hpp"

#include <cstdint>
#include <string_view>

namespace WFX::Postgres::Detail {

// Buffers above this are released by Reset() rather than carried forward, so
// one outsized result does not pin memory on a slot for the process lifetime
inline constexpr std::size_t RESULT_ARENA_KEEP_BYTES = 1u << 20;
inline constexpr std::size_t RESULT_FIELDS_KEEP = 64u * 1024u;

// -----------------------------------------------------------------------
// Column metadata from RowDescription. Names are offsets into the owned copy
// of that message, so one allocation holds every name and PgColumn stays
// trivially copyable.
// -----------------------------------------------------------------------
struct PgColumn {
    std::uint32_t nameOffset = 0;
    std::uint32_t nameLen = 0;
    std::uint32_t tableOid = 0;
    std::uint32_t typeOid = 0;
    std::int32_t typeModifier = 0;
    std::int16_t columnAttr = 0;
    std::int16_t typeSize = 0;
    std::int16_t format = FORMAT_TEXT;
};

// Where one field's bytes sit in the arena. len < 0 is SQL NULL.
struct PgField {
    std::uint32_t offset = 0;
    std::int32_t len = -1;
};

// -----------------------------------------------------------------------
// PgError
//
// Keeps the ErrorResponse or NoticeResponse payload verbatim and scans it on
// demand. Errors are rare, so one allocation and a scan beats splitting every
// field into its own string up front.
// -----------------------------------------------------------------------
class PgError {
public: // State
    bool IsSet() const noexcept
    {
        return !raw_.empty();
    }

    void Clear() noexcept
    {
        raw_.clear();
    }

    // The payload is a run of [1 byte code][null terminated value], ending at a zero byte
    void Assign(std::string_view payload)
    {
        raw_.assign(payload.data(), payload.size());
    }

public: // Fields
    std::string_view Field(char code) const noexcept
    {
        const char* p = raw_.data();
        const char* end = p + raw_.size();

        while(p < end && *p != '\0') {
            const char c = *p++;
            const void* nul = std::memchr(p, '\0', static_cast<std::size_t>(end - p));
            if(!nul)
                break;

            const auto n = static_cast<std::size_t>(static_cast<const char*>(nul) - p);
            if(c == code)
                return {p, n};

            p += n + 1;
        }

        return {};
    }

    std::string_view SqlState() const noexcept
    {
        return Field(ERRF_SQLSTATE);
    }

    std::string_view Message() const noexcept
    {
        return Field(ERRF_MESSAGE);
    }

    std::string_view Detail() const noexcept
    {
        return Field(ERRF_DETAIL);
    }

    std::string_view Hint() const noexcept
    {
        return Field(ERRF_HINT);
    }

    // The non-localized field is preferred, the localized one is the fallback
    // for servers that predate it
    std::string_view Severity() const noexcept
    {
        const std::string_view v = Field(ERRF_SEVERITY_NONLOCALIZED);
        return v.empty() ? Field(ERRF_SEVERITY) : v;
    }

    std::string_view ConstraintName() const noexcept
    {
        return Field(ERRF_CONSTRAINT_NAME);
    }

    std::string_view TableName() const noexcept
    {
        return Field(ERRF_TABLE_NAME);
    }

    std::string_view ColumnName() const noexcept
    {
        return Field(ERRF_COLUMN_NAME);
    }

    std::string_view SchemaName() const noexcept
    {
        return Field(ERRF_SCHEMA_NAME);
    }

    // 1-based index into the failing query, 0 when the server did not report one
    std::int32_t Position() const noexcept
    {
        return DecodeText<std::int32_t>(Field(ERRF_POSITION));
    }

private:
    WFX::String raw_;
};

class PgResult;

// -----------------------------------------------------------------------
// PgRow
//
// A cursor over one row. Cheap to copy, valid only while the result it came
// from is alive.
// -----------------------------------------------------------------------
class PgRow {
public: // Construction
    PgRow() noexcept = default;

    PgRow(const PgResult* result, std::uint32_t index) noexcept : result_(result), index_(index)
    {}

public: // Access
    std::uint16_t ColumnCount() const noexcept;
    std::int32_t IndexOf(std::string_view name) const noexcept;

    bool IsNull(std::uint16_t col) const noexcept;
    std::string_view Raw(std::uint16_t col) const noexcept;

    // Reading a NULL yields a value-initialized T, so test IsNull() first
    // wherever the difference matters
    template <typename T> T Get(std::uint16_t col) const noexcept;

    bool IsNull(std::string_view name) const noexcept
    {
        const std::int32_t i = IndexOf(name);
        return i < 0 || IsNull(static_cast<std::uint16_t>(i));
    }

    template <typename T> T Get(std::string_view name) const noexcept
    {
        const std::int32_t i = IndexOf(name);
        return i < 0 ? T{} : Get<T>(static_cast<std::uint16_t>(i));
    }

private:
    bool InRange(std::uint16_t col) const noexcept;

private:
    const PgResult* result_ = nullptr;
    std::uint32_t index_ = 0;
};

// -----------------------------------------------------------------------
// PgResult
//
// Owns the column metadata, the field bytes and the field table for one
// result. Filled by the parse callback as messages arrive.
// -----------------------------------------------------------------------
class PgResult {
public: // Shape
    std::uint32_t RowCount() const noexcept
    {
        return rowCount_;
    }

    std::uint16_t ColumnCount() const noexcept
    {
        return static_cast<std::uint16_t>(columns_.size());
    }

    bool Empty() const noexcept
    {
        return rowCount_ == 0;
    }

    // What CommandComplete reported, which for INSERT, UPDATE and DELETE is
    // rows affected rather than rows returned
    std::uint64_t AffectedRows() const noexcept
    {
        return affectedRows_;
    }

    const PgError& Error() const noexcept
    {
        return error_;
    }

    bool Failed() const noexcept
    {
        return error_.IsSet();
    }

public: // Rows
    PgRow At(std::uint32_t row) const noexcept
    {
        return PgRow{this, row};
    }

    PgRow operator[](std::uint32_t row) const noexcept
    {
        return At(row);
    }

public: // Columns
    const PgColumn& Column(std::uint16_t col) const noexcept
    {
        return columns_[col];
    }

    std::string_view ColumnName(std::uint16_t col) const noexcept
    {
        const PgColumn& c = columns_[col];
        return {descRaw_.data() + c.nameOffset, c.nameLen};
    }

    std::int32_t IndexOf(std::string_view name) const noexcept
    {
        for(std::size_t i = 0; i < columns_.size(); ++i) {
            const PgColumn& c = columns_[i];
            if(c.nameLen == name.size() && std::memcmp(descRaw_.data() + c.nameOffset, name.data(), c.nameLen) == 0)
                return static_cast<std::int32_t>(i);
        }

        return -1;
    }

    // Whether Get<T> can decode this column. Get<T> does not consult this and
    // trusts the caller instead: enums, domains and extension types are
    // assigned an OID per database, so no fixed table can recognize them, and
    // rejecting those would make them unreadable.
    template <typename T> bool Matches(std::uint16_t col) const noexcept
    {
        return col < ColumnCount() && CodecAccepts<T>(columns_[col].typeOid);
    }

    template <typename T> bool Matches(std::string_view name) const noexcept
    {
        const std::int32_t i = IndexOf(name);
        return i >= 0 && Matches<T>(static_cast<std::uint16_t>(i));
    }

public: // Field lookup, used by PgRow
    const PgField& FieldAt(std::uint32_t row, std::uint16_t col) const noexcept
    {
        return fields_[static_cast<std::size_t>(row) * columns_.size() + col];
    }

    std::string_view FieldBytes(const PgField& f) const noexcept
    {
        if(f.len <= 0)
            return {};

        return {arena_.data() + f.offset, static_cast<std::size_t>(f.len)};
    }

public: // Filling, driven by the parse callback
    // Capacity survives so a recycled result stops allocating, except where a
    // buffer has grown past the point worth carrying between queries
    void Reset() noexcept
    {
        columns_.clear();
        descRaw_.clear();
        fields_.clear();
        rowCount_ = 0;
        affectedRows_ = 0;
        error_.Clear();

        if(arena_.capacity() > RESULT_ARENA_KEEP_BYTES)
            WFX::Vector<char>{}.swap(arena_);
        else
            arena_.clear();

        if(fields_.capacity() > RESULT_FIELDS_KEEP)
            WFX::Vector<PgField>{}.swap(fields_);
    }

    // Drops the rows but keeps the columns. A streamed portal is described once,
    // on the round that opens it, so later rounds have nothing to rebuild the
    // column table from and it has to survive the chunk it described.
    void ResetRows() noexcept
    {
        arena_.clear();
        fields_.clear();
        rowCount_ = 0;
        affectedRows_ = 0;
        error_.Clear();
    }

    // Consumes a RowDescription payload. A new descriptor starts a new result,
    // so anything already accumulated is dropped.
    bool SetDescription(std::string_view payload)
    {
        columns_.clear();
        arena_.clear();
        fields_.clear();
        rowCount_ = 0;

        descRaw_.assign(payload.data(), payload.size());

        PgFieldReader r{descRaw_.data(), static_cast<std::uint32_t>(descRaw_.size())};
        const std::int16_t count = r.I16();
        if(count < 0 || !r.Ok())
            return false;

        columns_.reserve(static_cast<std::size_t>(count));

        for(std::int16_t i = 0; i < count; ++i) {
            const std::string_view name = r.CStr();

            PgColumn c;
            c.nameOffset = static_cast<std::uint32_t>(name.data() - descRaw_.data());
            c.nameLen = static_cast<std::uint32_t>(name.size());
            c.tableOid = static_cast<std::uint32_t>(r.I32());
            c.columnAttr = r.I16();
            c.typeOid = static_cast<std::uint32_t>(r.I32());
            c.typeSize = r.I16();
            c.typeModifier = r.I32();
            c.format = r.I16();

            if(!r.Ok())
                return false;

            columns_.push_back(c);
        }

        return true;
    }

    // Consumes a DataRow payload. Only field bytes reach the arena, the count
    // and the length prefixes stay behind in the field table.
    bool AppendRow(std::string_view payload)
    {
        const std::uint16_t ncols = ColumnCount();

        PgFieldReader r{payload.data(), static_cast<std::uint32_t>(payload.size())};
        const std::int16_t count = r.I16();
        if(count < 0 || static_cast<std::uint16_t>(count) != ncols || !r.Ok())
            return false;

        // No reserve() per row: it allocates the exact size asked for, which
        // turns the vector's geometric growth into one realloc per row. Plain
        // push_back doubles instead.
        for(std::uint16_t i = 0; i < ncols; ++i) {
            const std::int32_t len = r.I32();
            if(!r.Ok())
                return false;

            PgField f;
            f.len = len;

            if(len > 0) {
                const std::string_view bytes = r.Bytes(static_cast<std::uint32_t>(len));
                if(!r.Ok())
                    return false;

                f.offset = static_cast<std::uint32_t>(arena_.size());
                arena_.insert(arena_.end(), bytes.begin(), bytes.end());
            }

            fields_.push_back(f);
        }

        ++rowCount_;
        return true;
    }

    // CommandComplete carries a tag such as "INSERT 0 5" or "UPDATE 3", where
    // the affected count is always the trailing number
    void SetCommandTag(std::string_view tag) noexcept
    {
        affectedRows_ = 0;

        std::size_t i = tag.size();
        while(i > 0 && tag[i - 1] >= '0' && tag[i - 1] <= '9')
            --i;

        if(i < tag.size())
            affectedRows_ = DecodeText<std::uint64_t>(tag.substr(i));
    }

    void SetError(std::string_view payload)
    {
        error_.Assign(payload);
    }

private:
    WFX::Vector<PgColumn> columns_;
    WFX::String descRaw_; // RowDescription payload, backs every column name
    WFX::Vector<char> arena_;
    WFX::Vector<PgField> fields_; // row * ColumnCount() + col
    PgError error_;
    std::uint32_t rowCount_ = 0;
    std::uint64_t affectedRows_ = 0;
};

// -----------------------------------------------------------------------
// PgRow members that need the complete PgResult
// -----------------------------------------------------------------------
inline std::uint16_t PgRow::ColumnCount() const noexcept
{
    return result_ ? result_->ColumnCount() : std::uint16_t{0};
}

inline std::int32_t PgRow::IndexOf(std::string_view name) const noexcept
{
    return result_ ? result_->IndexOf(name) : -1;
}

// Both the row and the column have to be in range before fields_ is indexed,
// since At() will hand out a cursor for any row number
inline bool PgRow::InRange(std::uint16_t col) const noexcept
{
    return result_ && index_ < result_->RowCount() && col < result_->ColumnCount();
}

inline bool PgRow::IsNull(std::uint16_t col) const noexcept
{
    return !InRange(col) || result_->FieldAt(index_, col).len < 0;
}

inline std::string_view PgRow::Raw(std::uint16_t col) const noexcept
{
    if(!InRange(col))
        return {};

    return result_->FieldBytes(result_->FieldAt(index_, col));
}

template <typename T> inline T PgRow::Get(std::uint16_t col) const noexcept
{
    if(!InRange(col))
        return T{};

    const PgField& f = result_->FieldAt(index_, col);
    if(f.len < 0)
        return T{};

    return PgCodec<T>::Decode(result_->FieldBytes(f), result_->Column(col).typeOid);
}

} // namespace WFX::Postgres::Detail

#endif // WFX_INC_WFX_ENDPOINT_POSTGRES_RESULT_HPP
