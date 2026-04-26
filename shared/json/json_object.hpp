#ifndef WFX_SHARED_JSON_OBJECT_HPP
#define WFX_SHARED_JSON_OBJECT_HPP

#include "core/core.hpp"
#include "http/response.hpp"
#include "shared/abis/uuid.hpp"
#include "shared/utils/hash.hpp"
#include <string_view>
#include <cstdint>
#include <cstring>
#include <charconv>
#include <bit>

namespace WFX::Shared {

static constexpr std::uint32_t JSON_NIL      = 0xFFFFFFFF;
static constexpr std::uint32_t JSON_TOMB     = 0xFFFFFFFE;
static constexpr std::uint32_t JSON_INIT_CAP = 8;

inline void* Alloc(std::size_t n)            noexcept { return WFX::Core::MemoryApi()->Alloc(n); }
inline void  Free(void* ptr)                 noexcept { WFX::Core::MemoryApi()->Free(ptr); }
inline void* Realloc(void* p, std::size_t n) noexcept { return WFX::Core::MemoryApi()->Realloc(p, n); }

// vvv JsonTag vvv
//
// EMPTY    : null
// BOOL     : u64a = 0/1
// INT64    : u64a = int64  (bit-cast)
// UINT64   : u64a = uint64 (bit-cast)
// DOUBLE   : u64a = double (bit-cast)
// STR_VIEW : u64a = const char* (packed pointer), u32c = byte length
//            Zero-copy. Caller owns memory, must outlive node.
// STR_OWN  : u32a = offset into JsonStore::strs, u32c = byte length
//            Copied into store on assignment.
//
// ARRAY node layout:
//   u32a() = element count  (live)
//   u32b() = element cap
//   u64b   = uint32_t* pointing to elems[cap]
//            [ elem0_nodeIdx | elem1_nodeIdx | ... ]
//
// OBJECT node layout:
//   u32a() = JsonKV count  (live, i.e. non-erased keys)
//   u32b() = kvCap     (allocated slots in kvList region)
//   u64b   = uint32_t* pointing to combined buffer:
//            [ htCap (u32) | _pad (u32) | kvList[kvCap] | htSlots[htCap] ]
//            htCap is stored at buf[0] so we can read it without a separate field.
//            kvList  : JsonKV indices in insertion order
//            htSlots : open-addressing HT, JSON_NIL=empty, JSON_TOMB=deleted, else=JsonKV index
//
enum class JsonTag : std::uint8_t {
    EMPTY    = 0,
    BOOL     = 1,
    INT64    = 2,
    UINT64   = 3,
    DOUBLE   = 4,
    STR_VIEW = 5,
    STR_OWN  = 6,
    ARRAY    = 7,
    OBJECT   = 8,
};

// vvv JsonNode (24 bytes, standard layout, trivially copyable) vvv
struct JsonNode {
    JsonTag      tag     = JsonTag::EMPTY;
    std::uint8_t _Pad[7] = {};
    std::uint64_t u64a   = 0;
    std::uint64_t u64b   = 0;

public: // vvv uint32 aliases into u64a and u64b vvv
    std::uint32_t& u32a() noexcept { return reinterpret_cast<std::uint32_t*>(&u64a)[0]; }
    std::uint32_t& u32b() noexcept { return reinterpret_cast<std::uint32_t*>(&u64a)[1]; }
    std::uint32_t& u32c() noexcept { return reinterpret_cast<std::uint32_t*>(&u64b)[0]; }
    std::uint32_t& u32d() noexcept { return reinterpret_cast<std::uint32_t*>(&u64b)[1]; }

    std::uint32_t u32a() const noexcept { return reinterpret_cast<const std::uint32_t*>(&u64a)[0]; }
    std::uint32_t u32b() const noexcept { return reinterpret_cast<const std::uint32_t*>(&u64a)[1]; }
    std::uint32_t u32c() const noexcept { return reinterpret_cast<const std::uint32_t*>(&u64b)[0]; }
    std::uint32_t u32d() const noexcept { return reinterpret_cast<const std::uint32_t*>(&u64b)[1]; }

