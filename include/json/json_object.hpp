#ifndef WFX_INC_JSON_OBJECT_HPP
#define WFX_INC_JSON_OBJECT_HPP

#include "core/core.hpp"
#include "http/response.hpp"
#include "shared/abis/uuid.hpp"
#include "shared/utils/hash.hpp"
#include <string_view>
#include <cstdint>
#include <cstring>
#include <charconv>
#include <bit>

namespace WFX::Json {

static constexpr std::uint32_t NIL         = 0xFFFFFFFF;
static constexpr std::uint32_t HASH_THRESH = 8;
static constexpr std::uint32_t INIT_CAP    = 256;

// vvv KV key encoding, packed into a single uint64_t vvv
//
// View key  : top 32 bits are non-zero (actual ptr high32)
//             bottom 32 bits = ptr low32
//             full uint64 = the const char* pointer
//
// Owned key : top 32 bits are zero
//             bottom 32 bits = offset into str region
//
// Since heap pointers on 64-bit always have non-zero high32-
// -(user space addresses on x86-64 are 0x0000'XXXX'XXXX'XXXX,-
// -so high32 of a valid pointer is always > 0), zero high32 means owned

static constexpr std::uint64_t KV_OWNED_MASK = 0x00000000FFFFFFFFull;
static constexpr std::uint64_t KV_VIEW_MASK  = 0xFFFFFFFF00000000ull;

constexpr bool          KVKeyIsView  (std::uint64_t k) noexcept { return (k & KV_VIEW_MASK) != 0; }
constexpr bool          KVKeyIsOwned (std::uint64_t k) noexcept { return (k & KV_VIEW_MASK) == 0; }
constexpr std::uint32_t KVKeyOwnedOff(std::uint64_t k) noexcept { return static_cast<std::uint32_t>(k & KV_OWNED_MASK); }

inline std::uint64_t KVKeyPackView(const char* ptr) noexcept
{
    std::uint64_t packed = 0;
    std::memcpy(&packed, &ptr, 8);
    return packed;
}

inline const char* KVKeyUnpackView(std::uint64_t k) noexcept
{
    const char* ptr;
    std::memcpy(&ptr, &k, 8);
    return ptr;
}

inline const char* KVKeyResolve(std::uint64_t k, const char* strs) noexcept
{
    return KVKeyIsView(k) ? KVKeyUnpackView(k) : strs + KVKeyOwnedOff(k);
}

enum class JsonTag : std::uint8_t {
    EMPTY    = 0,
    BOOL     = 1,
    INT64    = 2,
    UINT64   = 3,
    DOUBLE   = 4,
    STR_VIEW = 5, // const char*, user owns it
    STR_OWN  = 6, // copied into store
    ARRAY    = 7,
    OBJ_LNR  = 8, // <  HASH_THRESH keys, linear scan
    OBJ_HASH = 9, // >= HASH_THRESH keys, hash table in NodeBlock
};

// vvv NodeBlock, one separately allocated block per container node vvv
//
// Layout in memory:
//   p[0]               = kvCap
//   p[1]               = htCap  (0 until OBJ_HASH)
//   p[2..kvCap+1]      = KV indices (insertion order, used for serialization)
//   p[kvCap+2..end]    = HT slots  (NIL = empty, else KV index)
//
// One pointer in Node::u64b and one Free on node destroy
// kvCap grows geometrically via GrowKV
// HT region is appended / rebuilt via BuildHT
// The whole block is reallocated when either region needs to grow
// BuildHT copies KV indices to a temp buffer before realloc to avoid-
// -use-after-free when kvIndices points into p

struct KV;

struct NodeBlock {
    std::uint32_t* p     = nullptr;
    std::uint32_t  kvCap = 0;
    std::uint32_t  htCap = 0;

public: // vvv Accessors vvv
    std::uint32_t*       KVs()       noexcept { return p + 2; }
    std::uint32_t*       HTs()       noexcept { return p + 2 + kvCap; }
    const std::uint32_t* KVs() const noexcept { return p + 2; }
    const std::uint32_t* HTs() const noexcept { return p + 2 + kvCap; }

public: // vvv Memory API vvv
    static void* Alloc  (std::size_t n)          noexcept { return WFX::Core::MemoryApi()->Alloc(n); }
    static void  Free   (void* ptr)              noexcept { WFX::Core::MemoryApi()->Free(ptr); }
    static void* Realloc(void* p, std::size_t n) noexcept { return WFX::Core::MemoryApi()->Realloc(p, n); }

public: // vvv Grow KV region, preserving HT slots vvv
    bool GrowKV(std::uint32_t newKvCap) noexcept
    {
        std::uint32_t total = 2 + newKvCap + htCap;
        auto* np = static_cast<std::uint32_t*>(
            p ? Realloc(p, total * sizeof(std::uint32_t)) : Alloc(total * sizeof(std::uint32_t))
        );

        if(!np)
            return false;

        // Move HT slots to new position if they exist
        if(htCap && p && newKvCap != kvCap)
            std::memmove(np + 2 + newKvCap, np + 2 + kvCap, htCap * sizeof(std::uint32_t));

        p     = np;
        p[0]  = newKvCap;
        p[1]  = htCap;
        kvCap = newKvCap;

        return true;
    }

