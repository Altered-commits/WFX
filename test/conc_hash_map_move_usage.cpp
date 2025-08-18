/*
 * Build: g++ -O3 -s -I. test/conc_hash_map_move_usage.cpp\
                         utils/logger/logger.cpp\
                         utils/buffer_pool/buffer_pool.cpp\
                         third_party/tlsf/tlsf.c\
                -o conc_bench
 */

#include "utils/hash_map/concurrent_map/concurrent_hash_map.hpp"
#include "utils/backport/move_only_function.hpp"

#include <memory>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
size_t getProcessMemoryUsageKB() {
    PROCESS_MEMORY_COUNTERS_EX memInfo;
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&memInfo, sizeof(memInfo));
    return memInfo.PrivateUsage / 1024;
}
#else
#include <unistd.h>
#include <fstream>
size_t getProcessMemoryUsageKB() {
    long rss = 0L;
    std::ifstream statm("/proc/self/statm");
    long page_size_kb = sysconf(_SC_PAGE_SIZE) / 1024;
    statm >> rss >> rss;
    return rss * page_size_kb;
}
#endif

using namespace WFX::Utils;

int main() {
    Logger::GetInstance().SetLevelMask(WFX_LOG_NONE);

    constexpr size_t NUM_OPS = 1'000;
    constexpr size_t BUF_SIZE = 4096;
    constexpr int NUM_ROUNDS = 10;

    using Callback = MoveOnlyFunction<void()>;

    std::cout << "===== MoveOnlyFunction Memory Reuse Benchmark =====\n";

    size_t baseMem = getProcessMemoryUsageKB();
    std::cout << "Initial memory   : " << baseMem << " KB\n";

    for (int round = 1; round <= NUM_ROUNDS; ++round) {
        ConcurrentHashMap<int, Callback, 64, 1024> map;

        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < NUM_OPS; ++i) {
            int socket = i;
            auto buf = std::make_unique<char[]>(BUF_SIZE);
            auto str = std::make_unique<std::string>("Callback " + std::to_string(i));

            Callback fn = [socket, buf = std::move(buf), str = std::move(str)]() {
                if (socket >= 0) {
                    std::memset(buf.get(), 'A', BUF_SIZE);
                    (*str) += " processed";
                    buf[0] = 'Z';
                }
            };

            map.Emplace(i, std::move(fn));
        }

        for (int i = 0; i < NUM_OPS; ++i) {
            auto* cb = map.Get(i);
            if (cb && *cb)
                (*cb)();
        }

        size_t erased = 0;
        for (int i = 0; i < NUM_OPS; ++i) {
            if (map.Erase(i))
                ++erased;
        }

        auto end = std::chrono::high_resolution_clock::now();
        size_t currentMem = getProcessMemoryUsageKB();

        std::cout << "[Round " << round << "] Memory used: " << (currentMem - baseMem) << " KB, ";
        std::cout << "Time: " << std::chrono::duration<double, std::milli>(end - start).count() << " ms, ";
        std::cout << "Erased: " << erased << " keys\n";
    }

    size_t finalMem = getProcessMemoryUsageKB();
    std::cout << "Final memory     : " << finalMem << " KB\n";
    std::cout << "Net memory delta : " << (finalMem - baseMem) << " KB\n";

    return 0;
}