/*
 * Build: g++ -O3 -s -I. test/rp_perf_test.cpp utils/crypt/hash.cpp utils/crypt/string.cpp utils/logger/logger.cpp [-lbcrypt]
 */

#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <sstream>
#include <atomic>
#include <cstring>

// Include your RandomPool implementation here
#include "utils/hash/hash.hpp"
#include "utils/diagnostics/logger.hpp"

using namespace WFX::Utils;

constexpr std::size_t kTotalBytes = 100'000'000; // 100MB
constexpr std::size_t kPerThreadChunk = 4096;    // 4KB per GetBytes()
constexpr std::size_t kThreadCount = 8;          // 8 threads

void BenchmarkThread(RandomPool& pool, std::atomic<std::size_t>& counter, std::atomic<bool>& failed) {
    std::vector<std::uint8_t> buffer(kPerThreadChunk);

    while (true) {
        std::size_t current = counter.fetch_add(kPerThreadChunk, std::memory_order_relaxed);
        if (current >= kTotalBytes)
            break;

        if (!pool.GetBytes(buffer.data(), kPerThreadChunk)) {
            failed.store(true, std::memory_order_relaxed);
            GetLogger().Info("Thread failed to get random bytes");
            break;
        }
    }
}

int main() {
    GetLogger().Info("[+] Starting RandomPool benchmark...");

    RandomPool& pool = GetRandomPool();
    std::atomic<std::size_t> counter{0};
    std::atomic<bool> failed{false};

    std::vector<std::thread> threads;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < kThreadCount; ++i) {
        threads.emplace_back(BenchmarkThread, std::ref(pool), std::ref(counter), std::ref(failed));
    }

    for (auto& t : threads) t.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    double seconds = elapsed_us / 1'000'000.0;
    double throughput_MBps = (kTotalBytes / (1024.0 * 1024.0)) / seconds;
    double bytes_per_call = kPerThreadChunk;
    double calls = kTotalBytes / bytes_per_call;
    double ns_per_call = (elapsed_us * 1000.0) / calls;

    std::ostringstream oss;
    oss << "\n[+] RandomPool Performance Benchmark\n"
        << "    Threads            : " << kThreadCount << "\n"
        << "    Total Bytes        : " << kTotalBytes << "\n"
        << "    Time Taken         : " << elapsed_us << " us\n"
        << "    Throughput         : " << throughput_MBps << " MB/s\n"
        << "    Calls (GetBytes)   : " << static_cast<size_t>(calls) << "\n"
        << "    Time per Call      : " << ns_per_call << " ns\n";

    GetLogger().Info(oss.str());

    if (failed.load()) {
        GetLogger().Info("[!] At least one thread failed to get random bytes");
        return 1;
    }

    GetLogger().Info("[+] RandomPool benchmark completed successfully");
    return 0;
}