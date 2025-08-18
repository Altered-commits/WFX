/*
 * Build: g++ -O3 -I. test/timer_wheel_test.cpp utils/timer_wheel/timer_wheel.cpp
 */

#ifndef WFX_UTILS_TIMEOUT_MAP_HPP
#define WFX_UTILS_TIMEOUT_MAP_HPP

// TimeoutMap: sharded, robin-hood + control bytes, AVX2 accelerated scanning,
// prefetch hints, auto-resize (table + node pool), seqlock lockless readers,
// integrated with TimerWheel (uses node ids as stable references).
//
// Requirements:
//  - Compile with -O3
//  - For SIMD: compile with -mavx2 (GCC/Clang). AVX2 code is guarded and falls back to scalar.
//  - Link with your TimerWheel implementation (header/api as provided earlier).

#include <vector>
#include <mutex>
#include <optional>
#include <functional>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <algorithm>
#include <utility>
#include <immintrin.h> // AVX2 (safe to include; uses guards)

#include "utils/timer_wheel/timer_wheel.hpp"

namespace WFX::Utils {

// ------------ implementation details ------------
static constexpr uint8_t CTRL_EMPTY = 0x80u;
static constexpr uint8_t CTRL_TOMB  = 0xFEu;
static constexpr uint32_t INVALID_NODE = UINT32_MAX;

template<typename K, typename V>
struct Node {
    K key;
    V val;
    bool alive = false;
};

template<typename K, typename V,
        uint32_t WHEEL_SLOTS, TimeUnit UNIT>
struct Shard {
    // public-like (used by outer class)
    std::vector<uint32_t> table; // node ids
    std::vector<uint8_t>  ctrl;  // control bytes (tag or EMPTY/TOMB)
    size_t mask = 0;

    std::vector<Node<K, V>> nodes;
    std::vector<uint32_t>   freelist;

    TimerWheel wheel;

    // writer mutex protects mutations; readers use seqlock (epoch)
    mutable std::mutex writer_mutex;
    std::atomic<uint64_t> epoch{0};

    uint32_t tick_val = 1;

    // Constants accessible here
    static constexpr uint32_t INVALID_NODE_LOCAL = INVALID_NODE;

    // init: node_capacity = exact number of nodes per shard
    void init(size_t node_capacity, uint32_t tick) {
        assert(node_capacity > 0);
        nodes.resize(node_capacity);
        freelist.clear();
        freelist.reserve(node_capacity);
        // push node ids in decreasing order for cache friendliness on alloc
        for (uint32_t i = static_cast<uint32_t>(node_capacity); i-- > 0;) freelist.push_back(i);

        // table size ~ 2 * node_capacity => load ~ 0.5
        size_t tsz = 1;
        while (tsz < (node_capacity * 2)) tsz <<= 1;
        table.assign(tsz, INVALID_NODE);
        ctrl.assign(tsz, CTRL_EMPTY);
        mask = tsz - 1;

        tick_val = tick;
        wheel.Init(static_cast<uint32_t>(node_capacity), WHEEL_SLOTS, tick_val, UNIT);
    }

    inline uint64_t h64(const K &k) const {
        return static_cast<uint64_t>(std::hash<K>{}(k));
    }

    inline uint32_t idx_from_hash(uint64_t h) const {
        return static_cast<uint32_t>(static_cast<uint32_t>(h) & static_cast<uint32_t>(mask));
    }

    inline uint8_t tag_from_hash(uint64_t h) const {
        uint8_t x = static_cast<uint8_t>((h >> 23) ^ (h >> 7) ^ (h >> 41));
        x = (x & 0x7F) | 1u;
        if (x == CTRL_EMPTY) x ^= 0x11;
        if (x == CTRL_TOMB)  x ^= 0x22;
        if (x == 0) x = 1;
        return x;
    }

    inline uint32_t alloc_node() {
        if (freelist.empty()) return INVALID_NODE_LOCAL;
        uint32_t id = freelist.back();
        freelist.pop_back();
        nodes[id].alive = true;
        return id;
    }

    inline void free_node(uint32_t id) {
        if (id == INVALID_NODE_LOCAL || id >= nodes.size()) return;
        nodes[id].alive = false;
        // don't destruct key/val here; user types will be overwritten when reused
        freelist.push_back(id);
    }

