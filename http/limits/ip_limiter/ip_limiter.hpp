#ifndef WFX_HTTP_IP_LIMITER_HPP
#define WFX_HTTP_IP_LIMITER_HPP

#include "../base_limiter.hpp"
#include "utils/hash_map/concurrent_map/concurrent_hash_map.hpp"

#include <string>
#include <chrono>
#include <mutex>

namespace WFX::Http {

using namespace WFX::Utils; // For 'ConcurrentHashMap'

class IpLimiter : BaseLimiter {
public:
    static IpLimiter& GetInstance();

    // Called on new connection attempt
    bool AllowConnection(const WFXIpAddress& ip);

    // Called on every request (after conn is accepted)
    bool AllowRequest(const WFXIpAddress& ip);

    // Called when a connection closes
    void ReleaseConnection(const WFXIpAddress& ip);

private:
    IpLimiter()  = default;
    ~IpLimiter() = default;

    IpLimiter(const IpLimiter&) = delete;
    IpLimiter& operator=(const IpLimiter&) = delete;
    IpLimiter(IpLimiter&&) = delete;
    IpLimiter& operator=(IpLimiter&&) = delete;

private:
    struct TokenBucket {
        int tokens = 0;
        std::chrono::steady_clock::time_point lastRefill = std::chrono::steady_clock::now();
    };

    struct IpLimiterEntry {
        int connectionCount = 0;
        TokenBucket bucket;
    };

    ConcurrentHashMap<WFXIpAddress, IpLimiterEntry, 64, 128> ipLimits_;
};

} // namespace WFX::Http

#endif // WFX_HTTP_IP_LIMITER_HPP