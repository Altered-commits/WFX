#ifndef WFX_HTTP_IP_LIMITER_HPP
#define WFX_HTTP_IP_LIMITER_HPP

#include "../base_limiter.hpp"
#include "utils/hash_map/hash_shard.hpp"

namespace WFX::Http {

using namespace WFX::Utils; // For 'HashShard', 'BufferPool'

class IpLimiter : BaseLimiter {
public:
    IpLimiter(BufferPool& poolRef);
    ~IpLimiter() = default;

public:
    // Called on new connection attempt
    bool AllowConnection(const WFXIpAddress& ip);

    // Called on every request (after conn is accepted)
    bool AllowRequest(const WFXIpAddress& ip);

    // Called when a connection closes
    void ReleaseConnection(const WFXIpAddress& ip);

private:
    IpLimiter(const IpLimiter&) = delete;
    IpLimiter& operator=(const IpLimiter&) = delete;
    IpLimiter(IpLimiter&&) = delete;
    IpLimiter& operator=(IpLimiter&&) = delete;

private:
    struct TokenBucket {
        std::uint64_t tokens = 0;
        std::chrono::steady_clock::time_point lastRefill = std::chrono::steady_clock::now();
    };

    struct IpLimiterEntry {
        std::uint32_t connectionCount = 0;
        TokenBucket bucket;
    };

    HashShard<WFXIpAddress, IpLimiterEntry> ipLimits_;
};

} // namespace WFX::Http

#endif // WFX_HTTP_IP_LIMITER_HPP