    // Grow nodes pool (double capacity). MUST be called under writer lock.
    void grow_nodes() {
        size_t old = nodes.size();
        size_t target = std::max<size_t>(4, old * 2);
        nodes.resize(target);
        // new nodes must be pushed into freelist
        for (uint32_t i = static_cast<uint32_t>(target); i-- > static_cast<uint32_t>(old);) {
            freelist.push_back(i);
        }
        // wheel capacity must be expanded to match nodes size
        // We can Reinit wheel's metadata (it preserves semantics)
        wheel.Reinit(static_cast<uint32_t>(target));
    }

    // Grow table (rehash). MUST be called under writer lock.
    void grow_table() {
        size_t old_tsz = table.size();
        size_t new_tsz = old_tsz * 2;
        std::vector<uint32_t> new_table(new_tsz, INVALID_NODE_LOCAL);
        std::vector<uint8_t>  new_ctrl(new_tsz, CTRL_EMPTY);
        size_t new_mask = new_tsz - 1;

        // reinsert all alive nodes
        for (uint32_t nid = 0; nid < nodes.size(); ++nid) {
            if (!nodes[nid].alive) continue;
            const K &key = nodes[nid].key;
            uint64_t h = h64(key);
            uint8_t tag = tag_from_hash(h);
            size_t idx = static_cast<size_t>(static_cast<uint32_t>(h) & static_cast<uint32_t>(new_mask));
            // robin hood insertion into new arrays (simple linear probe variant)
            size_t pos = idx;
            while (true) {
                if (new_ctrl[pos] == CTRL_EMPTY) {
                    new_ctrl[pos] = tag;
                    new_table[pos] = nid;
                    break;
                }
                pos = (pos + 1) & new_mask;
            }
        }

        table.swap(new_table);
        ctrl.swap(new_ctrl);
        mask = new_mask;
    }

    // Keep writer seqlock API: begin_write sets epoch odd; end_write sets even.
    void begin_write() {
        epoch.fetch_add(1, std::memory_order_acq_rel); // make odd
    }
    void end_write() {
        epoch.fetch_add(1, std::memory_order_acq_rel); // make even
    }

    // ----------------- probing / find helpers -----------------
    // Scalar fallback find_slot for locked/writer codepath
    inline std::pair<size_t,bool> find_slot_simd_locked(const K &key) const {
        uint64_t h = h64(key);
        uint8_t tag = tag_from_hash(h);
        size_t idx = idx_from_hash(h);
        while (true) {
            uint8_t c = ctrl[idx];
            if (c == CTRL_EMPTY) return {idx, false};
            if (c == tag) {
                uint32_t nid = table[idx];
                if (nid != INVALID_NODE_LOCAL && nodes[nid].alive && nodes[nid].key == key) return {idx, true};
            }
            idx = (idx + 1) & mask;
        }
    }

    // Lockless-aware find_slot: attempts a fast vectorized scan over control bytes.
    // This function touches ctrl/table/nodes without locking — it's meant to be used
    // inside a seqlock reader window where the caller ensures epoch didn't change.
    inline std::pair<size_t,bool> find_slot_simd_nolock(const K &key) const {
        uint64_t h = h64(key);
        uint8_t tag = tag_from_hash(h);
        size_t n = ctrl.size();
        size_t start = idx_from_hash(h);

        // For tiny tables just do scalar
        if (n < 32) {
            size_t idx = start;
            while (true) {
                uint8_t c = ctrl[idx];
                if (c == CTRL_EMPTY) return { idx, false };
                if (c == tag) {
                    uint32_t nid = table[idx];
                    if (nid != INVALID_NODE_LOCAL && nodes[nid].alive && nodes[nid].key == key) return { idx, true };
                }
                idx = (idx + 1) & mask;
            }
        }

#if defined(__AVX2__)
        const __m256i v_tag = _mm256_set1_epi8(static_cast<char>(tag));
        const __m256i v_empty = _mm256_set1_epi8(static_cast<char>(CTRL_EMPTY));

        size_t idx = start;
        while (true) {
            size_t remain = n - idx;
            if (remain < 32) {
                // scalar remainder, then wrap
                for (size_t i = 0; i < remain; ++i) {
                    uint8_t c = ctrl[idx];
                    if (c == CTRL_EMPTY) return { idx, false };
                    if (c == tag) {
                        uint32_t nid = table[idx];
                        if (nid != INVALID_NODE_LOCAL && nodes[nid].alive && nodes[nid].key == key) return { idx, true };
                    }
                    idx = (idx + 1) & mask;
                }
                idx = 0;
                continue;
            }

            // prefetch upcoming memory to hide latency
            _mm_prefetch(reinterpret_cast<const char*>(ctrl.data() + ((idx + 64) & mask)), _MM_HINT_T0);
            _mm_prefetch(reinterpret_cast<const char*>(table.data() + ((idx + 64) & mask)), _MM_HINT_T0);

            const uint8_t *ptr = ctrl.data() + idx;
            __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ptr));
            __m256i cmp_tag = _mm256_cmpeq_epi8(chunk, v_tag);
            __m256i cmp_empty = _mm256_cmpeq_epi8(chunk, v_empty);
            int mask_tag = _mm256_movemask_epi8(cmp_tag);
            int mask_empty = _mm256_movemask_epi8(cmp_empty);

