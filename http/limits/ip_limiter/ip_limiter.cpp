#include "ip_limiter.hpp"

// We will use std::min instead of min macro
#undef min

namespace WFX::Http {

using namespace std::chrono;

IpLimiter& IpLimiter::GetInstance()
{
    static IpLimiter instance;
    return instance;
}

bool IpLimiter::AllowConnection(const WFXIpAddress& ip)
{
    return ipLimits_.GetOrInsertWith(NormalizeIp(ip), [](IpLimiterEntry& entry) -> bool {
        if(entry.connectionCount >= MAX_CONNECTIONS)
            return false;

        ++entry.connectionCount;
        return true;
    });
}

bool IpLimiter::AllowRequest(const WFXIpAddress& ip)
{
    const auto now = steady_clock::now();

    return ipLimits_.GetWith(NormalizeIp(ip), [this, &now](IpLimiterEntry& entry) -> bool {
        TokenBucket& bucket = entry.bucket;

        const auto elapsedMs = duration_cast<milliseconds>(now - bucket.lastRefill).count();
        const int  refill    = static_cast<int>(elapsedMs * REFILL_RATE / 1000);

        if(refill > 0) {
            bucket.tokens     = std::min(MAX_TOKENS, bucket.tokens + refill);
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