    // vvv Build or rebuild HT region from scratch with newHtCap slots vvv
    // Copies kvIndices to a temp buffer before realloc since kvIndices
    // may point into p itself, avoids use-after-free
    bool BuildHT(
        std::uint32_t newHtCap, const std::uint32_t* kvIndices,
        std::uint32_t count, const KV* kvs, const char* strs
    ) noexcept;

    void Destroy() noexcept
    {
        if(p) {
            Free(p);
            p = nullptr;
        }

        kvCap = htCap = 0;
    }
};

static_assert(sizeof(NodeBlock) == 16,              "'NodeBlock' must be exactly 16 bytes");
static_assert(std::is_standard_layout_v<NodeBlock>, "'NodeBlock' must be standard layout");
static_assert(offsetof(NodeBlock, p)     == 0,      "NodeBlock::p offset changed");
static_assert(offsetof(NodeBlock, kvCap) == 8,      "NodeBlock::kvCap offset changed");
static_assert(offsetof(NodeBlock, htCap) == 12,     "NodeBlock::htCap offset changed");

struct Node {
    JsonTag      tag  = JsonTag::EMPTY; // |
    std::uint8_t _Pad[7] = {};          // | -> 8 byte aligned

    // vvv Slot meanings per tag vvv
    //
    // BOOL      : u64a = 0/1
    // INT64     : u64a = int64_t  (memcpy)
    // UINT64    : u64a = uint64_t (memcpy)
    // DOUBLE    : u64a = double   (memcpy)
    // STR_VIEW  : u64a = packed const char* pointer
    //             u32c = byte length
    // STR_OWN   : u32a = str region offset
    //             u32c = byte length
    //
    // ARRAY / OBJ_LNR / OBJ_HASH:
    //   u32b = element / key count  (high32 of u64a)
    //          never aliased by u64b, safe across BlockSet calls
    //   u64b = NodeBlock* pointer
    //          one allocation owns both KV index list and HT slots
    //          nullptr until first insert, freed on object destroy

    std::uint64_t u64a = 0;
    std::uint64_t u64b = 0;

    // vvv uint32 aliases into u64a and u64b vvv
    std::uint32_t& u32a() noexcept { return reinterpret_cast<std::uint32_t*>(&u64a)[0]; }
    std::uint32_t& u32b() noexcept { return reinterpret_cast<std::uint32_t*>(&u64a)[1]; }
    std::uint32_t& u32c() noexcept { return reinterpret_cast<std::uint32_t*>(&u64b)[0]; }
    std::uint32_t& u32d() noexcept { return reinterpret_cast<std::uint32_t*>(&u64b)[1]; }

    std::uint32_t u32a() const noexcept { return reinterpret_cast<const std::uint32_t*>(&u64a)[0]; }
    std::uint32_t u32b() const noexcept { return reinterpret_cast<const std::uint32_t*>(&u64a)[1]; }
    std::uint32_t u32c() const noexcept { return reinterpret_cast<const std::uint32_t*>(&u64b)[0]; }
    std::uint32_t u32d() const noexcept { return reinterpret_cast<const std::uint32_t*>(&u64b)[1]; }

    // vvv NodeBlock pointer helpers, only valid for container nodes vvv
    NodeBlock* Block() noexcept
    {
        NodeBlock* b;
        std::memcpy(&b, &u64b, 8);
        return b;
    }

    const NodeBlock* Block() const noexcept
    {
        const NodeBlock* b;
        std::memcpy(&b, &u64b, 8);
        return b;
    }

    void BlockSet(NodeBlock* b) noexcept { std::memcpy(&u64b, &b, 8); }
};

struct KV {
    // top 32 bits != 0 -> view  : full uint64 = packed const char* pointer
    // top 32 bits == 0 -> owned : bottom 32 = offset into str region
    std::uint64_t key    = 0;
    std::uint32_t keyLen = 0;
    std::uint32_t val    = NIL; // Node index
};

static_assert(sizeof(Node) == 24, "'Node' must be exactly 24 bytes");
static_assert(sizeof(KV) == 16,   "'KV' must be exactly 16 bytes");

static_assert(std::is_standard_layout_v<Node>,    "'Node' must be standard layout");
static_assert(std::is_standard_layout_v<KV>,      "'KV' must be standard layout");
static_assert(std::is_trivially_copyable_v<Node>, "'Node' must be trivially copyable");
static_assert(std::is_trivially_copyable_v<KV>,   "'KV' must be trivially copyable");

static_assert(offsetof(Node, tag)  == 0,  "Node::tag offset changed");
static_assert(offsetof(Node, u64a) == 8,  "Node::u64a offset changed");
static_assert(offsetof(Node, u64b) == 16, "Node::u64b offset changed");
static_assert(offsetof(KV, key) == 0,     "KV::key offset changed");
static_assert(offsetof(KV, keyLen) == 8,  "KV::keyLen offset changed");
static_assert(offsetof(KV, val) == 12,    "KV::val offset changed");

// vvv One allocation, three packed regions vvv
// [ Node[] | KV[] | str bytes ]
// Each container node owns a NodeBlock (separate allocation) for its-
// -KV index list and optional HT, no interleaving between nodes
struct Store {
    std::byte*    p       = nullptr;
    std::uint32_t cap     = 0;
    std::uint32_t _Pad    = 0;
    std::uint32_t nodeCap = 0;
    std::uint32_t nodeLen = 0;
    std::uint32_t kvCap   = 0;
    std::uint32_t kvLen   = 0;
    std::uint32_t strCap  = 0;
    std::uint32_t strLen  = 0;

public: // vvv Accessors vvv
    Node* Nodes() noexcept { return reinterpret_cast<Node*>(p); }
    KV*   KVs()   noexcept { return reinterpret_cast<KV*>(p + nodeCap * sizeof(Node)); }
    char* Strs()  noexcept { return reinterpret_cast<char*>(p + nodeCap * sizeof(Node) + kvCap * sizeof(KV)); }

