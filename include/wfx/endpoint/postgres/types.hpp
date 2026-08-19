// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_ENDPOINT_POSTGRES_TYPES_HPP
#define WFX_INC_WFX_ENDPOINT_POSTGRES_TYPES_HPP

// -----------------------------------------------------------------------
// Postgres type OIDs and their binary wire codecs.
//
// Every codec is a specialization of PgCodec<T>, so which codec runs is
// settled at compile time. Decoders take a view aimed at the receive buffer
// and return by value, so decoding a row allocates nothing.
// -----------------------------------------------------------------------

#include "protocol.hpp"

#include <bit>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

namespace WFX::Postgres::Detail {

// -----------------------------------------------------------------------
// Type OIDs, from pg_type. Only types with a codec below and the array
// element types are listed, anything else decodes as text.
// -----------------------------------------------------------------------
inline constexpr std::uint32_t OID_BOOL = 16;          // 1 byte, 0 or 1
inline constexpr std::uint32_t OID_BYTEA = 17;         // raw bytes
inline constexpr std::uint32_t OID_CHAR = 18;          // single byte, not text
inline constexpr std::uint32_t OID_NAME = 19;          // 63 byte identifier
inline constexpr std::uint32_t OID_INT8 = 20;          // signed 64 bit
inline constexpr std::uint32_t OID_INT2 = 21;          // signed 16 bit
inline constexpr std::uint32_t OID_INT4 = 23;          // signed 32 bit
inline constexpr std::uint32_t OID_TEXT = 25;          // utf8 bytes
inline constexpr std::uint32_t OID_OID = 26;           // unsigned 32 bit
inline constexpr std::uint32_t OID_JSON = 114;         // text
inline constexpr std::uint32_t OID_XML = 142;          // text
inline constexpr std::uint32_t OID_FLOAT4 = 700;       // ieee754 single
inline constexpr std::uint32_t OID_FLOAT8 = 701;       // ieee754 double
inline constexpr std::uint32_t OID_BPCHAR = 1042;      // blank padded text
inline constexpr std::uint32_t OID_VARCHAR = 1043;     // utf8 bytes
inline constexpr std::uint32_t OID_DATE = 1082;        // int32 days from 2000-01-01
inline constexpr std::uint32_t OID_TIME = 1083;        // int64 micros from midnight
inline constexpr std::uint32_t OID_TIMESTAMP = 1114;   // int64 micros, no zone
inline constexpr std::uint32_t OID_TIMESTAMPTZ = 1184; // int64 micros, utc
inline constexpr std::uint32_t OID_INTERVAL = 1186;    // int64 micros, int32 days, int32 months
inline constexpr std::uint32_t OID_TIMETZ = 1266;      // int64 micros, int32 zone offset
inline constexpr std::uint32_t OID_NUMERIC = 1700;     // base 10000 digit array
inline constexpr std::uint32_t OID_UUID = 2950;        // 16 raw bytes
inline constexpr std::uint32_t OID_JSONB = 3802;       // version byte then text

inline constexpr std::uint32_t OID_BOOL_ARRAY = 1000;        // bool[]
inline constexpr std::uint32_t OID_BYTEA_ARRAY = 1001;       // bytea[]
inline constexpr std::uint32_t OID_INT2_ARRAY = 1005;        // smallint[]
inline constexpr std::uint32_t OID_INT4_ARRAY = 1007;        // integer[]
inline constexpr std::uint32_t OID_TEXT_ARRAY = 1009;        // text[]
inline constexpr std::uint32_t OID_VARCHAR_ARRAY = 1015;     // varchar[]
inline constexpr std::uint32_t OID_INT8_ARRAY = 1016;        // bigint[]
inline constexpr std::uint32_t OID_FLOAT4_ARRAY = 1021;      // real[]
inline constexpr std::uint32_t OID_FLOAT8_ARRAY = 1022;      // double precision[]
inline constexpr std::uint32_t OID_TIMESTAMP_ARRAY = 1115;   // timestamp[]
inline constexpr std::uint32_t OID_TIMESTAMPTZ_ARRAY = 1185; // timestamptz[]
inline constexpr std::uint32_t OID_NUMERIC_ARRAY = 1231;     // numeric[]
inline constexpr std::uint32_t OID_UUID_ARRAY = 2951;        // uuid[]
inline constexpr std::uint32_t OID_JSONB_ARRAY = 3807;       // jsonb[]

inline constexpr std::uint32_t OID_UNSPECIFIED = 0; // let the server infer from context

// Format codes used by Bind and Describe
inline constexpr std::int16_t FORMAT_TEXT = 0;
inline constexpr std::int16_t FORMAT_BINARY = 1;

// -----------------------------------------------------------------------
// Temporal representations
//
// Postgres counts from 2000-01-01, Unix from 1970-01-01. Codecs convert at
// the wire boundary so callers only ever hold Unix time.
// -----------------------------------------------------------------------
inline constexpr std::int64_t PG_EPOCH_UNIX_SECONDS = 946684800;
inline constexpr std::int64_t PG_EPOCH_UNIX_MICROS = PG_EPOCH_UNIX_SECONDS * 1000000;
inline constexpr std::int32_t PG_EPOCH_UNIX_DAYS = static_cast<std::int32_t>(PG_EPOCH_UNIX_SECONDS / 86400);

// timestamptz is UTC on the wire. Plain timestamp carries no zone and is
// returned as though it were UTC.
struct PgTimestamp {
    std::int64_t unixMicros = 0;