            if ((mask_tag | mask_empty) == 0) {
                idx = (idx + 32) & mask;
                continue;
            }

            // determine earliest event in this block
            int first_tag = (mask_tag == 0) ? 32 : __builtin_ctz((unsigned)mask_tag);
            int first_emp = (mask_empty == 0) ? 32 : __builtin_ctz((unsigned)mask_empty);

            if (first_emp < first_tag) {
                // empty earlier -> return empty position
                size_t pos = (idx + static_cast<size_t>(first_emp)) & mask;
                return { pos, false };
            } else {
                // tag earlier or equal: check tag positions (may be collisions)
                int mt = mask_tag;
                int local_off = 0;
                while (mt) {
                    int bit = __builtin_ctz((unsigned)mt);
                    size_t pos = (idx + static_cast<size_t>(bit)) & mask;
                    uint32_t nid = table[pos];
                    if (nid != INVALID_NODE_LOCAL && nodes[nid].alive && nodes[nid].key == key) {
                        return { pos, true };
                    }
                    mt &= ~(1u << bit);
                    local_off = bit + 1;
                }
                // if we reached here none of the tag-hits matched the key; continue after block
                idx = (idx + 32) & mask;
                continue;
            }
        }
#else
        // no AVX2 -> scalar
        size_t idx = start;
        while (true) {
            uint8_t c = ctrl[idx];
            if (c == CTRL_EMPTY) return { idx, false };
            if (c == tag) {
                uint32_t nid = table[idx];
                if (nid != INVALID_NODE_LOCAL && nodes[nid].alive && nodes[nid].key == key) return { idx, true };
            }
            idx = (idx + 1) & mask;
        }
#endif
    }

    // insertion -- robin hood. caller must hold writer lock.
    bool map_insert_robinhood_locked(const K &key, uint32_t nid) {
        if (nid == INVALID_NODE_LOCAL) return false;
        uint64_t hcur = h64(key);
        uint8_t tag = tag_from_hash(hcur);
        size_t table_n = table.size();
        size_t cur_ideal = idx_from_hash(hcur);
        size_t pos = cur_ideal;
        size_t cur_dist = 0;

        uint32_t cur_nid = nid;
        uint8_t cur_tag = tag;

        for (;;) {
            uint8_t c = ctrl[pos];
            if (c == CTRL_EMPTY || c == CTRL_TOMB) {
                ctrl[pos] = cur_tag;
                table[pos] = cur_nid;
                return true;
            }

            // duplicate check
            if (c == cur_tag) {
                uint32_t ex = table[pos];
                if (ex != INVALID_NODE_LOCAL && nodes[ex].alive && nodes[ex].key == key) {
                    return false; // already present
                }
            }

            // compute occupant's ideal to decide swap
            uint32_t occ_nid = table[pos];
            uint64_t hocc = h64(nodes[occ_nid].key);
            size_t occ_ideal = idx_from_hash(hocc);
            size_t occ_dist = (pos + table_n - occ_ideal) & mask;
            cur_dist = (pos + table_n - cur_ideal) & mask;

            if (occ_dist < cur_dist) {
                // kick out occupant
                std::swap(cur_tag, ctrl[pos]);
                std::swap(cur_nid, table[pos]);
                // now cur_ideal becomes occ_ideal
                cur_ideal = occ_ideal;
            }
            pos = (pos + 1) & mask;
        }
    }

    // back-shift delete to keep clusters tight (writer-locked)
    void erase_at_index_locked(size_t hole) {
        size_t n = table.size();
        size_t cur = hole;
        size_t next = (cur + 1) & mask;
        while (true) {
            uint8_t cnext = ctrl[next];
            if (cnext == CTRL_EMPTY) {
                ctrl[cur] = CTRL_EMPTY;
                table[cur] = INVALID_NODE_LOCAL;
                return;
            }
            uint32_t nid = table[next];
            uint64_t h = h64(nodes[nid].key);
            size_t ideal = idx_from_hash(h);
            size_t dist = (next + n - ideal) & mask;
            if (dist == 0) {
                ctrl[cur] = CTRL_EMPTY;
                table[cur] = INVALID_NODE_LOCAL;
                return;
            }
            ctrl[cur] = ctrl[next];
            table[cur] = table[next];
            cur = next;
            next = (next + 1) & mask;
        }
    }

    // erase by key, return node id (writer-locked)
    uint32_t map_erase_by_key_locked(const K &key) {
        auto pr = find_slot_simd_locked(key);
        if (!pr.second) return INVALID_NODE_LOCAL;
        size_t pos = pr.first;
        uint32_t nid = table[pos];
        erase_at_index_locked(pos);
        return nid;
    }

    // core public methods used under writer lock
    bool emplace_locked(const K &key, V&& val, uint32_t ttl) {
        // if freelist empty, grow nodes
        if (freelist.empty()) grow_nodes();
        // if table load is too high (approx load > 0.8), grow table
        size_t used = nodes.size() - freelist.size();
        if (used * 10 > table.size() * 8) grow_table();

        // duplicate check
        auto pr = find_slot_simd_locked(key);
        if (pr.second) return false;

        uint32_t nid = alloc_node();
        if (nid == INVALID_NODE_LOCAL) return false;
        nodes[nid].key = key;
        nodes[nid].val = std::forward<V>(val);
        nodes[nid].alive = true;

        if (!map_insert_robinhood_locked(key, nid)) {
            free_node(nid);
            return false;
        }
        if (ttl > 0) wheel.Schedule(nid, ttl);
        return true;
    }

    void upsert_locked(const K &key, V&& val, uint32_t ttl) {
        // if freelist empty, grow nodes
        if (freelist.empty()) grow_nodes();
        size_t used = nodes.size() - freelist.size();
        if (used * 10 > table.size() * 8) grow_table();

        auto pr = find_slot_simd_locked(key);
        if (pr.second) {
            uint32_t nid = table[pr.first];
            nodes[nid].val = std::forward<V>(val);
            if (ttl > 0) wheel.Schedule(nid, ttl);
            else wheel.Cancel(nid);
            return;
        } else {
            uint32_t nid = alloc_node();
            if (nid == INVALID_NODE_LOCAL) return;
            nodes[nid].key = key;
            nodes[nid].val = std::forward<V>(val);
            nodes[nid].alive = true;
            map_insert_robinhood_locked(key, nid);
            if (ttl > 0) wheel.Schedule(nid, ttl);
        }
    }

    bool get_locked(const K &key, V &out) const {
        auto pr = find_slot_simd_locked(key);
        if (!pr.second) return false;
        uint32_t nid = table[pr.first];
        if (nid == INVALID_NODE_LOCAL) return false;
        if (!nodes[nid].alive) return false;
        out = nodes[nid].val;
        return true;
    }

    bool erase_locked(const K &key) {
        uint32_t nid = map_erase_by_key_locked(key);
        if (nid == INVALID_NODE_LOCAL) return false;
        if (nodes[nid].alive) {
            wheel.Cancel(nid);
            free_node(nid);
        }
        return true;
    }

    bool touch_locked(const K &key, uint32_t ttl) {
        auto pr = find_slot_simd_locked(key);
        if (!pr.second) return false;
        uint32_t nid = table[pr.first];
        if (!nodes[nid].alive) return false;
        if (ttl > 0) wheel.Schedule(nid, ttl);
        else wheel.Cancel(nid);
        return true;
    }

    // expire node (called under writer lock via tick)
    void expire_node_locked(uint32_t nid) {
        if (nid >= nodes.size()) return;
        if (!nodes[nid].alive) return;
        const K &key = nodes[nid].key;
        uint32_t removed = map_erase_by_key_locked(key);
        (void)removed;
        free_node(nid);
    }

    // tick operations (writer locked)
    void tick_once_locked() {
        wheel.Tick(wheel.GetTick() + 1, [&](uint32_t nid){ expire_node_locked(nid); });
    }
    void tick_to_locked(uint64_t now_tick) {
        wheel.Tick(now_tick, [&](uint32_t nid){ expire_node_locked(nid); });
    }

    // expire_node used by writer locked path
    void expire_node(uint32_t nid) {
        expire_node_locked(nid);
    }
}; // end Shard