    const Node* Nodes() const noexcept { return reinterpret_cast<const Node*>(p); }
    const KV*   KVs()   const noexcept { return reinterpret_cast<const KV*>(p + nodeCap * sizeof(Node)); }
    const char* Strs()  const noexcept { return reinterpret_cast<const char*>(p + nodeCap * sizeof(Node) + kvCap * sizeof(KV)); }

public: // vvv Memory API vvv
    static void* Alloc  (std::size_t n)            noexcept { return WFX::Core::MemoryApi()->Alloc(n); }
    static void  Free   (void* ptr)                noexcept { WFX::Core::MemoryApi()->Free(ptr); }
    static void* Realloc(void* ptr, std::size_t n) noexcept { return WFX::Core::MemoryApi()->Realloc(ptr, n); }

public: // vvv Allocators vvv
    bool Grow(std::uint32_t need) noexcept
    {
        std::uint32_t newCap = cap ? cap * 2 : INIT_CAP;
        while(newCap < need)
            newCap *= 2;

        auto* np = static_cast<std::byte*>(Realloc(p, newCap));
        if(!np)
            return false;

        p   = np;
        cap = newCap;
        return true;
    }

    std::uint32_t AllocNode() noexcept
    {
        if(nodeLen == nodeCap) {
            std::uint32_t nc   = nodeCap ? nodeCap * 2 : 4;
            std::uint32_t need = nc * sizeof(Node) + kvCap * sizeof(KV) + strCap;

            if(need > cap && !Grow(need))
                return NIL;

            std::uint32_t oldKvOff  = nodeCap * sizeof(Node);
            std::uint32_t newKvOff  = nc      * sizeof(Node);
            std::uint32_t oldStrOff = oldKvOff + kvCap * sizeof(KV);
            std::uint32_t newStrOff = newKvOff + kvCap * sizeof(KV);

            // Move back-to-front to avoid overlap
            if(strLen) std::memmove(p + newStrOff, p + oldStrOff, strLen);
            if(kvLen)  std::memmove(p + newKvOff,  p + oldKvOff,  kvLen * sizeof(KV));

            nodeCap = nc;
            strCap  = cap - newStrOff;
        }

        std::uint32_t idx = nodeLen++;
        new(&Nodes()[idx]) Node();
        return idx;
    }

    std::uint32_t AllocKV() noexcept
    {
        if(kvLen == kvCap) {
            std::uint32_t kc        = kvCap ? kvCap * 2 : 4;
            std::uint32_t oldStrOff = nodeCap * sizeof(Node) + kvCap * sizeof(KV);
            std::uint32_t newStrOff = nodeCap * sizeof(Node) + kc    * sizeof(KV);
            std::uint32_t need      = newStrOff + strCap;

            if(need > cap && !Grow(need))
                return NIL;

            // Only strings need moving, KVs expand in place
            if(strLen)
                std::memmove(p + newStrOff, p + oldStrOff, strLen);

            kvCap  = kc;
            strCap = cap - newStrOff;
        }

        std::uint32_t idx = kvLen++;
        new(&KVs()[idx]) KV();
        return idx;
    }

    std::uint32_t AllocStr(const char* data, std::uint32_t len) noexcept
    {
        std::uint32_t need = strLen + len + 1;
        if(need > strCap) {
            std::uint32_t sc = strCap ? strCap * 2 : 64;
            while(sc < need) sc *= 2;

            // Strings are at the end, just grow the buffer
            std::uint32_t strOff = nodeCap * sizeof(Node) + kvCap * sizeof(KV);
            std::uint32_t total  = strOff + sc;

            if(total > cap && !Grow(total)) return NIL;

            strCap = sc;
        }

        std::uint32_t off = strLen;
        if(data)
            std::memcpy(Strs() + off, data, len);

        Strs()[off + len] = '\0';
        strLen += len + 1;
        return off;
    }

