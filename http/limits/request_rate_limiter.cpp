// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "request_rate_limiter.hpp"

#include "config/config.hpp"

namespace WFX::Http {

using namespace WFX::Core; // For 'Config'

RequestRateLimiter::RequestRateLimiter() : pool_(GetConfig().ipConfig.maxTrackedIdentities)
{
    index_.Init(512);
}

void RequestRateLimiter::Unlink(std::uint32_t idx)
{
    RequestRateLimiterNode* node = pool_.GetPtr(idx);

    if(node->prev != kNoLimiterNode)
        pool_.GetPtr(node->prev)->next = node->next;
    else
        head_ = node->next;

    if(node->next != kNoLimiterNode)
        pool_.GetPtr(node->next)->prev = node->prev;
    else
        tail_ = node->prev;

    node->prev = kNoLimiterNode;
    node->next = kNoLimiterNode;
}

void RequestRateLimiter::LinkFront(std::uint32_t idx)
{
    RequestRateLimiterNode* node = pool_.GetPtr(idx);
    node->prev = kNoLimiterNode;
    node->next = head_;

    if(head_ != kNoLimiterNode)
        pool_.GetPtr(head_)->prev = idx;

    head_ = idx;

    if(tail_ == kNoLimiterNode)
        tail_ = idx;
}

void RequestRateLimiter::Touch(std::uint32_t idx)
{
    if(idx == head_)
        return;

    Unlink(idx);
    LinkFront(idx);
}

RequestRateLimiterNode* RequestRateLimiter::FindOrCreate(const WFXIpAddress& ip, bool& wasCreated)
{
    const WFXIpAddress key = IpUtils::NormalizeIp(ip);
    wasCreated = false;

    if(auto* idxPtr = index_.Get(key)) {
        Touch(*idxPtr);
        return pool_.GetPtr(*idxPtr);
    }

    RequestRateLimiterNode* node = pool_.AllocSlot();
    std::uint32_t idx;

    if(node)
        idx = pool_.GetIndex(node);
    else {
        // Pool is at the tracked-identity cap: evict the least-recently-touched entry that has-
        // -no live connection on it. Walk from the LRU end (tail_) towards the MRU end (head_),-
        // -skipping any entry a connection is still holding open (refCount > 0)
        std::uint32_t victim = tail_;
        while(victim != kNoLimiterNode && pool_.GetPtr(victim)->entry.refCount > 0)
            victim = pool_.GetPtr(victim)->prev;

        if(victim == kNoLimiterNode)
            return nullptr; // Every tracked identity has a live connection right now, nothing to evict

        idx = victim;
        node = pool_.GetPtr(idx);

        Unlink(idx);
        index_.Erase(node->key);
    }

    // Link into the LRU list only once the index actually holds 'idx' under 'key': linking first-
    // -and having the index insert fail (OOM) would leave a node Release() can never find by key-
    // -again, permanently stuck live and unreclaimable
    if(!index_.Insert(key, idx))
        return nullptr;

    node->key = key;
    node->entry = RequestRateLimiterEntry{};

    LinkFront(idx);

    wasCreated = true;
    return node;
}

bool RequestRateLimiter::Acquire(const WFXIpAddress& ip)
{
    bool wasCreated = false;

    RequestRateLimiterNode* node = FindOrCreate(ip, wasCreated);
    if(!node)
        return false; // Cap reached with every tracked identity live, caller must retry later

    // Only a brand-new identity gets a full bucket. An existing identity re-Acquire()'ing after-
    // -its refCount already returned to zero keeps whatever tokens it had left, or every reconnect-
    // -would reset the limiter back to full burst
    if(wasCreated)
        node->entry.bucket.tokens = GetConfig().ipConfig.maxRequestBurstSize;

    ++node->entry.refCount;
    return true;
}

bool RequestRateLimiter::AllowRequest(const WFXIpAddress& ip)
{
    const WFXIpAddress key = IpUtils::NormalizeIp(ip);

    auto* idxPtr = index_.Get(key);
    if(!idxPtr)
        return false; // Never Acquire()'d, or went untracked under cap pressure: deny by default

    Touch(*idxPtr);
    RequestRateLimiterNode* node = pool_.GetPtr(*idxPtr);

    const auto now = std::chrono::steady_clock::now();
    const auto& cfg = GetConfig().ipConfig;
    TokenBucket& bucket = node->entry.bucket;

    const std::int64_t elapsedMs =
        std::max(0L, std::chrono::duration_cast<std::chrono::milliseconds>(now - bucket.lastRefill).count());

    const std::uint64_t refill = (elapsedMs * std::uint64_t(cfg.maxTokensPerSecond)) / 1000ULL;

    if(refill > 0) {
        bucket.lastRefill = now;
        bucket.tokens =
            std::min<std::uint32_t>(cfg.maxRequestBurstSize, bucket.tokens + static_cast<std::uint32_t>(refill));
    }

    if(bucket.tokens > 0) {
        --bucket.tokens;
        return true;
    }

    return false;
}

void RequestRateLimiter::Release(const WFXIpAddress& ip)
{
    auto* idxPtr = index_.Get(IpUtils::NormalizeIp(ip));
    if(!idxPtr)
        return;

    RequestRateLimiterNode* node = pool_.GetPtr(*idxPtr);
    node->entry.refCount -= (node->entry.refCount > 0);

    // No erase-on-refCount-0 here: this entry's bucket must outlive the connection that-
    // -Acquire()'d it, or reconnecting always finds a freshly-seeded bucket and rate limiting-
    // -is bypassed entirely. Capacity is bounded via LRU eviction in FindOrCreate instead, which-
    // -never touches an entry still at refCount > 0
}

} // namespace WFX::Http