template<class K, class V, size_t SHARDS = 64,
         uint32_t WHEEL_SLOTS = 4096,
         TimeUnit UNIT = TimeUnit::Seconds>
class TimeoutMap {
    static_assert((SHARDS & (SHARDS - 1)) == 0, "SHARDS must be a power of two");

public:
    explicit TimeoutMap(size_t capacity_per_shard, uint32_t tick_val = 1) {
        assert(capacity_per_shard > 0);
        for (size_t i = 0; i < SHARDS; ++i) shards_[i].init(capacity_per_shard, tick_val);
    }

    // Emplace if absent
    bool Emplace(const K& key, V&& value, uint32_t ttl) {
        auto &s = shard_for(key);
        std::lock_guard<std::mutex> wl(s.writer_mutex);
        // writer enters seqlock odd phase
        s.begin_write();
        bool ok = s.emplace_locked(key, std::forward<V>(value), ttl);
        s.end_write();
        return ok;
    }

    // Insert or update
    void Upsert(const K& key, V&& value, uint32_t ttl) {
        auto &s = shard_for(key);
        std::lock_guard<std::mutex> wl(s.writer_mutex);
        s.begin_write();
        s.upsert_locked(key, std::forward<V>(value), ttl);
        s.end_write();
    }

    // Get value copy (fast lockless reader path with fallback)
    bool Get(const K& key, V &out) const {
        auto &s = shard_for(key);
        // try lockless read a few times
        for (int i = 0; i < 4; ++i) {
            uint64_t before = s.epoch.load(std::memory_order_acquire);
            if (before & 1) { /* writer active */ continue; }
            auto pr = s.find_slot_simd_nolock(key);
            uint64_t after = s.epoch.load(std::memory_order_acquire);
            if (before == after && !(after & 1)) {
                if (!pr.second) return false;
                uint32_t nid = s.table[pr.first];
                if (nid == s.INVALID_NODE_LOCAL) return false;
                if (!s.nodes[nid].alive) return false;
                out = s.nodes[nid].val;
                return true;
            }
        }
        // fallback to locking path
        std::lock_guard<std::mutex> wl(s.writer_mutex);
        // safe under writer lock
        auto pr = s.find_slot_simd_locked(key);
        if (!pr.second) return false;
        uint32_t nid = s.table[pr.first];
        if (nid == s.INVALID_NODE_LOCAL) return false;
        if (!s.nodes[nid].alive) return false;
        out = s.nodes[nid].val;
        return true;
    }