    // vvv Walk all nodes and free NodeBlock allocations vvv
    void FreeNodeData() noexcept
    {
        for(std::uint32_t i = 0; i < nodeLen; ++i) {
            Node& n = Nodes()[i];
            if(n.tag == JsonTag::ARRAY
                || n.tag == JsonTag::OBJ_LNR
                || n.tag == JsonTag::OBJ_HASH)
            {
                NodeBlock* b = n.Block();
                if(b) {
                    b->Destroy();
                    NodeBlock::Free(b);
                }

                n.BlockSet(nullptr);
            }
        }
    }
};

static_assert(std::is_standard_layout_v<Store>, "'Store' must be standard layout");
static_assert(sizeof(Store) == 40,              "'Store' must be exactly 40 bytes");
static_assert(offsetof(Store, p) == 0,          "Store::p offset changed");
static_assert(offsetof(Store, cap) == 8,        "Store::cap offset changed");
static_assert(offsetof(Store, nodeCap) == 16,   "Store::nodeCap offset changed");
static_assert(offsetof(Store, nodeLen) == 20,   "Store::nodeLen offset changed");
static_assert(offsetof(Store, kvCap) == 24,     "Store::kvCap offset changed");
static_assert(offsetof(Store, kvLen) == 28,     "Store::kvLen offset changed");
static_assert(offsetof(Store, strCap) == 32,    "Store::strCap offset changed");
static_assert(offsetof(Store, strLen) == 36,    "Store::strLen offset changed");

// vvv NodeBlock::BuildHT defined here since it needs KV vvv
inline bool NodeBlock::BuildHT(
    std::uint32_t newHtCap, const std::uint32_t* kvIndices,
    std::uint32_t count, const KV* kvs, const char* strs
) noexcept {
    // Copy KV indices before realloc, kvIndices may point into p
    std::uint32_t* tmp = nullptr;
    if(count > 0) {
        tmp = static_cast<std::uint32_t*>(Alloc(count * sizeof(std::uint32_t)));
        if(!tmp)
            return false;

        std::memcpy(tmp, kvIndices, count * sizeof(std::uint32_t));
    }

    std::uint32_t total = 2 + kvCap + newHtCap;
    auto* np = static_cast<std::uint32_t*>(
        p ? Realloc(p, total * sizeof(std::uint32_t)) : Alloc(total * sizeof(std::uint32_t))
    );
    if(!np) {
        if(tmp) Free(tmp);
        return false;
    }

    p     = np;
    p[0]  = kvCap;
    p[1]  = newHtCap;
    htCap = newHtCap;

    std::uint32_t* htSlots = p + 2 + kvCap;
    std::memset(htSlots, 0xFF, newHtCap * sizeof(std::uint32_t));

    // Use safe copy, not the potentially-freed kvIndices
    for(std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t ki  = tmp[i];
        const KV&     kv  = kvs[ki];
        const char*   key = KVKeyResolve(kv.key, strs);
        std::uint32_t h   = Shared::Hasher::Fnv1a(key, kv.keyLen) & (newHtCap - 1);

        while(htSlots[h] != NIL)
            h = (h + 1) & (newHtCap - 1);

        htSlots[h] = ki;
    }

    if(tmp)
        Free(tmp);

    return true;
}

class JsonObject;

// vvv Stack proxy into the store vvv
class Ref {
public: // Constructor
    Ref(Store* s, std::uint32_t idx) noexcept
        : s_(s), idx_(idx)
    {}

public: // vvv Type checks vvv
    bool Valid()    const noexcept { return s_ && idx_ != NIL; }
    bool IsNull()   const noexcept { return !Valid() || N().tag == JsonTag::EMPTY; }
    bool IsBool()   const noexcept { return  Valid() && N().tag == JsonTag::BOOL; }
    bool IsInt()    const noexcept { return  Valid() && N().tag == JsonTag::INT64; }
    bool IsUInt()   const noexcept { return  Valid() && N().tag == JsonTag::UINT64; }
    bool IsDouble() const noexcept { return  Valid() && N().tag == JsonTag::DOUBLE; }
    bool IsString() const noexcept { return  Valid() && (N().tag == JsonTag::STR_VIEW || N().tag == JsonTag::STR_OWN); }
    bool IsArray()  const noexcept { return  Valid() && N().tag == JsonTag::ARRAY; }
    bool IsObject() const noexcept { return  Valid() && (N().tag == JsonTag::OBJ_LNR || N().tag == JsonTag::OBJ_HASH); }

public: // vvv Value extraction vvv
    bool AsBool() const noexcept
    {
        return Valid() && N().u64a != 0;
    }

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
        if(!Valid()) return {};
        const auto& n = N();

        if(n.tag == JsonTag::STR_VIEW) {
            const char* ptr = KVKeyUnpackView(n.u64a);
            return {ptr, n.u32c()};
        }

        if(n.tag == JsonTag::STR_OWN)
            return {s_->Strs() + n.u32a(), n.u32c()};

        return {};
    }

    std::uint32_t Length() const noexcept { return Valid() ? N().u32b() : 0; }

public: // vvv Access vvv
    // Array index access
    Ref operator[](std::uint32_t i) noexcept
    {
        if(!Valid() || N().tag != JsonTag::ARRAY || i >= N().u32b())
            return Dead();

        const NodeBlock* b = N().Block();
        if(!b)
            return Dead();

        return Ref{s_, s_->KVs()[b->KVs()[i]].val};
    }