    std::int64_t UnixSeconds() const noexcept
    {
        return unixMicros / 1000000;
    }
};

struct PgDate {
    std::int32_t unixDays = 0;
};

struct PgTime {
    std::int64_t micros = 0; // since midnight
};

// Months and days stay separate from micros because their real length depends
// on the date they are applied to
struct PgInterval {
    std::int64_t micros = 0;
    std::int32_t days = 0;
    std::int32_t months = 0;
};

// -----------------------------------------------------------------------
// PgNumeric
//
// NUMERIC is arbitrary precision, stored as base-10000 digits, so no native
// C++ type holds it losslessly. The components stay as a view and the
// conversions are explicit, so the caller picks what to give up.
// -----------------------------------------------------------------------
inline constexpr std::uint16_t NUMERIC_SIGN_POSITIVE = 0x0000;
inline constexpr std::uint16_t NUMERIC_SIGN_NEGATIVE = 0x4000;
inline constexpr std::uint16_t NUMERIC_SIGN_NAN = 0xC000;
inline constexpr std::uint16_t NUMERIC_SIGN_PINF = 0xD000;
inline constexpr std::uint16_t NUMERIC_SIGN_NINF = 0xF000;

// Powers of 10000, spelled as literals so each is the correctly rounded double
// for that value. Scaling by one of these costs a single multiply and rounds
// once, where repeated multiplication rounds on every step.
inline constexpr std::int32_t NUMERIC_POW10K_MAX = 38;
inline constexpr double NUMERIC_POW10K[NUMERIC_POW10K_MAX + 1] = {
    1e0,   1e4,   1e8,   1e12,  1e16,  1e20,  1e24,  1e28,  1e32,  1e36,  1e40,  1e44,  1e48,
    1e52,  1e56,  1e60,  1e64,  1e68,  1e72,  1e76,  1e80,  1e84,  1e88,  1e92,  1e96,  1e100,
    1e104, 1e108, 1e112, 1e116, 1e120, 1e124, 1e128, 1e132, 1e136, 1e140, 1e144, 1e148, 1e152,
};

struct PgNumeric {
    const char* digits = nullptr; // ndigits big endian int16 values, base 10000
    std::int16_t ndigits = 0;
    std::int16_t weight = 0; // base-10000 exponent of the first digit
    std::uint16_t sign = NUMERIC_SIGN_POSITIVE;
    std::int16_t dscale = 0; // decimal digits after the point

public: // Classification
    bool IsNan() const noexcept
    {
        return sign == NUMERIC_SIGN_NAN;
    }

    bool IsInf() const noexcept
    {
        return sign == NUMERIC_SIGN_PINF || sign == NUMERIC_SIGN_NINF;
    }

    bool IsNegative() const noexcept
    {
        return sign == NUMERIC_SIGN_NEGATIVE || sign == NUMERIC_SIGN_NINF;
    }

public: // Conversion
    std::int16_t DigitAt(std::int16_t i) const noexcept
    {
        return static_cast<std::int16_t>(LoadBe16(digits + i * 2));
    }

