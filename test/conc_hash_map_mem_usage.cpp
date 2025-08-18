/*
 * Build:
 * g++ -std=c++20 -O0 -g -pthread -I. test/conc_hash_map_mem_usage.cpp \
    utils/buffer_pool/buffer_pool.cpp \
    utils/logger/logger.cpp \
    third_party/tlsf/tlsf.c \
    -o map_bench
 */

#include "utils/hash_map/concurrent_map/concurrent_hash_map.hpp"

#include <thread>
#include <atomic>
#include <random>
#include <iostream>
#include <chrono>
#include <vector>
#include <cassert>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#include <sys/resource.h>
#endif

using namespace WFX::Utils;

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

void LeakCheckTestThreaded(
    ConcurrentHashMap<uint64_t, uint64_t>& map,
    std::atomic<bool>& stopFlag,
    int threadId,
    int threadCount,
    int keysPerThread = 10'000
) {
    const uint64_t keyStart = threadId * keysPerThread;
    const uint64_t keyEnd   = keyStart + keysPerThread;

    while (!stopFlag.load()) {
        for (uint64_t key = keyStart; key < keyEnd; ++key) {
            map.Insert(key, key ^ 0xBEEF);
            map.Erase(key);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

int main()
{
    Logger::GetInstance().SetLevelMask(WFX_LOG_INFO);
    ConcurrentHashMap<uint64_t, uint64_t> map(512 * 1024);

    Logger::GetInstance().Info("[main] Map initialized successfully.");
    std::atomic<bool> stopFlag = false;

    const int durationSec = 80;
    const int threadCount = 4;

    std::vector<std::thread> threads;
    for (int i = 0; i < threadCount; ++i) {
        for (int i = 0; i < threadCount; ++i) {
            threads.emplace_back([&, i]() {
                LeakCheckTestThreaded(map, stopFlag, i, threadCount);
            });
        }
    }

    Logger::GetInstance().Info("[main] Starting RSS monitor with ", threadCount, " threads...");

    for (int t = 0; t < durationSec; ++t) {
        std::size_t rss = GetProcessRSS();
        Logger::GetInstance().Info("[t=", t, "s] RSS = ", (rss / (1024.0 * 1024.0)), " MB");
        // std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    stopFlag = true;
    for (auto& th : threads)
        th.join();

    Logger::GetInstance().Info("[DONE] Leak check complete.");
    return 0;
}