    // Object key access, const char* view, key must outlive this object
    Ref operator[](std::string_view key) noexcept
    {
        return GetOrCreate(key.data(), static_cast<std::uint32_t>(key.size()), true);
    }

    Ref Get(std::string_view key) const noexcept
    {
        return Find(key.data(), static_cast<std::uint32_t>(key.size()));
    }

public: // vvv Set with dynamic key, key copied into store, safe for temporaries vvv
    Ref Set(std::string_view key, std::string_view val)    noexcept { Ref r = GetOrCreate(key.data(), static_cast<std::uint32_t>(key.size()), false); if(r.Valid()) r = val;     return r; }
    Ref Set(std::string_view key, const char* val)         noexcept { Ref r = GetOrCreate(key.data(), static_cast<std::uint32_t>(key.size()), false); if(r.Valid()) r = val;     return r; }
    Ref Set(std::string_view key, std::int64_t val)        noexcept { Ref r = GetOrCreate(key.data(), static_cast<std::uint32_t>(key.size()), false); if(r.Valid()) r = val;     return r; }
    Ref Set(std::string_view key, std::uint64_t val)       noexcept { Ref r = GetOrCreate(key.data(), static_cast<std::uint32_t>(key.size()), false); if(r.Valid()) r = val;     return r; }
    Ref Set(std::string_view key, double val)              noexcept { Ref r = GetOrCreate(key.data(), static_cast<std::uint32_t>(key.size()), false); if(r.Valid()) r = val;     return r; }
    Ref Set(std::string_view key, bool val)                noexcept { Ref r = GetOrCreate(key.data(), static_cast<std::uint32_t>(key.size()), false); if(r.Valid()) r = val;     return r; }
    Ref Set(std::string_view key, std::nullptr_t)          noexcept { Ref r = GetOrCreate(key.data(), static_cast<std::uint32_t>(key.size()), false); if(r.Valid()) r = nullptr; return r; }
    Ref Set(std::string_view key, const Shared::UUID& val) noexcept { Ref r = GetOrCreate(key.data(), static_cast<std::uint32_t>(key.size()), false); if(r.Valid()) r = val;     return r; }

    Ref Set(std::string_view key, std::int32_t val)  noexcept { return Set(key, static_cast<std::int64_t>(val)); }
    Ref Set(std::string_view key, std::uint32_t val) noexcept { return Set(key, static_cast<std::uint64_t>(val)); }
    Ref Set(std::string_view key, std::int16_t val)  noexcept { return Set(key, static_cast<std::int64_t>(val)); }
    Ref Set(std::string_view key, std::uint16_t val) noexcept { return Set(key, static_cast<std::uint64_t>(val)); }
    Ref Set(std::string_view key, std::int8_t val)   noexcept { return Set(key, static_cast<std::int64_t>(val)); }
    Ref Set(std::string_view key, std::uint8_t val)  noexcept { return Set(key, static_cast<std::uint64_t>(val)); }
    Ref Set(std::string_view key, float val)         noexcept { return Set(key, static_cast<double>(val)); }

public: // vvv Array push, returns Ref to new element vvv
    Ref PushBack() noexcept
    {
        if(!Valid()) return Dead();

        auto& n = NMut();
        if(n.tag == JsonTag::EMPTY) { n.tag = JsonTag::ARRAY; n.u64a = 0; n.u64b = 0; }
        if(n.tag != JsonTag::ARRAY) return Dead();

        std::uint32_t valIdx = s_->AllocNode(); if(valIdx == NIL) return Dead();
        std::uint32_t kvIdx  = s_->AllocKV();   if(kvIdx  == NIL) return Dead();

        s_->KVs()[kvIdx].val = valIdx;

        // Re-fetch after potential realloc, then append to this node's block
        auto& n2 = NMut();
        if(!AppendKV(n2, kvIdx))
            return Dead();

        return Ref{s_, valIdx};
    }

    void PushBack(std::nullptr_t)        noexcept { auto r = PushBack(); r = nullptr; }
    void PushBack(bool v)                noexcept { auto r = PushBack(); r = v; }
    void PushBack(std::int64_t v)        noexcept { auto r = PushBack(); r = v; }
    void PushBack(std::uint64_t v)       noexcept { auto r = PushBack(); r = v; }
    void PushBack(double v)              noexcept { auto r = PushBack(); r = v; }
    void PushBack(const char* v)         noexcept { auto r = PushBack(); r = v; }
    void PushBack(std::string_view v)    noexcept { auto r = PushBack(); r = v; }
    void PushBack(const Shared::UUID& v) noexcept { auto r = PushBack(); r = v; }
    void PushBack(std::int32_t v)        noexcept { PushBack(static_cast<std::int64_t>(v));  }
    void PushBack(std::uint32_t v)       noexcept { PushBack(static_cast<std::uint64_t>(v)); }
    void PushBack(float v)               noexcept { PushBack(static_cast<double>(v)); }

public: // vvv Assign vvv
    Ref& operator=(std::nullptr_t) noexcept { if(Valid()) NMut().tag = JsonTag::EMPTY; return *this; }

