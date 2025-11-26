/*
 * Build: g++ -O3 -march=native -I. -flto test/timer_heap_test.cpp\
            utils/buffer_pool/buffer_pool.cpp\
            utils/timer/timer_heap/timer_heap.cpp\
            utils/logger/logger.cpp\
            utils/math/math.cpp\
            -o timer_bench

 * BUT because this also relies on tlsf, which is in cmake build files, i gotta manually find header and lib
 * for me its:
 *      ./<cmake-build-folder-name>/_deps/tlsf-src/ for header
 *      ./<cmake-build-folder-name>/libtlsf.a       for lib
 * 
 * So gotta add the -I./<cmake-build-folder-name>/_deps/tlsf-src/ and ./<cmake-build-folder-name>/libtlsf.a in the above command
 * 
 * final command for me: g++ -O3 -march=native -I. -I./BuildGCC/_deps/tlsf-src/ -flto test/timer_heap_test.cpp utils/buffer_pool/buffer_pool.cpp utils/timer/timer_heap/timer_heap.cpp utils/logger/logger.cpp utils/math/math.cpp ./BuildGCC/libtlsf.a -o timer_bench
 */

/* BEST TIME
[09:32:48.844] [INFO] [BufferPool]: Created initial pool of size: 524288000 bytes
=== PURE INSERT STORM ===
Insert time: 1382ms
=== CHURN (INSERT + REMOVE) ===
Churn time: 297ms, removed=495762
=== EXPIRATION BURSTS ===
Infinite loop prevented!
[Burst 1] expired=10000001, heap size=604237
[Burst 2] expired=10604238, heap size=100000
Infinite loop prevented!
[Burst 3] expired=20604239, heap size=199999
Infinite loop prevented!
[Burst 4] expired=30604240, heap size=299999
Infinite loop prevented!
[Burst 5] expired=40604241, heap size=399999
Burst phase time: 725ms
=== DONE ===
[09:32:51.386] [INFO] [BufferPool]: Cleanup complete

FOR:
    constexpr size_t N_INSERT = 10'000'000;  // initial insert
    constexpr size_t N_CHURN  = 1'000'000;   // insert + remove
    constexpr size_t N_BURSTS = 5;           // expiration bursts
    constexpr size_t BURST_REFILL = 100'000; // refill per burst
*/

#include "../utils/timer/timer_heap/timer_heap.hpp"
#include "../utils/buffer_pool/buffer_pool.hpp"

#include <chrono>
#include <iostream>
#include <random>
#include <vector>
#include <cassert>

using namespace WFX::Utils;

static inline uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

int main() {
    BufferPool& pool = BufferPool::GetInstance();
    pool.Init(1024 * 1024 * 500, [](std::size_t cs){ return cs * 1.2; }); // 500MB pool

    TimerHeap heap(pool);

    constexpr size_t N_INSERT = 10'000'000; // initial insert
    constexpr size_t N_CHURN  = 1'000'000;   // insert + remove
    constexpr size_t N_BURSTS = 5;         // expiration bursts
    constexpr size_t BURST_REFILL = 100'000;// refill per burst

    std::vector<uint64_t> ids;
    ids.reserve(N_INSERT);

    std::mt19937_64 rng(1337);
    std::uniform_int_distribution<uint64_t> delayDist(1, 5000);
    std::uniform_int_distribution<uint64_t> pickDist(0, 100);

    std::cout << "=== PURE INSERT STORM ===\n";
    auto t0 = now_ms();
    for (size_t i = 0; i < N_INSERT; i++) {
        uint64_t id = i + 1;
        uint64_t delay = delayDist(rng);
        if (i % 13 == 0) delay = 1; // clustered pathology
        heap.Insert(id, delay, 1);
        ids.push_back(id);
    }
    auto t1 = now_ms();
    std::cout << "Insert time: " << (t1 - t0) << "ms\n";

    std::cout << "=== CHURN (INSERT + REMOVE) ===\n";
    size_t deleted = 0;
    t0 = now_ms();
    for (size_t i = 0; i < N_CHURN; i++) {
        uint64_t id = N_INSERT + i + 1;
        uint64_t delay = delayDist(rng);
        heap.Insert(id, delay, 1);

        if (pickDist(rng) < 50 && !ids.empty()) {
            size_t victimIdx = rng() % ids.size();
            uint64_t victim = ids[victimIdx];
            if (heap.Remove(victim)) deleted++;
            ids[victimIdx] = id;
        }
    }
    t1 = now_ms();
    std::cout << "Churn time: " << (t1 - t0) << "ms, removed=" << deleted << "\n";

    std::cout << "=== EXPIRATION BURSTS ===\n";
    size_t expired = 0;
    uint64_t now = 0;
    t0 = now_ms();

    for (size_t b = 0; b < N_BURSTS; b++) {
        now += 5000; // jump ahead

        size_t safety = 0;
        uint64_t outData;
        while (heap.PopExpired(now, outData)) {
            expired++;
            safety++;
            if (safety > 10'000'000) { // railguard
                std::cerr << "Infinite loop prevented!\n";
                break;
            }
        }

        // Degenerate refill: huge cluster of timers all in future relative to now
        for (size_t i = 0; i < BURST_REFILL; i++) {
            uint64_t id = (rng() << 32) ^ rng();
            heap.Insert(id, now + delayDist(rng), 1);
        }

        std::cout << "[Burst " << b+1 << "] expired=" << expired
                  << ", heap size=" << heap.Size() << "\n";
    }

    t1 = now_ms();
    std::cout << "Burst phase time: " << (t1 - t0) << "ms\n";
    std::cout << "=== DONE ===\n";
}