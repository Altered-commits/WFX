#ifndef WFX_HTTP_BASE_LIMITER_HPP
#define WFX_HTTP_BASE_LIMITER_HPP

/* This pure header file impl is there for one reason, house commonly used functions in limiters */

#include "http/connection/http_connection.hpp"

#include <chrono>

namespace WFX::Http {

    struct BaseLimiter {
        WFXIpAddress NormalizeIp(const WFXIpAddress& ip)
        {
            WFXIpAddress out = ip;

            if(ip.ipType == AF_INET)
                out.ip.v4.s_addr &= htonl(0xFFFFFF00); // Mask out subnet /24
            else
                memset(&out.ip.v6.s6_addr[8], 0, 8); // Mask out subnet /64

            return out;
        }

        std::uint64_t NowEpochSeconds() const
        {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now().time_since_epoch()
                ).count()
            );
        }
    };
    
} // namespace WFX::Http


#endif // WFX_HTTP_BASE_LIMITER_HPP