    Ref& operator=(bool v) noexcept
    {
        if(!Valid()) return *this;
        auto& n = NMut(); n.tag = JsonTag::BOOL; n.u64a = v ? 1u : 0u;
        return *this;
    }

    Ref& operator=(std::int64_t v) noexcept
    {
        if(!Valid()) return *this;
        auto& n = NMut(); n.tag = JsonTag::INT64; std::memcpy(&n.u64a, &v, 8);
        return *this;
    }

    Ref& operator=(std::uint64_t v) noexcept
    {
        if(!Valid()) return *this;
        auto& n = NMut(); n.tag = JsonTag::UINT64; std::memcpy(&n.u64a, &v, 8);
        return *this;
    }

    Ref& operator=(double v) noexcept
    {
        if(!Valid()) return *this;
        auto& n = NMut(); n.tag = JsonTag::DOUBLE; std::memcpy(&n.u64a, &v, 8);
        return *this;
    }

    // const char*, zero copy view, pointer packed into u64a
    Ref& operator=(const char* v) noexcept
    {
        if(!Valid() || !v) return *this;
        auto& n  = NMut();
        n.tag    = JsonTag::STR_VIEW;
        n.u64a   = KVKeyPackView(v);
        n.u32c() = static_cast<std::uint32_t>(std::strlen(v));
        return *this;
    }

    // string_view, copied into str region
    Ref& operator=(std::string_view v) noexcept
    {
        if(!Valid()) return *this;
        std::uint32_t off = s_->AllocStr(v.data(), static_cast<std::uint32_t>(v.size()));
        if(off == NIL) return *this;
        auto& n  = NMut();
        n.tag    = JsonTag::STR_OWN;
        n.u32a() = off;
        n.u32c() = static_cast<std::uint32_t>(v.size());
        return *this;
    }

    Ref& operator=(const Shared::UUID& v) noexcept
    {
        auto s = v.ToString(); return *this = std::string_view{s.data, 36};
    }

    Ref& operator=(std::int32_t v)  noexcept { return *this = static_cast<std::int64_t>(v); }
    Ref& operator=(std::uint32_t v) noexcept { return *this = static_cast<std::uint64_t>(v); }
    Ref& operator=(std::int16_t v)  noexcept { return *this = static_cast<std::int64_t>(v); }
    Ref& operator=(std::uint16_t v) noexcept { return *this = static_cast<std::uint64_t>(v); }
    Ref& operator=(std::int8_t v)   noexcept { return *this = static_cast<std::int64_t>(v); }
    Ref& operator=(std::uint8_t v)  noexcept { return *this = static_cast<std::uint64_t>(v); }
    Ref& operator=(float v)         noexcept { return *this = static_cast<double>(v); }

    Node& NMut() noexcept { return s_->Nodes()[idx_]; }

private: // vvv Internals vvv
    Ref         Dead() const noexcept { return Ref{nullptr, NIL}; }
    const Node& N()    const noexcept { return s_->Nodes()[idx_]; }

    // vvv Get or allocate NodeBlock for a container node vvv
    static NodeBlock* GetOrAllocBlock(Node& n) noexcept
    {
        NodeBlock* b = n.Block();
        if(b)
            return b;

        b = static_cast<NodeBlock*>(NodeBlock::Alloc(sizeof(NodeBlock)));
        if(!b)
            return nullptr;

        new(b) NodeBlock();
        n.BlockSet(b);
        return b;
    }

    // vvv Append kvIdx to node's ordered KV list, growing block if needed vvv
    static bool AppendKV(Node& n, std::uint32_t kvIdx) noexcept
    {
        NodeBlock* b = GetOrAllocBlock(n);
        if(!b)
            return false;

        std::uint32_t count = n.u32b();
        if(count >= b->kvCap) {
            std::uint32_t newCap = b->kvCap ? b->kvCap * 2 : 4;
            if(!b->GrowKV(newCap))
                return false;
        }

        n.Block()->KVs()[count] = kvIdx;
        n.u32b()++;
        return true;
    }

    void BuildHT() noexcept
    {
        auto& n = NMut();
        NodeBlock* b = n.Block();
        if(!b)
            return;

        // Always 2x key count to keep load under 50%
        std::uint32_t htCap = std::bit_ceil(n.u32b() * 2u);
        if(!b->BuildHT(htCap, b->KVs(), n.u32b(), s_->KVs(), s_->Strs()))
            return;

        n.tag = JsonTag::OBJ_HASH;
    }

    void MaybeRehash() noexcept
    {
        auto& n = NMut();
        if(n.tag != JsonTag::OBJ_HASH)
            return;

        NodeBlock* b = n.Block();
        if(!b || n.u32b() <= b->htCap / 2)
            return;

        b->BuildHT(b->htCap * 2, b->KVs(), n.u32b(), s_->KVs(), s_->Strs());
    }