    // Buffer pointer helpers (ARRAY and OBJECT)
    // Raw pointer stored in u64b via memcpy to avoid strict-aliasing UB
    template<typename T> T*       BufAs()       noexcept { T* p; std::memcpy(&p, &u64b, 8); return p; }
    template<typename T> const T* BufAs() const noexcept { const T* p; std::memcpy(&p, &u64b, 8); return p; }
    template<typename T> void     BufSet(T* p)  noexcept { std::memcpy(&u64b, &p, 8); }
};

static_assert(sizeof(JsonNode) == 24,                 "'JsonNode' must be exactly 24 bytes");
static_assert(std::is_standard_layout_v<JsonNode>,    "'JsonNode' must be standard layout");
static_assert(std::is_trivially_copyable_v<JsonNode>, "'JsonNode' must be trivially copyable");
static_assert(offsetof(JsonNode, tag)  == 0,          "JsonNode::tag offset changed");
static_assert(offsetof(JsonNode, u64a) == 8,          "JsonNode::u64a offset changed");
static_assert(offsetof(JsonNode, u64b) == 16,         "JsonNode::u64b offset changed");

//
// vvv JsonKV  (16 bytes, standard layout, trivially copyable) vvv
// Keys always copied into JsonStore::strs
// valIdx = JSON_NIL means erased (on free-list)
// _Pad reused as free-list next pointer when valIdx == JSON_NIL
//
struct JsonKV {
    std::uint32_t keyOff = JSON_NIL;
    std::uint32_t keyLen = 0;
    std::uint32_t valIdx = JSON_NIL;
    std::uint32_t _Pad   = 0;
};

static_assert(sizeof(JsonKV) == 16,                 "'JsonKV' must be exactly 16 bytes");
static_assert(std::is_standard_layout_v<JsonKV>,    "'JsonKV' must be standard layout");
static_assert(std::is_trivially_copyable_v<JsonKV>, "'JsonKV' must be trivially copyable");

// vvv Object buffer layout helpers vvv
//
//   buf[0]                        = htCap   (uint32)
//   buf[1]                        = _pad    (uint32)
//   buf[2..2+kvCap-1]             = kvList  (JsonKV indices, insertion order)
//   buf[2+kvCap..2+kvCap+htCap-1] = htSlots (open-addressing HT)
//
// Accessed via free functions so JsonNode methods stay thin
//
inline std::uint32_t        ObjHtCap (const std::uint32_t* buf)                      noexcept { return buf[0]; }
inline std::uint32_t*       ObjKVList(std::uint32_t* buf)                            noexcept { return buf + 2; }
inline std::uint32_t*       ObjHTSlot(std::uint32_t* buf, std::uint32_t kvCap)       noexcept { return buf + 2 + kvCap; }
inline const std::uint32_t* ObjKVList(const std::uint32_t* buf)                      noexcept { return buf + 2; }
inline const std::uint32_t* ObjHTSlot(const std::uint32_t* buf, std::uint32_t kvCap) noexcept { return buf + 2 + kvCap; }

// Total uint32 slots needed for an object buffer
inline std::uint32_t ObjBufSize(std::uint32_t kvCap, std::uint32_t htCap) noexcept { return 2 + kvCap + htCap; }

// vvv Main storage unit vvv
struct JsonStore {
    JsonNode*         nodes   = nullptr;
    std::uint32_t nodeCap = 0;
    std::uint32_t nodeLen = 0;

    JsonKV*           kvs     = nullptr;
    std::uint32_t kvCap   = 0;
    std::uint32_t kvLen   = 0;
    std::uint32_t kvFree  = JSON_NIL; // free-list head (next via JsonKV::_Pad)

    char*         strs    = nullptr;
    std::uint32_t strCap  = 0;
    std::uint32_t strLen  = 0;

public:
    bool Reserve(std::uint32_t nodeHint, std::uint32_t kvHint, std::uint32_t strHint) noexcept
    {
        if(nodeHint > nodeCap) {
            auto* np = static_cast<JsonNode*>(Realloc(nodes, nodeHint * sizeof(JsonNode)));
            if(!np)
                return false;

            nodes   = np;
            nodeCap = nodeHint;
        }

        if(kvHint > kvCap) {
            auto* np = static_cast<JsonKV*>(Realloc(kvs, kvHint * sizeof(JsonKV)));
            if(!np)
                return false;

            kvs   = np;
            kvCap = kvHint;
        }

        if(strHint > strCap) {
            auto* np = static_cast<char*>(Realloc(strs, strHint));
            if(!np)
                return false;

            strs   = np;
            strCap = strHint;
        }

        return true;
    }

    std::uint32_t AllocNode() noexcept
    {
        if(nodeLen == nodeCap) {
            std::uint32_t nc = nodeCap ? nodeCap * 2 : JSON_INIT_CAP;
            auto* np = static_cast<JsonNode*>(nodes ? Realloc(nodes, nc * sizeof(JsonNode)) : Alloc(nc * sizeof(JsonNode)));
            if(!np)
                return JSON_NIL;

            nodes   = np;
            nodeCap = nc;
        }

        std::uint32_t idx = nodeLen++;
        new(&nodes[idx]) JsonNode();
        return idx;
    }

    std::uint32_t AllocKV() noexcept
    {
        if(kvFree != JSON_NIL) {
            std::uint32_t idx = kvFree;
            kvFree = kvs[idx]._Pad;
            new(&kvs[idx]) JsonKV();
            return idx;
        }

        if(kvLen == kvCap) {
            std::uint32_t kc = kvCap ? kvCap * 2 : JSON_INIT_CAP;
            auto* np = static_cast<JsonKV*>(kvs ? Realloc(kvs, kc * sizeof(JsonKV)) : Alloc(kc * sizeof(JsonKV)));
            if(!np)
                return JSON_NIL;

            kvs   = np;
            kvCap = kc;
        }

        std::uint32_t idx = kvLen++;
        new(&kvs[idx]) JsonKV();
        return idx;
    }

    void FreeKV(std::uint32_t idx) noexcept
    {
        kvs[idx].valIdx = JSON_NIL;
        kvs[idx]._Pad   = kvFree;
        kvFree = idx;
    }

    std::uint32_t AllocStr(const char* data, std::uint32_t len) noexcept
    {
        std::uint32_t need = strLen + len + 1;
        if(need > strCap) {
            std::uint32_t sc = strCap ? strCap * 2 : 64;
            while(sc < need)
                sc *= 2;

            auto* np = static_cast<char*>(strs ? Realloc(strs, sc) : Alloc(sc));
            if(!np)
                return JSON_NIL;

            strs   = np;
            strCap = sc;
        }

        std::uint32_t off = strLen;
        if(data)
            std::memcpy(strs + off, data, len);

        strs[off + len] = '\0';
        strLen += len + 1;
        return off;
    }

