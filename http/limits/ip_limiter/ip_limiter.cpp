#include "ip_limiter.hpp"

#include "config/config.hpp"

// We will use std::min instead of min macro
#undef min

namespace WFX::Http {

using namespace WFX::Core;  // For 'Config'
using namespace std::chrono;

IpLimiter& IpLimiter::GetInstance()
{
    static IpLimiter instance;
    return instance;
}

bool IpLimiter::AllowConnection(const WFXIpAddress& ip)
{
    return ipLimits_.GetOrInsertWith(NormalizeIp(ip), [](IpLimiterEntry& entry) -> bool {
        auto& cfg = Config::GetInstance().networkConfig;

        if(entry.connectionCount >= cfg.maxConnectionsPerIp)
            return false;

        // Initialize token bucket on first connection if not already set
        if(entry.connectionCount == 0 && entry.bucket.tokens == 0)
            entry.bucket.tokens = cfg.maxRequestBurstSize;

        ++entry.connectionCount;
        return true;
    });
}

bool IpLimiter::AllowRequest(const WFXIpAddress& ip)
{
    return ipLimits_.GetWith(NormalizeIp(ip), [](IpLimiterEntry& entry) -> bool {
        const auto now = std::chrono::steady_clock::now();
        const auto& cfg = Config::GetInstance().networkConfig;

        TokenBucket& bucket = entry.bucket;

        const std::int64_t elapsedMs  = std::max<std::int64_t>(
            0, std::chrono::duration_cast<std::chrono::milliseconds>(now - bucket.lastRefill).count()
        );
        const std::uint32_t refillRate = cfg.maxTokensPerSecond;
        const std::uint32_t burstCap   = cfg.maxRequestBurstSize;

        const std::uint64_t refill = (elapsedMs * refillRate) / 1000ULL;

        if(refill > 0) {
            bucket.tokens = std::min<std::uint32_t>(
                burstCap,
                bucket.tokens + static_cast<std::uint32_t>(refill)
            );
            bucket.lastRefill = now;
        }

        if(bucket.tokens > 0) {
            --bucket.tokens;
            return true;
        }

        return false;
    });
}

void IpLimiter::ReleaseConnection(const WFXIpAddress& ip)
{
    const WFXIpAddress key = NormalizeIp(ip);
    const bool shouldErase = ipLimits_.GetWith(key, [](IpLimiterEntry& entry) -> bool {
                                return --entry.connectionCount <= 0;
                            });
    if(shouldErase)
        ipLimits_.Erase(key);
}

} // namespace WFX::Http