    Ref Find(const char* key, std::uint32_t klen) const noexcept
    {
        if(!Valid())
            return Ref{nullptr, NIL};

        const auto& n    = N();
        const char* strs = s_->Strs();

        if(n.tag == JsonTag::OBJ_LNR) {
            const NodeBlock* b = n.Block();
            if(!b)
                return Ref{nullptr, NIL};

            const KV*            kvs  = s_->KVs();
            const std::uint32_t* list = b->KVs();

            for(std::uint32_t i = 0; i < n.u32b(); ++i) {
                const KV&   kv    = kvs[list[i]];
                const char* kvKey = KVKeyResolve(kv.key, strs);
                if(kv.keyLen == klen && std::memcmp(kvKey, key, klen) == 0)
                    return Ref{s_, kv.val};
            }

            return Ref{nullptr, NIL};
        }

        if(n.tag == JsonTag::OBJ_HASH) {
            const NodeBlock* b = n.Block();
            if(!b)
                return Ref{nullptr, NIL};

            const std::uint32_t* htSlots = b->HTs();
            std::uint32_t        htCap   = b->htCap;
            std::uint32_t        h       = Shared::Hasher::Fnv1a(key, klen) & (htCap - 1);
            const KV*            kvs     = s_->KVs();

            while(htSlots[h] != NIL) {
                const KV&   kv    = kvs[htSlots[h]];
                const char* kvKey = KVKeyResolve(kv.key, strs);
                if(kv.keyLen == klen && std::memcmp(kvKey, key, klen) == 0)
                    return Ref{s_, kv.val};

                h = (h + 1) & (htCap - 1);
            }
        }

        return Ref{nullptr, NIL};
    }

    // isView = true  -> store pointer as-is, key must outlive this object
    // isView = false -> copy key into str region, safe for temporaries
    Ref GetOrCreate(const char* key, std::uint32_t klen, bool isView) noexcept
    {
        if(!Valid())
            return Dead();

        auto& n = NMut();
        if(n.tag == JsonTag::EMPTY) { n.tag = JsonTag::OBJ_LNR; n.u64a = 0; n.u64b = 0; }

        Ref ex = Find(key, klen);
        if(ex.Valid())
            return ex;

        std::uint64_t packedKey = 0;
        if(isView)
            packedKey = KVKeyPackView(key);
        else {
            std::uint32_t off = s_->AllocStr(key, klen);
            if(off == NIL)
                return Dead();

            packedKey = static_cast<std::uint64_t>(off); // top 32 = 0, signals owned
        }

        std::uint32_t valIdx = s_->AllocNode(); if(valIdx == NIL) return Dead();
        std::uint32_t kvIdx  = s_->AllocKV();   if(kvIdx  == NIL) return Dead();

        // Re-fetch after potential reallocs
        auto& n2  = NMut();
        KV&   kv  = s_->KVs()[kvIdx];
        kv.key    = packedKey;
        kv.keyLen = klen;
        kv.val    = valIdx;

        if(!AppendKV(n2, kvIdx))
            return Dead();

        if(n2.u32b() == HASH_THRESH)
            BuildHT();
        else if(n2.tag == JsonTag::OBJ_HASH) {
            // Insert directly into existing HT then check if rehash needed
            NodeBlock*     b       = n2.Block();
            std::uint32_t* htSlots = b->HTs();
            std::uint32_t  htCap   = b->htCap;
            const char*    rkey    = KVKeyResolve(packedKey, s_->Strs());
            std::uint32_t  h       = Shared::Hasher::Fnv1a(rkey, klen) & (htCap - 1);

            while(htSlots[h] != NIL)
                h = (h + 1) & (htCap - 1);

            htSlots[h] = kvIdx;
            MaybeRehash();
        }

        return Ref{s_, valIdx};
    }

private: // Storage
    friend class JsonObject;

    Store*        s_;
    std::uint32_t idx_;
};

// vvv ABI stable JSON object vvv
class JsonObject {
public: // Constructor and Destructor
    ~JsonObject() noexcept { Destroy(); }

    JsonObject(JsonObject&& o) noexcept : s_(o.s_) { o.s_ = nullptr; }
    JsonObject& operator=(JsonObject&& o) noexcept
    {
        if(this != &o) {
            Destroy();
            s_   = o.s_;
            o.s_ = nullptr;
        }

        return *this;
    }

    JsonObject()                             = default;
    JsonObject(const JsonObject&)            = delete;
    JsonObject& operator=(const JsonObject&) = delete;

public: // vvv Initializer vvv
    static JsonObject Init() noexcept
    {
        auto* api = WFX::Core::MemoryApi();

        auto* s = static_cast<Store*>(api->Alloc(sizeof(Store)));
        if(!s)
            return {};

        new(s) Store();

        s->p = static_cast<std::byte*>(api->Alloc(INIT_CAP));
        if(!s->p) {
            api->Free(s);
            return {};
        }

        s->cap = INIT_CAP;

        if(s->AllocNode() == NIL) {
            api->Free(s->p);
            api->Free(s);
            return {};
        }

        Node& root = s->Nodes()[0];
        root.tag   = JsonTag::OBJ_LNR;
        root.u64a  = 0;
        root.u64b  = 0;

        return JsonObject{s};
    }

public: // vvv Main Functions vvv
    bool Valid() const noexcept { return s_ != nullptr; }