    // Walk all nodes and free their associated buffers
    // ARRAY : u64b = uint32_t* (element index array)  |
    // OBJECT: u64b = uint32_t* (combined kvList+HT)   | -> Free directly
    void FreeNodeData() noexcept
    {
        for(std::uint32_t i = 0; i < nodeLen; ++i) {
            JsonNode& n = nodes[i];
            if(n.tag == JsonTag::ARRAY || n.tag == JsonTag::OBJECT) {
                auto* buf = n.BufAs<std::uint32_t>();
                if(buf) {
                    Free(buf);
                    n.BufSet<std::uint32_t>(nullptr);
                }
            }
        }
    }
};

static_assert(std::is_standard_layout_v<JsonStore>,  "'JsonStore' must be standard layout");
static_assert(sizeof(JsonStore) == 56,               "'JsonStore' must be 56 bytes");
static_assert(offsetof(JsonStore, nodes)   == 0,     "JsonStore::nodes offset changed");
static_assert(offsetof(JsonStore, nodeCap) == 8,     "JsonStore::nodeCap offset changed");
static_assert(offsetof(JsonStore, nodeLen) == 12,    "JsonStore::nodeLen offset changed");
static_assert(offsetof(JsonStore, kvs)     == 16,    "JsonStore::kvs offset changed");
static_assert(offsetof(JsonStore, kvCap)   == 24,    "JsonStore::kvCap offset changed");
static_assert(offsetof(JsonStore, kvLen)   == 28,    "JsonStore::kvLen offset changed");
static_assert(offsetof(JsonStore, kvFree)  == 32,    "JsonStore::kvFree offset changed");
static_assert(offsetof(JsonStore, strs)    == 40,    "JsonStore::strs offset changed");

// vvv JsonRef (stack only proxy into a JsonStore, never crosses ABI boundaries) vvv
//
class JsonObject;
class JsonParser;

class JsonRef {
public:
    JsonRef(JsonStore* s, std::uint32_t idx) noexcept : s_(s), idx_(idx) {}

public:
    bool Valid()    const noexcept { return s_ && idx_ != JSON_NIL; }
    bool IsNull()   const noexcept { return !Valid() || N().tag == JsonTag::EMPTY; }
    bool IsBool()   const noexcept { return Valid() && N().tag == JsonTag::BOOL; }
    bool IsInt()    const noexcept { return Valid() && N().tag == JsonTag::INT64; }
    bool IsUInt()   const noexcept { return Valid() && N().tag == JsonTag::UINT64; }
    bool IsDouble() const noexcept { return Valid() && N().tag == JsonTag::DOUBLE; }
    bool IsString() const noexcept { return Valid() && (N().tag == JsonTag::STR_VIEW || N().tag == JsonTag::STR_OWN); }
    bool IsArray()  const noexcept { return Valid() && N().tag == JsonTag::ARRAY; }
    bool IsObject() const noexcept { return Valid() && N().tag == JsonTag::OBJECT; }

public:
    bool AsBool() const noexcept { return Valid() && N().u64a != 0; }

    std::int64_t AsInt() const noexcept
    {
        if(!Valid()) return 0;
        std::int64_t v; std::memcpy(&v, &N().u64a, 8); return v;
    }

    std::uint64_t AsUInt() const noexcept
    {
        if(!Valid()) return 0;
        std::uint64_t v; std::memcpy(&v, &N().u64a, 8); return v;
    }

    double AsDouble() const noexcept
    {
        if(!Valid()) return 0.0;
        double v; std::memcpy(&v, &N().u64a, 8); return v;
    }

    std::string_view AsString() const noexcept
    {
        if(!Valid())
            return {};

        const auto& n = N();
        if(n.tag == JsonTag::STR_VIEW) {
            const char* ptr; std::memcpy(&ptr, &n.u64a, 8);
            return {ptr, n.u32c()};
        }

        if(n.tag == JsonTag::STR_OWN)
            return {s_->strs + n.u32a(), n.u32c()};

        return {};
    }

    // For ARRAY: element count, For OBJECT: key count
    std::uint32_t Length() const noexcept
    {
        if(!Valid()) return 0;
        return N().u32a(); // both ARRAY and OBJECT store live count in u32a
    }

public:
    // O(1) array index access
    JsonRef operator[](std::uint32_t i) noexcept
    {
        if(!Valid() || N().tag != JsonTag::ARRAY || i >= N().u32a())
            return Dead();

        auto* buf = N().BufAs<std::uint32_t>();
        if(!buf)
            return Dead();

        return JsonRef{s_, buf[i]};
    }

    // Object key access, creates key if missing
    JsonRef operator[](std::string_view key) noexcept
    {
        return GetOrCreate(key.data(), static_cast<std::uint32_t>(key.size()));
    }

    // Read only object key lookup
    JsonRef Get(std::string_view key) const noexcept
    {
        return Find(key.data(), static_cast<std::uint32_t>(key.size()));
    }

public:
    // Set with owned key (key copied)
    JsonRef Set(std::string_view key, std::string_view v)    noexcept { auto r = GetOrCreate(key.data(), static_cast<std::uint32_t>(key.size())); if(r.Valid()) r = v;       return r; }
    JsonRef Set(std::string_view key, const char* v)         noexcept { auto r = GetOrCreate(key.data(), static_cast<std::uint32_t>(key.size())); if(r.Valid()) r = v;       return r; }
    JsonRef Set(std::string_view key, std::int64_t v)        noexcept { auto r = GetOrCreate(key.data(), static_cast<std::uint32_t>(key.size())); if(r.Valid()) r = v;       return r; }
    JsonRef Set(std::string_view key, std::uint64_t v)       noexcept { auto r = GetOrCreate(key.data(), static_cast<std::uint32_t>(key.size())); if(r.Valid()) r = v;       return r; }
    JsonRef Set(std::string_view key, double v)              noexcept { auto r = GetOrCreate(key.data(), static_cast<std::uint32_t>(key.size())); if(r.Valid()) r = v;       return r; }
    JsonRef Set(std::string_view key, bool v)                noexcept { auto r = GetOrCreate(key.data(), static_cast<std::uint32_t>(key.size())); if(r.Valid()) r = v;       return r; }
    JsonRef Set(std::string_view key, std::nullptr_t)        noexcept { auto r = GetOrCreate(key.data(), static_cast<std::uint32_t>(key.size())); if(r.Valid()) r = nullptr; return r; }
    JsonRef Set(std::string_view key, const Shared::UUID& v) noexcept { auto r = GetOrCreate(key.data(), static_cast<std::uint32_t>(key.size())); if(r.Valid()) r = v;       return r; }