    // Horner over base 10000, then one scaling step. Exact only while the value
    // fits a double's 53 bit mantissa, which is why it is opt-in rather than
    // the default decode.
    double ToDouble() const noexcept
    {
        if(IsNan())
            return std::numeric_limits<double>::quiet_NaN();

        if(IsInf())
            return IsNegative() ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();

        double v = 0.0;
        for(std::int16_t i = 0; i < ndigits; ++i)
            v = v * 10000.0 + static_cast<double>(DigitAt(i));

        // weight is the position of the leading digit group, so whatever the
        // digit list did not cover is trailing zero groups
        const std::int32_t shift = static_cast<std::int32_t>(weight) - ndigits + 1;

        if(shift > 0)
            v = shift <= NUMERIC_POW10K_MAX ? v * NUMERIC_POW10K[shift] : std::numeric_limits<double>::infinity();
        else if(shift < 0)
            v = -shift <= NUMERIC_POW10K_MAX ? v / NUMERIC_POW10K[-shift] : 0.0;

        return IsNegative() ? -v : v;
    }
};

// Raw byte view. Separate from string_view so a parameter binds as bytea
// rather than text, since the declared OID is what the server keys off.
struct PgBytes {
    const char* data = nullptr;
    std::uint32_t size = 0;

    std::string_view View() const noexcept
    {
        return {data, size};
    }
};

struct PgUuid {
    std::uint8_t bytes[16]{};

    std::string_view View() const noexcept
    {
        return {reinterpret_cast<const char*>(bytes), sizeof(bytes)};
    }
};

// -----------------------------------------------------------------------
// PgCodec
//
// The primary template is left undefined, so binding or reading an
// unsupported type fails at compile time naming that type.
// -----------------------------------------------------------------------
template <typename T> struct PgCodec;

// Whether a column's OID is one the codec can read. Checked once per column
// at RowDescription rather than per row.
template <typename T> inline bool CodecAccepts(std::uint32_t oid) noexcept
{
    return PgCodec<T>::Accepts(oid);
}

// vvv Booleans vvv
template <> struct PgCodec<bool> {
    static constexpr std::uint32_t OID = OID_BOOL;

public: // Wire
    static bool Accepts(std::uint32_t oid) noexcept
    {
        return oid == OID_BOOL;
    }

    static void Encode(PgWriter& w, bool v) noexcept
    {
        w.FieldU8(v ? 1 : 0);
    }

    static bool Decode(std::string_view f, std::uint32_t /*oid*/) noexcept
    {
        return !f.empty() && f[0] != 0;
    }
};

// vvv Integers vvv
template <typename T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
struct PgCodec<T> {
    // Postgres has no one byte integer, so char-sized values travel as int2
    static constexpr std::uint32_t OID = sizeof(T) <= 2 ? OID_INT2 : sizeof(T) <= 4 ? OID_INT4 : OID_INT8;

public: // Wire
    // Narrower columns widen losslessly into a wider request, so each width
    // accepts everything at or below it
    static bool Accepts(std::uint32_t oid) noexcept
    {
        if(oid == OID_INT2)
            return true;

        if(oid == OID_INT4)
            return sizeof(T) >= 4;

        // oid spans the full unsigned 32 bit range, which a signed 32 bit T
        // cannot hold
        if(oid == OID_OID)
            return sizeof(T) >= 8 || (sizeof(T) == 4 && std::is_unsigned_v<T>);

        return oid == OID_INT8 && sizeof(T) >= 8;
    }

    static void Encode(PgWriter& w, T v) noexcept
    {
        if constexpr(sizeof(T) <= 2)
            w.FieldI16(static_cast<std::int16_t>(v));
        else if constexpr(sizeof(T) <= 4)
            w.FieldI32(static_cast<std::int32_t>(v));
        else
            w.FieldI64(static_cast<std::int64_t>(v));
    }

