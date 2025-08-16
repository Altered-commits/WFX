/*
 * Build: g++ -O3 -I. -pthread test/buffer_pool_test.cpp\
                                utils/buffer_pool/buffer_pool.cpp\
                                utils/logger/logger.cpp\
                                third_party/tlsf/tlsf.c\
                                -o alloc_bench
 */

#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <random>

#include "../utils/buffer_pool/buffer_pool.hpp"

using namespace WFX::Utils;

static void test_basic_lease_release(BufferPool& pool) {
    std::cout << "[TEST] Basic Lease & Release\n";

    void* buf1 = pool.Lease(128);
    assert(buf1 != nullptr);
    std::memset(buf1, 0xAA, 128);
    pool.Release(buf1);

    void* buf2 = pool.Lease(128);
    assert(buf2 != nullptr);
    std::memset(buf2, 0xBB, 128);
    pool.Release(buf2);

    std::cout << "[+] Passed basic lease/release\n";
}

static void test_reacquire(BufferPool& pool) {
    std::cout << "[TEST] Reacquire (resize) functionality\n";

    void* buf = pool.Lease(64);
    assert(buf != nullptr);
    std::memset(buf, 0x11, 64);

    void* bigger = pool.Reacquire(buf, 256); // resize up
    assert(bigger != nullptr);
    // old contents preserved?
    for (size_t i = 0; i < 64; ++i) {
        assert(((unsigned char*)bigger)[i] == 0x11);
    }

    std::memset(bigger, 0x22, 256);
    pool.Release(bigger);

    std::cout << "[+] Passed reacquire upsize test\n";
}

static void test_reacquire_downsize(BufferPool& pool) {
    std::cout << "[TEST] Reacquire downsize\n";

    void* buf = pool.Lease(512);
    assert(buf != nullptr);
    std::memset(buf, 0x33, 512);

    void* smaller = pool.Reacquire(buf, 128); // resize down
    assert(smaller != nullptr);
    // first 128 bytes preserved
    for (size_t i = 0; i < 128; ++i) {
        assert(((unsigned char*)smaller)[i] == 0x33);
    }

    pool.Release(smaller);
    std::cout << "[+] Passed reacquire downsize test\n";
}

static void stress_test_multithread(BufferPool& pool, int threads, int ops) {
    std::cout << "[TEST] Multithreaded lease/release stress test: " << threads << " threads, " << ops << " ops\n";
    std::atomic<bool> startFlag{false};

    auto worker = [&](int tid) {
        std::mt19937 rng(tid);
        std::uniform_int_distribution<size_t> sizeDist(16, 1024);

        while (!startFlag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (int i = 0; i < ops; ++i) {
            size_t sz = sizeDist(rng);
            void* ptr = pool.Lease(sz);
            assert(ptr != nullptr);
            std::memset(ptr, tid, sz);

            if (i % 3 == 0) { // occasional resize
                size_t newSz = sizeDist(rng);
                ptr = pool.Reacquire(ptr, newSz);
                assert(ptr != nullptr);
            }

            pool.Release(ptr);
        }
    };

    std::vector<std::thread> th;
    th.reserve(threads);
    for (int i = 0; i < threads; ++i) {
        th.emplace_back(worker, i + 1);
    }
    startFlag.store(true, std::memory_order_release);

    for (auto& t : th) {
        t.join();
    }

    std::cout << "[+] Passed multithread stress test\n";
}

int main() {
    BufferPool pool(/*shardCount*/ 4, /*initialSize*/ 4096);

    test_basic_lease_release(pool);
    test_reacquire(pool);
    test_reacquire_downsize(pool);
    stress_test_multithread(pool, 8, 100000);

    std::cout << "[+] All tests passed\n";
    return 0;
}