    JsonRef Set(std::string_view key, std::int32_t v)  noexcept { return Set(key, static_cast<std::int64_t>(v));  }
    JsonRef Set(std::string_view key, std::uint32_t v) noexcept { return Set(key, static_cast<std::uint64_t>(v)); }
    JsonRef Set(std::string_view key, std::int16_t v)  noexcept { return Set(key, static_cast<std::int64_t>(v));  }
    JsonRef Set(std::string_view key, std::uint16_t v) noexcept { return Set(key, static_cast<std::uint64_t>(v)); }
    JsonRef Set(std::string_view key, std::int8_t v)   noexcept { return Set(key, static_cast<std::int64_t>(v));  }
    JsonRef Set(std::string_view key, std::uint8_t v)  noexcept { return Set(key, static_cast<std::uint64_t>(v)); }
    JsonRef Set(std::string_view key, float v)         noexcept { return Set(key, static_cast<double>(v));        }

    // Set with JsonRef value, aliases by node index, no node copy, no new allocation
    JsonRef Set(std::string_view key, JsonRef v) noexcept
    {
        if(!Valid() || !v.Valid())
            return Dead();

        const char*   kptr = key.data();
        std::uint32_t klen = static_cast<std::uint32_t>(key.size());

        auto& n = NMut();
        if(n.tag == JsonTag::EMPTY) { n.tag = JsonTag::OBJECT; n.u64a = 0; n.u64b = 0; }
        if(n.tag != JsonTag::OBJECT) return Dead();

        // Key exists, redirect valIdx
        std::uint32_t existing = HTFind(kptr, klen);
        if(existing != JSON_NIL) {
            s_->kvs[existing].valIdx = v.idx_;
            return JsonRef{s_, v.idx_};
        }

        // New key, allocate JsonKV + copy key string, then insert
        std::uint32_t keyOff = s_->AllocStr(kptr, klen);
        if(keyOff == JSON_NIL) return Dead();

        std::uint32_t kvIdx = s_->AllocKV();
        if(kvIdx == JSON_NIL) return Dead();

        // Re fetch node after potential reallocs from AllocStr / AllocKV
        auto& n2 = NMut();
        JsonKV& kv    = s_->kvs[kvIdx];
        kv.keyOff = keyOff;
        kv.keyLen = klen;
        kv.valIdx = v.idx_;

        if(!ObjAppendKV(n2, kvIdx, klen))
            return Dead();

        return JsonRef{s_, v.idx_};
    }

public:
    bool Erase(std::string_view key) noexcept
    {
        if(!Valid() || N().tag != JsonTag::OBJECT) return false;
        return EraseKey(key.data(), static_cast<std::uint32_t>(key.size()));
    }

    bool Erase(std::uint32_t i) noexcept
    {
        if(!Valid() || N().tag != JsonTag::ARRAY) return false;
        return EraseIndex(i);
    }

public:
    // Array push, returns JsonRef to new (EMPTY) element node
    JsonRef PushBack() noexcept
    {
        if(!Valid())
            return Dead();

        auto& n = NMut();
        if(n.tag == JsonTag::EMPTY) { n.tag = JsonTag::ARRAY; n.u64a = 0; n.u64b = 0; }
        if(n.tag != JsonTag::ARRAY) return Dead();

        std::uint32_t valIdx = s_->AllocNode();
        if(valIdx == JSON_NIL)
            return Dead();

        auto& n2 = NMut(); // re-fetch after potential realloc
        if(!ArrAppend(n2, valIdx))
            return Dead();

        return JsonRef{s_, valIdx};
    }

    void PushBack(std::nullptr_t v)      noexcept { auto r = PushBack(); if(r.Valid()) r = v; }
    void PushBack(bool v)                noexcept { auto r = PushBack(); if(r.Valid()) r = v; }
    void PushBack(std::int64_t v)        noexcept { auto r = PushBack(); if(r.Valid()) r = v; }
    void PushBack(std::uint64_t v)       noexcept { auto r = PushBack(); if(r.Valid()) r = v; }
    void PushBack(double v)              noexcept { auto r = PushBack(); if(r.Valid()) r = v; }
    void PushBack(const char* v)         noexcept { auto r = PushBack(); if(r.Valid()) r = v; }
    void PushBack(std::string_view v)    noexcept { auto r = PushBack(); if(r.Valid()) r = v; }
    void PushBack(const Shared::UUID& v) noexcept { auto r = PushBack(); if(r.Valid()) r = v; }
    void PushBack(std::int32_t v)        noexcept { PushBack(static_cast<std::int64_t>(v)); }
    void PushBack(std::uint32_t v)       noexcept { PushBack(static_cast<std::uint64_t>(v)); }
    void PushBack(float v)               noexcept { PushBack(static_cast<double>(v)); }

public:
    JsonRef& operator=(std::nullptr_t) noexcept
    {
        if(Valid()) NMut().tag = JsonTag::EMPTY;
        return *this;
    }

    JsonRef& operator=(bool v) noexcept
    {
        if(!Valid()) return *this;
        auto& n = NMut(); n.tag = JsonTag::BOOL; n.u64a = v ? 1u : 0u;
        return *this;
    }

    JsonRef& operator=(std::int64_t v) noexcept
    {
        if(!Valid()) return *this;
        auto& n = NMut(); n.tag = JsonTag::INT64; std::memcpy(&n.u64a, &v, 8);
        return *this;
    }