    // Width comes from the column, not from T, so a narrower column still
    // decodes correctly into a wider T
    static T Decode(std::string_view f, std::uint32_t oid) noexcept
    {
        switch(f.size()) {
            case 2:
                return static_cast<T>(static_cast<std::int16_t>(LoadBe16(f.data())));
            case 4:
                // Sign extending an oid would turn anything above 2^31 negative
                return oid == OID_OID ? static_cast<T>(LoadBe32(f.data()))
                                      : static_cast<T>(static_cast<std::int32_t>(LoadBe32(f.data())));
            case 8:
                return static_cast<T>(static_cast<std::int64_t>(LoadBe64(f.data())));
            default:
                return T{};
        }
    }
};

// vvv Floating point vvv
template <> struct PgCodec<float> {
    static constexpr std::uint32_t OID = OID_FLOAT4;

public: // Wire
    static bool Accepts(std::uint32_t oid) noexcept
    {
        return oid == OID_FLOAT4;
    }

    static void Encode(PgWriter& w, float v) noexcept
    {
        w.I32(4);
        w.I32(static_cast<std::int32_t>(std::bit_cast<std::uint32_t>(v)));
    }

    static float Decode(std::string_view f, std::uint32_t /*oid*/) noexcept
    {
        return f.size() < 4 ? 0.0f : std::bit_cast<float>(LoadBe32(f.data()));
    }
};

template <> struct PgCodec<double> {
    static constexpr std::uint32_t OID = OID_FLOAT8;

public: // Wire
    // float4 widens losslessly
    static bool Accepts(std::uint32_t oid) noexcept
    {
        return oid == OID_FLOAT8 || oid == OID_FLOAT4;
    }

    static void Encode(PgWriter& w, double v) noexcept
    {
        w.I32(8);
        w.I64(static_cast<std::int64_t>(std::bit_cast<std::uint64_t>(v)));
    }

    static double Decode(std::string_view f, std::uint32_t /*oid*/) noexcept
    {
        if(f.size() == 4)
            return std::bit_cast<float>(LoadBe32(f.data()));

        return f.size() < 8 ? 0.0 : std::bit_cast<double>(LoadBe64(f.data()));
    }
};

// vvv Text vvv
// The view aims into the receive buffer and dies with it, so results borrow
// rather than own
template <> struct PgCodec<std::string_view> {
    static constexpr std::uint32_t OID = OID_TEXT;

public: // Wire
    static bool Accepts(std::uint32_t oid) noexcept
    {
        return oid == OID_TEXT || oid == OID_VARCHAR || oid == OID_BPCHAR || oid == OID_NAME || oid == OID_CHAR ||
               oid == OID_JSON || oid == OID_JSONB || oid == OID_XML;
    }

    static void Encode(PgWriter& w, std::string_view v) noexcept
    {
        w.Field(v.data(), static_cast<std::int32_t>(v.size()));
    }

    // jsonb is versioned, one leading byte that is currently always 1
    static std::string_view Decode(std::string_view f, std::uint32_t oid) noexcept
    {
        if(oid == OID_JSONB)
            return f.empty() ? f : f.substr(1);

        return f;
    }
};

// vvv Bytea vvv
template <> struct PgCodec<PgBytes> {
    static constexpr std::uint32_t OID = OID_BYTEA;

public: // Wire
    static bool Accepts(std::uint32_t oid) noexcept
    {
        return oid == OID_BYTEA;
    }

    static void Encode(PgWriter& w, PgBytes v) noexcept
    {
        w.Field(v.data, static_cast<std::int32_t>(v.size));
    }

    static PgBytes Decode(std::string_view f, std::uint32_t /*oid*/) noexcept
    {
        return PgBytes{f.data(), static_cast<std::uint32_t>(f.size())};
    }
};

// vvv Uuid vvv
template <> struct PgCodec<PgUuid> {
    static constexpr std::uint32_t OID = OID_UUID;

public: // Wire
    static bool Accepts(std::uint32_t oid) noexcept
    {
        return oid == OID_UUID;
    }

    static void Encode(PgWriter& w, const PgUuid& v) noexcept
    {
        w.Field(v.bytes, sizeof(v.bytes));
    }

    static PgUuid Decode(std::string_view f, std::uint32_t /*oid*/) noexcept
    {
        PgUuid u;
        if(f.size() >= sizeof(u.bytes))
            std::memcpy(u.bytes, f.data(), sizeof(u.bytes));

        return u;
    }
};

// vvv Temporal vvv
template <> struct PgCodec<PgTimestamp> {
    static constexpr std::uint32_t OID = OID_TIMESTAMPTZ;

public: // Wire
    static bool Accepts(std::uint32_t oid) noexcept
    {
        return oid == OID_TIMESTAMPTZ || oid == OID_TIMESTAMP;
    }

    static void Encode(PgWriter& w, PgTimestamp v) noexcept
    {
        w.I32(8);
        w.I64(v.unixMicros - PG_EPOCH_UNIX_MICROS);
    }

    static PgTimestamp Decode(std::string_view f, std::uint32_t /*oid*/) noexcept
    {
        if(f.size() < 8)
            return {};

        return PgTimestamp{static_cast<std::int64_t>(LoadBe64(f.data())) + PG_EPOCH_UNIX_MICROS};
    }
};

template <> struct PgCodec<PgDate> {
    static constexpr std::uint32_t OID = OID_DATE;

public: // Wire
    static bool Accepts(std::uint32_t oid) noexcept
    {
        return oid == OID_DATE;
    }

    static void Encode(PgWriter& w, PgDate v) noexcept
    {
        w.I32(4);
        w.I32(v.unixDays - PG_EPOCH_UNIX_DAYS);
    }

    static PgDate Decode(std::string_view f, std::uint32_t /*oid*/) noexcept
    {
        if(f.size() < 4)
            return {};

        return PgDate{static_cast<std::int32_t>(LoadBe32(f.data())) + PG_EPOCH_UNIX_DAYS};
    }
};

template <> struct PgCodec<PgTime> {
    static constexpr std::uint32_t OID = OID_TIME;

public: // Wire
    static bool Accepts(std::uint32_t oid) noexcept
    {
        return oid == OID_TIME;
    }

    static void Encode(PgWriter& w, PgTime v) noexcept
    {
        w.I32(8);
        w.I64(v.micros);
    }

    static PgTime Decode(std::string_view f, std::uint32_t /*oid*/) noexcept
    {
        return f.size() < 8 ? PgTime{} : PgTime{static_cast<std::int64_t>(LoadBe64(f.data()))};
    }
};

template <> struct PgCodec<PgInterval> {
    static constexpr std::uint32_t OID = OID_INTERVAL;

public: // Wire
    static bool Accepts(std::uint32_t oid) noexcept
    {
        return oid == OID_INTERVAL;
    }

    static void Encode(PgWriter& w, PgInterval v) noexcept
    {
        w.I32(16);
        w.I64(v.micros);
        w.I32(v.days);
        w.I32(v.months);
    }

    static PgInterval Decode(std::string_view f, std::uint32_t /*oid*/) noexcept
    {
        if(f.size() < 16)
            return {};

        PgInterval v;
        v.micros = static_cast<std::int64_t>(LoadBe64(f.data()));
        v.days = static_cast<std::int32_t>(LoadBe32(f.data() + 8));
        v.months = static_cast<std::int32_t>(LoadBe32(f.data() + 12));
        return v;
    }
};

// vvv Numeric vvv
// Decode only. Encoding would mean owning a bignum, so numeric parameters
// bind as text and the server parses them.
template <> struct PgCodec<PgNumeric> {
    static constexpr std::uint32_t OID = OID_NUMERIC;

public: // Wire
    static bool Accepts(std::uint32_t oid) noexcept
    {
        return oid == OID_NUMERIC;
    }

    static PgNumeric Decode(std::string_view f, std::uint32_t /*oid*/) noexcept
    {
        if(f.size() < 8)
            return {};

        PgNumeric n;
        n.ndigits = static_cast<std::int16_t>(LoadBe16(f.data()));
        n.weight = static_cast<std::int16_t>(LoadBe16(f.data() + 2));
        n.sign = LoadBe16(f.data() + 4);
        n.dscale = static_cast<std::int16_t>(LoadBe16(f.data() + 6));
        n.digits = f.data() + 8;

        // A truncated digit list would otherwise be read past the buffer
        if(n.ndigits < 0 || static_cast<std::uint32_t>(n.ndigits) * 2u + 8u > f.size())
            return {};

        return n;
    }
};

// -----------------------------------------------------------------------
// Text-format fallbacks, for columns whose OID has no binary codec and for
// every column when binary is turned off
// -----------------------------------------------------------------------
template <typename T> inline T DecodeText(std::string_view f) noexcept
{
    static_assert(std::is_arithmetic_v<T>, "DecodeText only handles arithmetic types");

    T v{};
    if(!f.empty())
        (void)std::from_chars(f.data(), f.data() + f.size(), v);

    return v;
}

inline bool DecodeTextBool(std::string_view f) noexcept
{
    return !f.empty() && (f[0] == 't' || f[0] == 'T' || f[0] == '1');
}

// -----------------------------------------------------------------------
// Array views
//
// Binary layout is ndim, a has-nulls flag and the element OID, then per
// dimension a length and lower bound, then length-prefixed elements in
// row-major order. Elements stay as views.
// -----------------------------------------------------------------------
inline constexpr std::int32_t MAX_ARRAY_DIMS = 6;

struct PgArrayView {
    const char* elements = nullptr; // first length-prefixed element
    std::uint32_t bytes = 0;
    std::int32_t ndim = 0;
    std::uint32_t elementOid = 0;
    std::int32_t dimLengths[MAX_ARRAY_DIMS]{};
    std::int32_t lowerBounds[MAX_ARRAY_DIMS]{};

