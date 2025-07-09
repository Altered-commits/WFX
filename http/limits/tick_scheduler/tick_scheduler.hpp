#ifndef WFX_HTTP_TIMEOUT_LIMITER_HPP
#define WFX_HTTP_TIMEOUT_LIMITER_HPP

#include "utils/hash_map/concurrent_hash_map.hpp"
#include "utils/backport/move_only_function.hpp"

#include <cstdint>
#include <thread>
#include <atomic>
#include <memory>

namespace WFX::Http {

using namespace WFX::Utils; // For 'MoveOnlyFunction'

class TickScheduler {
public:
    using TickType = std::uint16_t;

    TickScheduler()  = default;
    ~TickScheduler() = default;

    void Start(MoveOnlyFunction<void(TickType)> fn);
    void Stop();
    
    TickType GetCurrentTick() const;
    bool     IsExpired(TickType now, TickType then, TickType timeout) const;

private:
    void Run();

    std::atomic<bool> running_{false};
    std::atomic<TickType> tick_{0};
    std::thread worker_;
    MoveOnlyFunction<void(TickType)> callback_;
};

} // namespace WFX::Http

#endif // WFX_HTTP_TIMEOUT_LIMITER_HPP