    JsonRef& operator=(std::uint64_t v) noexcept
    {
        if(!Valid()) return *this;
        auto& n = NMut(); n.tag = JsonTag::UINT64; std::memcpy(&n.u64a, &v, 8);
        return *this;
    }

    JsonRef& operator=(double v) noexcept
    {
        if(!Valid()) return *this;
        auto& n = NMut(); n.tag = JsonTag::DOUBLE; std::memcpy(&n.u64a, &v, 8);
        return *this;
    }

    // Zero-copy string view, pointer packed into u64a, length in u32c
    JsonRef& operator=(const char* v) noexcept
    {
        if(!Valid() || !v)
            return *this;

        auto& n = NMut();
        n.tag   = JsonTag::STR_VIEW;
        n.u64b  = 0;

        std::memcpy(&n.u64a, &v, 8);
        n.u32c() = static_cast<std::uint32_t>(std::strlen(v));
        return *this;
    }

    // Owned string, copied into str region
    JsonRef& operator=(std::string_view v) noexcept
    {
        if(!Valid())
            return *this;

        std::uint32_t off = s_->AllocStr(v.data(), static_cast<std::uint32_t>(v.size()));
        if(off == JSON_NIL)
            return *this;

        auto& n = NMut();
        n.tag   = JsonTag::STR_OWN;
        n.u64b  = 0;

        n.u32a() = off;
        n.u32c() = static_cast<std::uint32_t>(v.size());
        return *this;
    }

    JsonRef& operator=(const Shared::UUID& v) noexcept
    {
        auto s = v.ToString();
        return *this = std::string_view{s.data, 36};
    }

    JsonRef& operator=(std::int32_t v)  noexcept { return *this = static_cast<std::int64_t>(v); }
    JsonRef& operator=(std::uint32_t v) noexcept { return *this = static_cast<std::uint64_t>(v); }
    JsonRef& operator=(std::int16_t v)  noexcept { return *this = static_cast<std::int64_t>(v); }
    JsonRef& operator=(std::uint16_t v) noexcept { return *this = static_cast<std::uint64_t>(v); }
    JsonRef& operator=(std::int8_t v)   noexcept { return *this = static_cast<std::int64_t>(v); }
    JsonRef& operator=(std::uint8_t v)  noexcept { return *this = static_cast<std::uint64_t>(v); }
    JsonRef& operator=(float v)         noexcept { return *this = static_cast<double>(v); }

    JsonNode&         NMut()          noexcept { return s_->nodes[idx_]; }
    std::uint32_t NodeIdx() const noexcept { return idx_; }

private:
    JsonRef         Dead() const noexcept { return JsonRef{nullptr, JSON_NIL}; }
    const JsonNode& N()    const noexcept { return s_->nodes[idx_]; }

    // HT lookup, returns JsonKV index or JSON_NIL
    std::uint32_t HTFind(const char* key, std::uint32_t klen) const noexcept
    {
        const JsonNode& n = N();
        if(n.tag != JsonTag::OBJECT)
            return JSON_NIL;

        const auto* buf = n.BufAs<std::uint32_t>();
        if(!buf)
            return JSON_NIL;

        std::uint32_t htCap = ObjHtCap(buf);
        if(htCap == 0)
            return JSON_NIL;

        std::uint32_t kvCap = n.u32b();
        const std::uint32_t* slots = ObjHTSlot(buf, kvCap);
        std::uint32_t h = static_cast<std::uint32_t>(Shared::Hasher::WyHash(key, klen)) & (htCap - 1);

        while(slots[h] != JSON_NIL) {
            if(slots[h] != JSON_TOMB) {
                const JsonKV& kv = s_->kvs[slots[h]];
                if(kv.keyLen == klen && std::memcmp(s_->strs + kv.keyOff, key, klen) == 0)
                    return slots[h];
            }

            h = (h + 1) & (htCap - 1);
        }
        return JSON_NIL;
    }

    // Rebuild HT in-place from kvList
    // Reallocates buf if htCap changes (kvCap unchanged)
    // Returns new buf pointer (may differ if realloc moved it), or null on OOM
    // JsonNode's u64b is updated on success
    std::uint32_t* RebuildHT(JsonNode& n, std::uint32_t newHtCap) noexcept
    {
        std::uint32_t  kvCap  = n.u32b();
        std::uint32_t  count  = n.u32a();
        auto*          oldBuf = n.BufAs<std::uint32_t>();

        // Safe-copy kvList before any realloc
        std::uint32_t* tmp = nullptr;
        if(count > 0) {
            tmp = static_cast<std::uint32_t*>(Alloc(count * sizeof(std::uint32_t)));
            if(!tmp)
                return nullptr;

            std::memcpy(tmp, ObjKVList(oldBuf), count * sizeof(std::uint32_t));
        }

        std::uint32_t total = ObjBufSize(kvCap, newHtCap);
        auto* newBuf = static_cast<std::uint32_t*>(
            oldBuf ? Realloc(oldBuf, total * sizeof(std::uint32_t)) : Alloc(total * sizeof(std::uint32_t))
        );

        if(!newBuf) {
            if(tmp)
                Free(tmp);

            return nullptr;
        }

        // Write new htCap at buf[0]
        newBuf[0] = newHtCap;
        newBuf[1] = 0; // pad

        // Restore kvList (may have moved)
        if(tmp) {
            std::memcpy(ObjKVList(newBuf), tmp, count * sizeof(std::uint32_t));
            Free(tmp);
        }

        // Zero the HT slots region
        std::uint32_t* slots = ObjHTSlot(newBuf, kvCap);
        std::memset(slots, 0xFF, newHtCap * sizeof(std::uint32_t));

        // Re-insert all live KVs
        for(std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t ki  = ObjKVList(newBuf)[i];
            const JsonKV&     kv  = s_->kvs[ki];
            const char*   key = s_->strs + kv.keyOff;
            std::uint32_t h   = static_cast<std::uint32_t>(Shared::Hasher::WyHash(key, kv.keyLen)) & (newHtCap - 1);

            while(slots[h] != JSON_NIL && slots[h] != JSON_TOMB)
                h = (h + 1) & (newHtCap - 1);

            slots[h] = ki;
        }

        n.BufSet(newBuf);
        return newBuf;
    }

