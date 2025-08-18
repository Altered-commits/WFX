/*
 * Build: g++ -O3 -pthread test/alloc_pool_mem_usage.cpp utils/fixed_pool/fixed_pool.cpp -o alloc_bench
 */

#include "../utils/fixed_pool/fixed_pool.hpp"

#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#include <sys/resource.h>
#endif

using namespace WFX::Utils;

// Return Resident Set Size in bytes
std::size_t GetProcessRSS()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
    return static_cast<std::size_t>(pmc.WorkingSetSize);
#else
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return static_cast<std::size_t>(usage.ru_maxrss) * 1024;
#endif
}

// Threaded hammering function
void StressTest(ConfigurableFixedAllocPool& pool, std::atomic<bool>& stopFlag)
{
    std::vector<std::size_t> sizes = { 64, 128, 4096 };
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, sizes.size() - 1);

    std::vector<std::pair<void*, std::size_t>> allocations;
    allocations.reserve(10000);

    while (!stopFlag.load()) {
        // Allocate 1000 blocks
        for (int i = 0; i < 1000; ++i) {
            std::size_t size = sizes[dist(rng)];
            void* ptr = pool.Allocate(size);
            if (ptr) {
                std::memset(ptr, 0xAB, size); // Simulate usage
                allocations.emplace_back(ptr, size);
            }
        }

        // Free all
        for (auto& [ptr, size] : allocations)
            pool.Free(ptr, size);

        allocations.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

int main()
{
    ConfigurableFixedAllocPool pool({ 64, 128, 4096 });
    pool.PreWarmAll(256);

    std::atomic<bool> stopFlag = false;

    const int threadCount = 4;
    const int durationSec = 300;

    std::vector<std::thread> threads;

    std::cout << "Benchmark started with " << threadCount << " threads for "
              << durationSec << " seconds...\n";

    // Launch stress threads
    for (int i = 0; i < threadCount; ++i)
        threads.emplace_back(StressTest, std::ref(pool), std::ref(stopFlag));

    // Memory monitor loop
    for (int t = 0; t < durationSec; ++t) {
        std::size_t rss = GetProcessRSS();
        std::cout << "[t=" << t << "s] RSS = " << (rss / (1024.0 * 1024.0)) << " MB" << std::endl;
        // std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Stop and cleanup
    stopFlag = true;
    for (auto& th : threads)
        th.join();

    std::cout << "[DONE] Benchmark complete.\n";
    std::cin.get();
    return 0;
}