    std::int32_t Count() const noexcept
    {
        if(ndim <= 0)
            return 0;

        std::int32_t n = 1;
        for(std::int32_t i = 0; i < ndim; ++i)
            n *= dimLengths[i];

        return n;
    }
};

// Steps one element. Returns false at the end or on a truncated element.
// isNull comes from the -1 length Postgres uses inside arrays too.
inline bool NextArrayElement(const char*& p, const char* end, std::string_view& out, bool& isNull) noexcept
{
    if(end - p < 4)
        return false;

    const auto len = static_cast<std::int32_t>(LoadBe32(p));
    p += 4;

    if(len < 0) {
        isNull = true;
        out = {};
        return true;
    }

    if(end - p < len)
        return false;

    isNull = false;
    out = std::string_view{p, static_cast<std::size_t>(len)};
    p += len;
    return true;
}

inline bool DecodeArray(std::string_view f, PgArrayView& out) noexcept
{
    if(f.size() < 12)
        return false;

    out.ndim = static_cast<std::int32_t>(LoadBe32(f.data()));
    out.elementOid = LoadBe32(f.data() + 8);

    if(out.ndim < 0 || out.ndim > MAX_ARRAY_DIMS)
        return false;

    // An empty array carries no dimension headers
    if(out.ndim == 0) {
        out.elements = f.data() + 12;
        out.bytes = 0;
        return true;
    }

    const std::uint32_t headerBytes = 12 + static_cast<std::uint32_t>(out.ndim) * 8u;
    if(f.size() < headerBytes)
        return false;

    for(std::int32_t i = 0; i < out.ndim; ++i) {
        out.dimLengths[i] = static_cast<std::int32_t>(LoadBe32(f.data() + 12 + i * 8));
        out.lowerBounds[i] = static_cast<std::int32_t>(LoadBe32(f.data() + 16 + i * 8));

        if(out.dimLengths[i] < 0)
            return false;
    }

    out.elements = f.data() + headerBytes;
    out.bytes = static_cast<std::uint32_t>(f.size()) - headerBytes;
    return true;
}

// -----------------------------------------------------------------------
// Parameter binding
//
// Binding PgNull writes SQL NULL, which is how a caller expresses one
// without an optional wrapper around every parameter.
// -----------------------------------------------------------------------
struct PgNull {};

// String literals, const char* and any owning string all bind as text, so they
// route to the string_view codec rather than needing a specialization each
template <typename T>
using PgParamCodec =
    PgCodec<std::conditional_t<std::is_convertible_v<T, std::string_view>, std::string_view, std::decay_t<T>>>;

template <typename T> inline constexpr std::uint32_t ParamOid() noexcept
{
    if constexpr(std::is_same_v<std::decay_t<T>, PgNull>)
        return OID_UNSPECIFIED;
    else
        return PgParamCodec<T>::Oid;
}

template <typename T> inline void EncodeParam(PgWriter& w, const T& v) noexcept
{
    if constexpr(std::is_same_v<std::decay_t<T>, PgNull>)
        w.FieldNull();
    else
        PgParamCodec<T>::Encode(w, v);
}

} // namespace WFX::Postgres::Detail

#endif // WFX_INC_WFX_ENDPOINT_POSTGRES_TYPES_HPP