    // Grow kvList region (kvCap doubles), slide HT region right
    // Returns new buf pointer or null on OOM. Updates node
    std::uint32_t* GrowKVCap(JsonNode& n) noexcept
    {
        std::uint32_t  kvCap    = n.u32b();
        std::uint32_t  newKvCap = kvCap ? kvCap * 2 : JSON_INIT_CAP;
        auto*          oldBuf   = n.BufAs<std::uint32_t>();
        std::uint32_t  htCap    = oldBuf ? ObjHtCap(oldBuf) : 0;
        std::uint32_t  count    = n.u32a();

        std::uint32_t total = ObjBufSize(newKvCap, htCap);
        auto* newBuf = static_cast<std::uint32_t*>(
            oldBuf ? Realloc(oldBuf, total * sizeof(std::uint32_t)) : Alloc(total * sizeof(std::uint32_t))
        );

        if(!newBuf)
            return nullptr;

        // Slide HT region right to make room for bigger kvList
        if(htCap && newKvCap != kvCap)
            std::memmove(
                ObjHTSlot(newBuf, newKvCap),
                ObjHTSlot(newBuf, kvCap),
                htCap * sizeof(std::uint32_t)
            );

        newBuf[0] = htCap;
        newBuf[1] = 0;

        n.u32b()  = newKvCap;
        n.BufSet(newBuf);
        (void)count;

        return newBuf;
    }

    // Insert kvIdx into HT. Builds or rebuilds HT when load >= 50%
    bool HTInsert(JsonNode& n, std::uint32_t* buf, std::uint32_t kvIdx, std::uint32_t klen) noexcept
    {
        std::uint32_t count = n.u32a();
        std::uint32_t htCap = buf ? ObjHtCap(buf) : 0;

        // Build / rebuild when htCap is 0 or load would exceed 50%
        if(htCap == 0 || count > htCap / 2) {
            std::uint32_t newHtCap = std::bit_ceil(count * 2u);
            if(newHtCap < 8)
                newHtCap = 8;

            // RebuildHT updates n.u64b internally
            return RebuildHT(n, newHtCap) != nullptr;
        }

        // Direct insert into existing HT
        const JsonKV&     kv    = s_->kvs[kvIdx];
        const char*   key   = s_->strs + kv.keyOff;
        std::uint32_t kvCap = n.u32b();
        std::uint32_t* slots = ObjHTSlot(buf, kvCap);
        std::uint32_t h = static_cast<std::uint32_t>(Shared::Hasher::WyHash(key, klen)) & (htCap - 1);

        while(slots[h] != JSON_NIL && slots[h] != JSON_TOMB)
            h = (h + 1) & (htCap - 1);

        slots[h] = kvIdx;
        return true;
    }

    // Append kvIdx to kvList (growing if needed), then insert into HT
    bool ObjAppendKV(JsonNode& n, std::uint32_t kvIdx, std::uint32_t klen) noexcept
    {
        std::uint32_t count = n.u32a();
        std::uint32_t kvCap = n.u32b();

        auto* buf = n.BufAs<std::uint32_t>();

        if(count >= kvCap) {
            buf = GrowKVCap(n);
            if(!buf) return false;
            // n.u32b() updated inside GrowKVCap
        }

        ObjKVList(buf)[count] = kvIdx;
        n.u32a()++;

        return HTInsert(n, buf, kvIdx, klen);
    }

    bool ArrAppend(JsonNode& n, std::uint32_t elemIdx) noexcept
    {
        std::uint32_t count = n.u32a();
        std::uint32_t cap   = n.u32b();
        auto*         buf   = n.BufAs<std::uint32_t>();

        if(count >= cap) {
            std::uint32_t nc = cap ? cap * 2 : JSON_INIT_CAP;
            auto* newBuf = static_cast<std::uint32_t*>(
                buf ? Realloc(buf, nc * sizeof(std::uint32_t)) : Alloc(nc * sizeof(std::uint32_t))
            );

            if(!newBuf)
                return false;

            n.u32b() = nc;
            n.BufSet(newBuf);
            buf = newBuf;
        }

        buf[count] = elemIdx;
        n.u32a()++;
        return true;
    }

    JsonRef GetOrCreate(const char* key, std::uint32_t klen) noexcept
    {
        if(!Valid())
            return Dead();

        auto& n = NMut();
        if(n.tag == JsonTag::EMPTY) { n.tag = JsonTag::OBJECT; n.u64a = 0; n.u64b = 0; }
        if(n.tag != JsonTag::OBJECT) return Dead();

        // Check existing key first
        std::uint32_t existing = HTFind(key, klen);
        if(existing != JSON_NIL)
            return JsonRef{s_, s_->kvs[existing].valIdx};

        // New key: allocate string, value node, JsonKV
        std::uint32_t keyOff = s_->AllocStr(key, klen);
        if(keyOff == JSON_NIL)
            return Dead();

        std::uint32_t valIdx = s_->AllocNode();
        if(valIdx == JSON_NIL)
            return Dead();

        std::uint32_t kvIdx = s_->AllocKV();
        if(kvIdx == JSON_NIL)
            return Dead();

        // Re-fetch node after potential reallocs
        auto& n2  = NMut();
        JsonKV&   kv  = s_->kvs[kvIdx];
        kv.keyOff = keyOff;
        kv.keyLen = klen;
        kv.valIdx = valIdx;

        if(!ObjAppendKV(n2, kvIdx, klen))
            return Dead();

        return JsonRef{s_, valIdx};
    }