    // Erase
    bool Erase(const K& key) {
        auto &s = shard_for(key);
        std::lock_guard<std::mutex> wl(s.writer_mutex);
        s.begin_write();
        bool ok = s.erase_locked(key);
        s.end_write();
        return ok;
    }

    // Touch (reschedule)
    bool Touch(const K& key, uint32_t ttl) {
        auto &s = shard_for(key);
        std::lock_guard<std::mutex> wl(s.writer_mutex);
        s.begin_write();
        bool ok = s.touch_locked(key, ttl);
        s.end_write();
        return ok;
    }

    // Advance one tick on every shard (call from single-threaded tick driver)
    void TickOnce() {
        for (size_t i = 0; i < SHARDS; ++i) {
            auto &s = shards_[i];
            std::lock_guard<std::mutex> wl(s.writer_mutex);
            s.begin_write();
            s.tick_once_locked();
            s.end_write();
        }
    }

    // Advance to a specific tick (batch)
    void TickTo(uint64_t now_tick) {
        for (size_t i = 0; i < SHARDS; ++i) {
            auto &s = shards_[i];
            std::lock_guard<std::mutex> wl(s.writer_mutex);
            s.begin_write();
            s.tick_to_locked(now_tick);
            s.end_write();
        }
    }

private:
    inline Shard<K, V, WHEEL_SLOTS, UNIT> & shard_for(const K &k) {
        size_t i = static_cast<size_t>(std::hash<K>{}(k)) & (SHARDS - 1);
        return shards_[i];
    }

    inline const Shard<K, V, WHEEL_SLOTS, UNIT> & shard_for(const K &k) const {
        size_t i = static_cast<size_t>(std::hash<K>{}(k)) & (SHARDS - 1);
        return shards_[i];
    }

    Shard<K, V, WHEEL_SLOTS, UNIT> shards_[SHARDS];
};

} // namespace WFX::Utils

