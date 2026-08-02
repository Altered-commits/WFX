// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_HTTP_REQUEST_RATE_LIMITER_HPP
#define WFX_HTTP_REQUEST_RATE_LIMITER_HPP

#include "http/connection/http_connection.hpp"
#include "http/limits/ip_utils.hpp"
#include "utils/hash_map/hash_shard.hpp"
#include "utils/pool/bitmap_pool.hpp"

#include <chrono>

namespace WFX::Http {

struct TokenBucket {
    std::uint64_t tokens = 0;
    std::chrono::steady_clock::time_point lastRefill = std::chrono::steady_clock::now();
};

struct RequestRateLimiterEntry {
    // How many currently-open connections are mapped to this resolved IP, mirrors-
    // -ConnectionLimiterEntry's connectionCount but keyed on the resolved (not peer) IP
    std::uint32_t refCount = 0;
    TokenBucket bucket;
};

// Sentinel index meaning "no node", used by both the free-standing pool index and the LRU links
constexpr std::uint32_t K_NO_LIMITER_NODE = 0xFFFFFFFFu;

// Fixed-address pool node (BitmapPool never moves a live slot). 'prev'/'next' thread an intrusive-
// -doubly-linked LRU list by index rather than pointer, since indices stay valid across HashShard-
// -reshuffling while a pointer into HashShard storage would not
struct RequestRateLimiterNode {
    WFXIpAddress key;
    RequestRateLimiterEntry entry;
    std::uint32_t prev = K_NO_LIMITER_NODE;
    std::uint32_t next = K_NO_LIMITER_NODE;
};

// Resolved-IP keyed: token-bucket request rate limiting. Unlike ConnectionLimiter, an entry's-
// -bucket history must survive its owning connection closing, or a client that never reuses a-
// -connection gets a freshly-seeded, full-burst bucket on every single request. So entries are-
// -never erased just because refCount drops to zero: Acquire()/Release() only adjust refCount,-
// -and an entry only disappears via LRU eviction once the tracked-identity cap is hit, which-
// -itself skips any entry still tied to a live connection (refCount > 0)
class RequestRateLimiter {
public:
    RequestRateLimiter();
    ~RequestRateLimiter() = default;

public:
    // Called once per connection, the first time its resolved IP becomes known. False means the-
    // -tracked-identity cap is full of live entries and this identity was NOT tracked: the caller-
    // -must retry on a later request rather than treat this as a one-shot, or Release() below can-
    // -never be paired back up with it
    bool Acquire(const WFXIpAddress& ip);

    // Called on every request once Acquire() has returned true for this connection
    bool AllowRequest(const WFXIpAddress& ip);

    // Called when a connection whose Acquire() call returned true closes. Safe to call on an-
    // -identity Acquire() never actually tracked, it just finds nothing and no-ops
    void Release(const WFXIpAddress& ip);

private:
    RequestRateLimiter(const RequestRateLimiter&) = delete;
    RequestRateLimiter& operator=(const RequestRateLimiter&) = delete;
    RequestRateLimiter(RequestRateLimiter&&) = delete;
    RequestRateLimiter& operator=(RequestRateLimiter&&) = delete;

private:
    // Unhooks 'idx' from the LRU list, patching up 'head_'/'tail_' if it was an endpoint
    void Unlink(std::uint32_t idx);

    // Hooks 'idx' in at the MRU end (head_)
    void LinkFront(std::uint32_t idx);

    // Moves 'idx' to the MRU end, called on every Acquire()/AllowRequest() touch
    void Touch(std::uint32_t idx);

    // Looks up 'ip', or creates a tracked entry for it if the cap allows (evicting the LRU-
    // -non-live entry if the pool is already full). 'wasCreated' distinguishes a brand-new-
    // -entry (bucket needs seeding) from an existing one (bucket must be left alone). Returns-
    // -nullptr only when every tracked identity currently has a live connection on it
    RequestRateLimiterNode* FindOrCreate(const WFXIpAddress& ip, bool& wasCreated);

private:
    Utils::BitmapPool<RequestRateLimiterNode> pool_;
    Utils::HashShard<WFXIpAddress, std::uint32_t> index_; // Resolved IP -> pool_ slot index

    std::uint32_t head_ = K_NO_LIMITER_NODE; // Most-recently-touched entry
    std::uint32_t tail_ = K_NO_LIMITER_NODE; // Least-recently-touched entry, first eviction candidate
};

} // namespace WFX::Http

#endif // WFX_HTTP_REQUEST_RATE_LIMITER_HPP
