#include <iostream>
#include <vector>
#include <thread>
#include <random>
#include <chrono>
#include <algorithm>
#include <atomic>

/*
 * OUTDATED, WON'T COMPILE, BUT U GET THE IDEA :)
 */

#include "utils/hash_map/concurrent_hash_map.hpp"
#include "utils/logger/logger.hpp"

using namespace WFX::Utils;

constexpr size_t TOTAL_KEYS       = 5'000'000; // total entries
constexpr size_t THREAD_COUNT     = 8;         // threads for stress test
constexpr size_t SHARD_COUNT      = 64;        // match your current config [not even going to bother removing this comment]
constexpr size_t BUCKET_COUNT     = 512;

using MapType = ConcurrentHashMap<uint64_t, uint64_t, SHARD_COUNT, BUCKET_COUNT>;

void insert_worker(MapType& map, const std::vector<uint64_t>& keys, size_t start, size_t end) {
    for (size_t i = start; i < end; ++i) {
        map.Emplace(keys[i], keys[i] * 2);
    }
}

void erase_worker(MapType& map, const std::vector<uint64_t>& keys, size_t start, size_t end) {
    for (size_t i = start; i < end; ++i) {
        map.Erase(keys[i]);
    }
}

int main() {
    Logger::GetInstance().SetLevelMask(WFX_LOG_WARNINGS);
    MapType map;
    std::vector<uint64_t> keys(TOTAL_KEYS);

    // Random key generation (no sequential locality)
    std::mt19937_64 rng(12345);
    for (size_t i = 0; i < TOTAL_KEYS; ++i) {
        keys[i] = rng();
    }
    std::shuffle(keys.begin(), keys.end(), rng);

    // Partition work
    size_t per_thread = TOTAL_KEYS / THREAD_COUNT;

    // Insertion benchmark
    auto start_insert = std::chrono::high_resolution_clock::now();
    {
        std::vector<std::thread> threads;
        for (size_t t = 0; t < THREAD_COUNT; ++t) {
            size_t start = t * per_thread;
            size_t end   = (t == THREAD_COUNT - 1) ? TOTAL_KEYS : start + per_thread;
            threads.emplace_back(insert_worker, std::ref(map), std::cref(keys), start, end);
        }
        for (auto& th : threads) th.join();
    }
    auto end_insert = std::chrono::high_resolution_clock::now();

    double insert_time = std::chrono::duration<double>(end_insert - start_insert).count();
    double insert_ops  = TOTAL_KEYS / insert_time;
    std::cout << "Insertion: " << insert_time << " sec, " << insert_ops << " ops/sec\n";

    // Deletion benchmark
    std::shuffle(keys.begin(), keys.end(), rng); // ensure random erase order
    auto start_erase = std::chrono::high_resolution_clock::now();
    {
        std::vector<std::thread> threads;
        for (size_t t = 0; t < THREAD_COUNT; ++t) {
            size_t start = t * per_thread;
            size_t end   = (t == THREAD_COUNT - 1) ? TOTAL_KEYS : start + per_thread;
            threads.emplace_back(erase_worker, std::ref(map), std::cref(keys), start, end);
        }
        for (auto& th : threads) th.join();
    }
    auto end_erase = std::chrono::high_resolution_clock::now();

    double erase_time = std::chrono::duration<double>(end_erase - start_erase).count();
    double erase_ops  = TOTAL_KEYS / erase_time;
    std::cout << "Deletion: " << erase_time << " sec, " << erase_ops << " ops/sec\n";

    return 0;
}