#endif // WFX_UTILS_TIMEOUT_MAP_HPP

#include <chrono>
#include <thread>
#include <iostream>
#include <cstdint>
#include <cstdlib>

#if defined(_WIN32)
    #include <windows.h>
    #include <psapi.h>
    size_t getMemoryUsageBytes() {
        PROCESS_MEMORY_COUNTERS_EX pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
            return static_cast<size_t>(pmc.WorkingSetSize);
        }
        return 0;
    }
#else
    #include <sys/resource.h>
    #include <unistd.h>
    size_t getMemoryUsageBytes() {
        struct rusage usage;
        if (getrusage(RUSAGE_SELF, &usage) == 0) {
            // ru_maxrss is KB on Linux, bytes on macOS -> normalize to bytes
            #if defined(__APPLE__)
                return static_cast<size_t>(usage.ru_maxrss);
            #else
                return static_cast<size_t>(usage.ru_maxrss) * 1024ULL;
            #endif
        }
        return 0;
    }
#endif

int main(int argc, char** argv) {
    using namespace WFX::Utils;
    using Clock = std::chrono::high_resolution_clock;

    // Config (change on cmdline)
    const size_t N = (argc > 1) ? std::stoull(argv[1]) : 10'000'000ULL;
    constexpr size_t SHARDS = 1;
    constexpr uint32_t WHEEL_SLOTS = 1024 * 4;
    constexpr TimeUnit UNIT = TimeUnit::Milliseconds;
    const uint32_t tick_val = 1;
    const uint32_t ttl = 1;

    std::cout << "Brutal TimeoutMap benchmark: N=" << N
              << ", SHARDS=" << SHARDS
              << ", WHEEL_SLOTS=" << WHEEL_SLOTS
              << ", unit=ms, ttl=" << ttl << "\n";

    size_t mem_before = getMemoryUsageBytes();
    std::cout << "Memory before construction: " << (mem_before / (1024.0*1024.0)) << " MB\n";

    TimeoutMap<uint64_t, uint64_t, SHARDS, WHEEL_SLOTS, UNIT> map(static_cast<size_t>(N), tick_val);

    size_t mem_after_construct = getMemoryUsageBytes();
    std::cout << "Memory after map construction: " << (mem_after_construct / (1024.0*1024.0)) << " MB\n";

    std::cout << "Inserting " << N << " entries...\n";
    auto t0 = Clock::now();
    for (uint64_t i = 1; i <= N; ++i) {
        map.Emplace(i, std::move(i), ttl);
    }
    auto t1 = Clock::now();
    double insert_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    size_t mem_after_insert = getMemoryUsageBytes();
    std::cout << "Insertion of " << N << " keys took " << insert_ms << " ms\n";
    std::cout << "Memory after insertion: " << (mem_after_insert / (1024.0*1024.0)) << " MB\n";

    uint64_t v;
    bool ok1 = map.Get(1, v);
    bool okN = map.Get(N, v);
    std::cout << "Before Tick: key 1 present=" << ok1 << ", key " << N << " present=" << okN << "\n";

    std::cout << "Calling TickOnce() -> evict all...\n";
    auto t2 = Clock::now();
    map.TickOnce();
    auto t3 = Clock::now();
    double tick_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
    std::cout << "TickOnce took " << tick_ms << " ms\n";

    size_t mem_after_tick = getMemoryUsageBytes();
    std::cout << "Memory after TickOnce: " << (mem_after_tick / (1024.0*1024.0)) << " MB\n";

    bool post1 = map.Get(1, v);
    bool postN = map.Get(N, v);
    if (post1 || postN) {
        std::cout << "Keys not fully evicted, advancing ticks...\n";
        auto t4 = Clock::now();
        int extra_ticks = 0;
        while ((post1 || postN) && extra_ticks < 1000) {
            map.TickOnce();
            post1 = map.Get(1, v);
            postN = map.Get(N, v);
            ++extra_ticks;
        }
        auto t5 = Clock::now();
        double extra_ms = std::chrono::duration<double, std::milli>(t5 - t4).count();
        std::cout << "Extra ticks: " << extra_ticks << " in " << extra_ms << " ms\n";
        size_t mem_after_extra = getMemoryUsageBytes();
        std::cout << "Memory after extra ticks: " << (mem_after_extra / (1024.0*1024.0)) << " MB\n";
    }

    std::cout << "Benchmark finished.\n";
    return 0;
}