    JsonRef Find(const char* key, std::uint32_t klen) const noexcept
    {
        if(!Valid() || N().tag != JsonTag::OBJECT)
            return JsonRef{nullptr, JSON_NIL};

        std::uint32_t ki = HTFind(key, klen);
        if(ki == JSON_NIL)
            return JsonRef{nullptr, JSON_NIL};

        return JsonRef{s_, s_->kvs[ki].valIdx};
    }

    bool EraseKey(const char* key, std::uint32_t klen) noexcept
    {
        auto& n   = NMut();
        auto* buf = n.BufAs<std::uint32_t>();
        if(!buf)
            return false;

        std::uint32_t htCap = ObjHtCap(buf);
        if(htCap == 0)
            return false;

        std::uint32_t  kvCap = n.u32b();
        std::uint32_t* slots = ObjHTSlot(buf, kvCap);
        std::uint32_t  h     = static_cast<std::uint32_t>(Shared::Hasher::WyHash(key, klen)) & (htCap - 1);

        std::uint32_t htSlot = JSON_NIL;
        std::uint32_t kvIdx  = JSON_NIL;

        while(slots[h] != JSON_NIL) {
            if(slots[h] != JSON_TOMB) {
                const JsonKV& kv = s_->kvs[slots[h]];
                if(kv.keyLen == klen && std::memcmp(s_->strs + kv.keyOff, key, klen) == 0) {
                    htSlot = h;
                    kvIdx  = slots[h];
                    break;
                }
            }

            h = (h + 1) & (htCap - 1);
        }

        if(kvIdx == JSON_NIL)
            return false;

        // Tombstone the HT slot
        slots[htSlot] = JSON_TOMB;

        // Remove from kvList (shift left)
        std::uint32_t* list  = ObjKVList(buf);
        std::uint32_t  count = n.u32a();
        for(std::uint32_t i = 0; i < count; ++i) {
            if(list[i] == kvIdx) {
                std::memmove(list + i, list + i + 1, (count - i - 1) * sizeof(std::uint32_t));
                break;
            }
        }

        n.u32a()--;
        s_->FreeKV(kvIdx);

        // Rebuild HT if tombstone density > 25%
        // Count tombstones by scanning slots, slots pointer still valid (buf not reallocated yet)
        std::uint32_t tombs = 0;
        for(std::uint32_t i = 0; i < htCap; ++i)
            if(slots[i] == JSON_TOMB)
                ++tombs;

        if(tombs > htCap / 4)
            RebuildHT(n, htCap); // same htCap, just clean tombstones

        return true;
    }

    bool EraseIndex(std::uint32_t i) noexcept
    {
        auto& n = NMut();
        if(i >= n.u32a())
            return false;

        auto* buf = n.BufAs<std::uint32_t>();
        if(!buf)
            return false;

        std::memmove(buf + i, buf + i + 1, (n.u32a() - i - 1) * sizeof(std::uint32_t));
        n.u32a()--;
        return true;
    }

private:
    friend class JsonObject;
    friend class JsonParser;

    JsonStore*        s_;
    std::uint32_t idx_;
};

static_assert(sizeof(JsonRef) == 16,                 "JsonRef must be 16 bytes");
static_assert(std::is_trivially_copyable_v<JsonRef>, "JsonRef must be trivially copyable");

// vvv JsonObject (owns the JsonStore, crosses module boundaries safely) vvv
//
class JsonObject {
public:
    ~JsonObject() noexcept { Destroy(); }

    JsonObject() = default;
    JsonObject(JsonObject&& o) noexcept : s_(o.s_) { o.s_ = nullptr; }
    JsonObject& operator=(JsonObject&& o) noexcept
    {
        if(this != &o) {
            Destroy();
            s_ = o.s_;
            o.s_ = nullptr;
        }

        return *this;
    }

    JsonObject(const JsonObject&)            = delete;
    JsonObject& operator=(const JsonObject&) = delete;

public:
    static JsonObject Init() noexcept
    {
        auto* s = static_cast<JsonStore*>(Alloc(sizeof(JsonStore)));
        if(!s)
            return {};

        new(s) JsonStore();

        if(s->AllocNode() == JSON_NIL) {
            Free(s);
            return {};
        }

        JsonNode& root = s->nodes[0];
        root.tag  = JsonTag::OBJECT;
        root.u64a = 0;
        root.u64b = 0;

        return JsonObject{s};
    }

    static JsonObject Init(std::uint32_t nodeHint, std::uint32_t kvHint, std::uint32_t strHint) noexcept
    {
        auto obj = Init();
        if(!obj.Valid())
            return {};

        if(!obj.s_->Reserve(nodeHint, kvHint, strHint))
            return {};

        return obj;
    }

public:
    bool Valid() const noexcept { return s_ != nullptr; }

    JsonRef operator[](std::string_view key) noexcept
    {
        if(!s_) return JsonRef{nullptr, JSON_NIL};
        return JsonRef{s_, 0}[key];
    }

    JsonRef Get(std::string_view key) const noexcept
    {
        if(!s_) return JsonRef{nullptr, JSON_NIL};
        return JsonRef{const_cast<JsonStore*>(s_), 0}.Get(key);
    }

    template<typename T>
    JsonRef Set(std::string_view key, T&& val) noexcept
    {
        if(!s_) return JsonRef{nullptr, JSON_NIL};
        return JsonRef{s_, 0}.Set(key, std::forward<T>(val));
    }

