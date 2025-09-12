/*
 * Build: g++ -O3 -I.
            test/rw_buffer_test.cpp
            utils/rw_buffer/rw_buffer.cpp
            utils/logger/logger.cpp
            utils/buffer_pool/buffer_pool.cpp
 */

#include <chrono>
#include <iostream>
#include <vector>
#include <memory>
#include <random>
#include "utils/rw_buffer/rw_buffer.hpp"
#include "utils/buffer_pool/buffer_pool.hpp"

using namespace WFX::Utils;

constexpr std::size_t NUM_BUFFERS  = 100000;
constexpr std::size_t NUM_ITER     = 10000;
constexpr std::size_t DEFAULT_SIZE = 64;
constexpr std::size_t MAX_SIZE     = 4096;

int main() {
    Logger::GetInstance().SetLevelMask(WFX_LOG_INFO | WFX_LOG_WARNINGS);
    BufferPool pool{1, 1024 * 4, [](std::size_t curSize) { return curSize * 2; }};

    // Use unique_ptr to avoid vector moving RWBuffer objects
    std::vector<std::unique_ptr<RWBuffer>> buffers;
    buffers.reserve(NUM_BUFFERS);
    for (std::size_t i = 0; i < NUM_BUFFERS; ++i) {
        auto buf = std::make_unique<RWBuffer>();
        if (!buf->InitReadBuffer(pool, DEFAULT_SIZE))
            Logger::GetInstance().Fatal("Failed to init read buffer");
        if (!buf->InitWriteBuffer(DEFAULT_SIZE))
            Logger::GetInstance().Fatal("Failed to init write buffer");
        buffers.push_back(std::move(buf));
    }

    std::mt19937 rng(12345);
    std::uniform_int_distribution<std::size_t> dist(1, 128);

    auto start = std::chrono::high_resolution_clock::now();

    for (std::size_t iter = 0; iter < NUM_ITER; ++iter) {
        for (auto& buf : buffers) {
            auto* readMeta = buf->GetReadMeta();
            std::size_t writeLen = dist(rng);

            std::size_t spaceLeft = readMeta->bufferSize - 1 - readMeta->dataLength;
            if (writeLen > spaceLeft) {
                // Try to grow
                if (buf->GrowReadBuffer(DEFAULT_SIZE, MAX_SIZE)) {
                    readMeta = buf->GetReadMeta();
                    spaceLeft = readMeta->bufferSize - 1 - readMeta->dataLength;
                    writeLen = std::min(writeLen, spaceLeft);
                } else {
                    // Cannot grow, clamp
                    writeLen = spaceLeft;
                    if (writeLen == 0) continue; // buffer full
                }
            }

            // Write safely
            char* ptr = buf->GetReadData();
            for (std::size_t i = 0; i < writeLen; ++i)
                ptr[readMeta->dataLength + i] = 'x';

            buf->AdvanceReadLength(static_cast<std::uint32_t>(writeLen));
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    std::cout << "Brutal benchmark finished in " << diff.count() << " seconds\n";

    // After benchmark
    std::size_t errors = 0;
    for (auto& buf : buffers) {
        auto* readMeta = buf->GetReadMeta();
        char* data = buf->GetReadData();

        for (std::size_t i = 0; i < readMeta->dataLength; ++i) {
            if (data[i] != 'x') {
                ++errors;
                break; // one bad per buffer is enough
            }
        }
    }

    if (errors == 0)
        std::cout << "Sanity check passed: all data is correct\n";
    else
        std::cout << "Sanity check failed: " << errors << " buffers corrupted\n";

    return 0;
}