    Ref operator[](std::string_view key) noexcept
    {
        if(!s_) return Ref{nullptr, NIL};
        return Ref{s_, 0}[key];
    }

    Ref Get(std::string_view key) const noexcept
    {
        if(!s_) return Ref{nullptr, NIL};
        return Ref{const_cast<Store*>(s_), 0}.Get(key);
    }

    template<typename T>
    Ref Set(std::string_view key, T&& val) noexcept
    {
        if(!s_) return Ref{nullptr, NIL};
        return Ref{s_, 0}.Set(key, std::forward<T>(val));
    }

    // vvv Deep copy top-level keys from other into this vvv
    void Merge(const JsonObject& other) noexcept
    {
        if(!s_ || !other.s_) return;

        const Node& root = other.s_->Nodes()[0];
        if(root.tag != JsonTag::OBJ_LNR && root.tag != JsonTag::OBJ_HASH) return;

        const NodeBlock* b = root.Block();
        if(!b) return;

        const KV*            kvs  = other.s_->KVs();
        const char*          strs = other.s_->Strs();
        const std::uint32_t* list = b->KVs();

        for(std::uint32_t i = 0; i < root.u32b(); ++i) {
            const KV&   kv  = kvs[list[i]];
            const char* key = KVKeyResolve(kv.key, strs);

            // isView = false, copy key into this store, other may not outlive this
            Ref dst = Ref{s_, 0}.GetOrCreate(key, kv.keyLen, false);
            if(dst.Valid())
                dst.NMut() = other.s_->Nodes()[kv.val];
        }
    }

    void Write(Http::Response& res) noexcept
    {
        if(!s_)
            return;

        res.Header("Content-Type", "application/json");
        Serialize(res, 0);
        res.Commit();
    }

private: // vvv Internals vvv
    explicit JsonObject(Store* s) noexcept : s_(s) {}

    void Destroy() noexcept
    {
        if(!s_) return;

        auto* api = WFX::Core::MemoryApi();
        if(api) {
            s_->FreeNodeData();
            api->Free(s_->p);
            api->Free(s_);
        }

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
        const Node& n    = s_->Nodes()[idx];
        const char* strs = s_->Strs();

        switch(n.tag) {
            case JsonTag::EMPTY:  Raw(res, "null",  4); break;
            case JsonTag::BOOL:   n.u64a ? Raw(res, "true", 4) : Raw(res, "false", 5); break;
            case JsonTag::INT64:  { std::int64_t  v; std::memcpy(&v, &n.u64a, 8); char b[20]; auto [e,_] = std::to_chars(b, b + 20, v); Raw(res, b, e - b); break; }
            case JsonTag::UINT64: { std::uint64_t v; std::memcpy(&v, &n.u64a, 8); char b[20]; auto [e,_] = std::to_chars(b, b + 20, v); Raw(res, b, e - b); break; }
            case JsonTag::DOUBLE: { double        v; std::memcpy(&v, &n.u64a, 8); char b[32]; auto [e,_] = std::to_chars(b, b + 32, v); Raw(res, b, e - b); break; }

            case JsonTag::STR_VIEW: {
                const char* ptr = KVKeyUnpackView(n.u64a);
                Raw(res, "\"", 1); Escaped(res, ptr, n.u32c()); Raw(res, "\"", 1);
                break;
            }

            case JsonTag::STR_OWN:
                Raw(res, "\"", 1); Escaped(res, strs + n.u32a(), n.u32c()); Raw(res, "\"", 1);
                break;

            case JsonTag::ARRAY: {
                Raw(res, "[", 1);
                const NodeBlock*     b    = n.Block();
                const std::uint32_t* list = b ? b->KVs() : nullptr;
                const KV*            kvs  = s_->KVs();

                for(std::uint32_t i = 0; i < n.u32b(); ++i) {
                    if(i) Raw(res, ",", 1);
                    Serialize(res, kvs[list[i]].val);
                }

                Raw(res, "]", 1);
                break;
            }

            case JsonTag::OBJ_LNR:
            case JsonTag::OBJ_HASH: {
                Raw(res, "{", 1);
                const NodeBlock*     b    = n.Block();
                const std::uint32_t* list = b ? b->KVs() : nullptr;
                const KV*            kvs  = s_->KVs();

                for(std::uint32_t i = 0; i < n.u32b(); ++i) {
                    if(i) Raw(res, ",", 1);

                    const KV&   kv  = kvs[list[i]];
                    const char* key = KVKeyResolve(kv.key, strs);

                    Raw(res, "\"", 1);
                    Escaped(res, key, kv.keyLen);
                    Raw(res, "\":", 2);
                    Serialize(res, kv.val);
                }

                Raw(res, "}", 1);
                break;
            }
        }
    }

private: // Storage
    Store* s_ = nullptr;
};

static_assert(sizeof(JsonObject) == 8,               "'JsonObject' must be exactly 8 bytes");
static_assert(std::is_standard_layout_v<JsonObject>, "'JsonObject' must be standard layout");

} // namespace WFX::Json

#endif // WFX_INC_JSON_OBJECT_HPP