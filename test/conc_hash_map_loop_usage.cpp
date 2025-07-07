#include <iostream>
#include <chrono>
#include <string>
#include <random>
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>

#include "utils/hash_map/concurrent_hash_map.hpp"
#include "utils/logger/logger.hpp"

enum class ParseState : uint8_t {
    Idle,
    ParsingHeaders,
    ParsingBody,
    ReadyToClose
};

struct ConnectionContext {
    uint16_t   timeoutTick;
    ParseState parseState;
};

constexpr uint16_t IDLE_TIMEOUT   = 5;
constexpr uint16_t HEADER_TIMEOUT = 3;
constexpr uint16_t BODY_TIMEOUT   = 7;
std::atomic<uint16_t> GlobalTick = 100;

using ConnMap = WFX::Utils::ConcurrentHashMap<uint64_t, ConnectionContext>;

size_t LinearSweepAndErase(ConnMap& map) {
    size_t expired = 0;
    map.ForEachEraseIf([&](ConnectionContext& ctx) -> bool {
        uint16_t elapsed = static_cast<uint16_t>(GlobalTick.load() - ctx.timeoutTick);

        switch (ctx.parseState) {
            case ParseState::Idle:           return (elapsed >= IDLE_TIMEOUT)   && (++expired, true);
            case ParseState::ParsingHeaders: return (elapsed >= HEADER_TIMEOUT) && (++expired, true);
            case ParseState::ParsingBody:    return (elapsed >= BODY_TIMEOUT)   && (++expired, true);
            default: return false;
        }
    });

    return expired;
}

int main() {
    WFX::Utils::Logger::GetInstance().SetLevelMask(WFX::Utils::WFX_LOG_NONE);
    ConnMap connMap;
    constexpr size_t TOTAL          = 5'000'000;
              int WORKER_THREADS    = std::thread::hardware_concurrency();
    constexpr int TEST_DURATION_SEC = 20;

    std::mt19937 rng(1337);
    std::uniform_int_distribution<int> stateDist(0, 2);
    std::uniform_int_distribution<int> offsetDist(0, 10);

    // -----------------------
    // Insert Initial Entries
    // -----------------------
    auto insertStart = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < TOTAL; ++i) {
        ConnectionContext ctx;
        ctx.timeoutTick = GlobalTick.load() - offsetDist(rng);
        ctx.parseState  = static_cast<ParseState>(stateDist(rng));
        connMap.Insert(i, std::move(ctx));
    }
    auto insertEnd = std::chrono::high_resolution_clock::now();
    double insertMs = std::chrono::duration<double, std::milli>(insertEnd - insertStart).count();
    std::cout << "[*] Inserted " << TOTAL << " connections in " << insertMs << " ms\n";

    // -----------------------
    // Benchmark Starts
    // -----------------------
    std::atomic<bool> running = true;
    std::atomic<uint64_t> totalOps = 0;
    std::mutex sweepMutex;
    std::vector<double> sweepTimes;
    std::vector<size_t> sweepExpiredCounts;

    // Simulate IOCP worker threads
    std::vector<std::thread> workers;
    for (int t = 0; t < WORKER_THREADS; ++t) {
        workers.emplace_back([&, t]() {
            std::mt19937 localRng(1000 + t);
            std::uniform_int_distribution<uint64_t> keyDist(0, TOTAL - 1);

            while (running.load()) {
                uint64_t key = keyDist(localRng);
                ConnectionContext updated;
                if (connMap.Get(key, updated)) {
                    updated.timeoutTick++;  // Simulate activity
                    connMap.Update(key, updated);
                }
                ++totalOps;
            }
        });
    }

    // Sweeper thread every 5s
    std::thread sweeper([&]() {
        for (int i = 0; i < TEST_DURATION_SEC / 5; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(5));

            GlobalTick += 5;

            auto start = std::chrono::high_resolution_clock::now();
            size_t expired = LinearSweepAndErase(connMap);
            auto end = std::chrono::high_resolution_clock::now();

            double durationMs = std::chrono::duration<double, std::milli>(end - start).count();

            {
                std::lock_guard<std::mutex> lock(sweepMutex);
                sweepTimes.push_back(durationMs);
                sweepExpiredCounts.push_back(expired);
            }
        }
    });

    // Main thread waits for 20 seconds
    std::this_thread::sleep_for(std::chrono::seconds(TEST_DURATION_SEC));
    running = false;

    // Join workers
    for (auto& w : workers) w.join();
    sweeper.join();

    // -----------------------
    // Results
    // -----------------------
    std::cout << "\n================== RESULTS ==================\n";
    std::cout << "Total worker threads: " << WORKER_THREADS << "\n";
    std::cout << "Total operations by IOCP workers: " << totalOps.load() << "\n";

    for (size_t i = 0; i < sweepTimes.size(); ++i) {
        std::cout << "Sweep #" << (i + 1) << ": " << sweepExpiredCounts[i]
                  << " expired, took " << sweepTimes[i] << " ms\n";
    }

    std::cout << "============================================\n";
    return 0;
}