    bool Erase(std::string_view key) noexcept
    {
        if(!s_) return false;
        return JsonRef{s_, 0}.Erase(key);
    }

    // Shallow merge, copies top-level keys from other into this
    void Merge(const JsonObject& other) noexcept
    {
        if(!s_ || !other.s_)
            return;

        const JsonNode& root = other.s_->nodes[0];
        if(root.tag != JsonTag::OBJECT)
            return;

        const auto* buf = root.BufAs<std::uint32_t>();
        if(!buf)
            return;

        const std::uint32_t* list = ObjKVList(buf);
        const JsonKV*            kvs  = other.s_->kvs;
        const char*          strs = other.s_->strs;

        for(std::uint32_t i = 0; i < root.u32a(); ++i) {
            const JsonKV&   kv  = kvs[list[i]];
            const char* key = strs + kv.keyOff;

            JsonRef dst = JsonRef{s_, 0}.GetOrCreate(key, kv.keyLen);
            if(dst.Valid())
                dst.NMut() = other.s_->nodes[kv.valIdx];
        }
    }

    void Write(Http::Response& res) noexcept
    {
        if(!s_) return;
        res.Header("Content-Type", "application/json");
        Serialize(res, 0);
        res.Commit();
    }

    JsonStore*       GetStore()       noexcept { return s_; }
    const JsonStore* GetStore() const noexcept { return s_; }

private:
    explicit JsonObject(JsonStore* s) noexcept : s_(s) {}

    void Destroy() noexcept
    {
        if(!s_)
            return;

        s_->FreeNodeData();
        Free(s_->nodes);
        Free(s_->kvs);
        Free(s_->strs);
        Free(s_);

        s_ = nullptr;
    }

    static void Raw(Http::Response& res, const char* d, std::size_t l) noexcept { res.Write({d, l}); }

    static void Escaped(Http::Response& res, const char* p, std::uint32_t len) noexcept
    {
        const char* e = p + len, *start = p;
        while(p != e) {
            const char* esc = nullptr; std::size_t el = 0;
            switch(*p) {
                case '"':  esc = "\\\""; el = 2; break;
                case '\\': esc = "\\\\"; el = 2; break;
                case '\n': esc = "\\n";  el = 2; break;
                case '\r': esc = "\\r";  el = 2; break;
                case '\t': esc = "\\t";  el = 2; break;
                case '\b': esc = "\\b";  el = 2; break;
                case '\f': esc = "\\f";  el = 2; break;
                default: break;
            }

            if(esc) {
                if(p > start)
                    Raw(res, start, p - start);

                Raw(res, esc, el);
                start = p + 1;
            }

            ++p;
        }

        if(p > start)
            Raw(res, start, p - start);
    }

    void Serialize(Http::Response& res, std::uint32_t idx) const noexcept
    {
        const JsonNode& n    = s_->nodes[idx];
        const char* strs = s_->strs;

        switch(n.tag) {
            case JsonTag::EMPTY:  Raw(res, "null", 4);  break;
            case JsonTag::BOOL:   n.u64a ? Raw(res, "true", 4) : Raw(res, "false", 5); break;

            case JsonTag::INT64:  { std::int64_t  v; std::memcpy(&v, &n.u64a, 8); char b[20]; auto [e,_] = std::to_chars(b, b+20, v); Raw(res, b, e-b); break; }
            case JsonTag::UINT64: { std::uint64_t v; std::memcpy(&v, &n.u64a, 8); char b[20]; auto [e,_] = std::to_chars(b, b+20, v); Raw(res, b, e-b); break; }
            case JsonTag::DOUBLE: { double        v; std::memcpy(&v, &n.u64a, 8); char b[32]; auto [e,_] = std::to_chars(b, b+32, v); Raw(res, b, e-b); break; }

            case JsonTag::STR_VIEW: {
                const char* ptr; std::memcpy(&ptr, &n.u64a, 8);
                Raw(res, "\"", 1); Escaped(res, ptr, n.u32c()); Raw(res, "\"", 1);
                break;
            }

            case JsonTag::STR_OWN:
                Raw(res, "\"", 1); Escaped(res, strs + n.u32a(), n.u32c()); Raw(res, "\"", 1);
                break;

            case JsonTag::ARRAY: {
                Raw(res, "[", 1);
                const auto* buf   = n.BufAs<std::uint32_t>();
                std::uint32_t cnt = n.u32a();

                for(std::uint32_t i = 0; i < cnt; ++i) {
                    if(i)
                        Raw(res, ",", 1);

                    Serialize(res, buf[i]);
                }

                Raw(res, "]", 1);
                break;
            }

            case JsonTag::OBJECT: {
                Raw(res, "{", 1);
                const auto*          buf  = n.BufAs<std::uint32_t>();
                const std::uint32_t* list = buf ? ObjKVList(buf) : nullptr;
                std::uint32_t        cnt  = n.u32a();
                const JsonKV*            kvs  = s_->kvs;

                for(std::uint32_t i = 0; i < cnt; ++i) {
                    if(i)
                        Raw(res, ",", 1);

                    const JsonKV& kv = kvs[list[i]];
                    Raw(res, "\"", 1);
                    Escaped(res, strs + kv.keyOff, kv.keyLen);
                    Raw(res, "\":", 2);
                    Serialize(res, kv.valIdx);
                }

                Raw(res, "}", 1);
                break;
            }
        }
    }

private:
    friend class JsonParser;
    JsonStore* s_ = nullptr;
};

static_assert(sizeof(JsonObject) == 8,               "JsonObject must be 8 bytes");
static_assert(std::is_standard_layout_v<JsonObject>, "JsonObject must be standard layout");

} // namespace WFX::Shared

#endif // WFX_SHARED_JSON_OBJECT_HPP