// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_UTILS_BITMAP_POOL_HPP
#define WFX_UTILS_BITMAP_POOL_HPP

#include <cstdint>
#include <bit>
#include "utils/diagnostics/logger.hpp"
#include "shared/utils/memory.hpp"

namespace WFX::Utils {

template <typename T> class BitmapPool {
public: // vvv Constructor and Destructor vvv
    BitmapPool(std::uint32_t numSlots, bool exactSize = false)
    {
        // Rounding keeps every bit backed by storage for free, which is what most pools want. It
        // also allocates a full 64 objects for a caller that asked for 8, so a pool of something as
        // heavy as a connection context can opt out with exactSize.
        if(exactSize) {
            // Only the bitmap rounds, so a whole number of 64-bit words still covers every slot
            slots_ = numSlots;
            words_ = std::uint32_t((std::uint64_t(numSlots) + 63) >> 6);
        }
        else {
            // Round up to 64-bit boundary so every allocated bit always maps to valid storage
            // In simpler words_, it rounds to the next 64 divisible number pretty much
            std::uint64_t rounded = std::uint64_t(numSlots) + 63;
            rounded &= ~std::uint64_t(63);

            // Clamp to avoid exceeding valid range
            if(rounded > MAX_64_ALIGNED)
                rounded = MAX_64_ALIGNED;

            slots_ = std::uint32_t(rounded);
            words_ = slots_ >> 6;
        }

        pool_ = Shared::NewArray<T>(slots_);
        bitmap_ = Shared::NewArray<std::uint64_t>(words_);

        // numSlots == 0 is a deliberate empty pool (AllocSlot then always returns nullptr), not a
        // failure: Alloc(0) itself returns nullptr, so only fail loud when slots were actually requested.
        if(slots_ > 0 && (!pool_ || !bitmap_))
            GetLogger().Fatal("[BitmapPool]: Failed to create pools (Allocation returned nullptr)");

        // Reachable only under exactSize, where the last word runs past the end of the pool. Marking
        // that tail taken stops AllocSlot returning a pointer outside it, and costs the scan nothing.
        // The rounded path leaves slots_ a multiple of 64, so this is inert there.
        if(const std::uint32_t tailBits = slots_ & 63; bitmap_ && tailBits != 0)
            bitmap_[words_ - 1] = ~((1ULL << tailBits) - 1);
    }

    ~BitmapPool()
    {
        if(pool_) {
            Shared::DeleteArray(pool_, slots_);
            pool_ = nullptr;
        }
        if(bitmap_) {
            Shared::DeleteArray(bitmap_, words_);
            bitmap_ = nullptr;
        }
    }

    // No copying allowed
    BitmapPool(const BitmapPool&) = delete;
    BitmapPool& operator=(const BitmapPool&) = delete;

    // vvv Move semantics vvv
    BitmapPool(BitmapPool&& other) noexcept
    {
        pool_ = other.pool_;
        bitmap_ = other.bitmap_;
        slots_ = other.slots_;
        words_ = other.words_;
        lastUsedIndex_ = other.lastUsedIndex_;

        other.pool_ = nullptr;
        other.bitmap_ = nullptr;
        other.slots_ = 0;
        other.words_ = 0;
        other.lastUsedIndex_ = 0;
    }

    BitmapPool& operator=(BitmapPool&& other) noexcept
    {
        if(this != &other) {
            Shared::DeleteArray(pool_, slots_);
            Shared::DeleteArray(bitmap_, words_);

            pool_ = other.pool_;
            bitmap_ = other.bitmap_;
            slots_ = other.slots_;
            words_ = other.words_;
            lastUsedIndex_ = other.lastUsedIndex_;

            other.pool_ = nullptr;
            other.bitmap_ = nullptr;
            other.slots_ = 0;
            other.words_ = 0;
            other.lastUsedIndex_ = 0;
        }
        return *this;
    }

public: // vvv Main Functions vvv
    T* AllocSlot()
    {
        std::uint32_t w = lastUsedIndex_;

        // Primary scan: from last index to end
        for(; w < words_; ++w) {
            const std::uint64_t inv = ~bitmap_[w];
            if(inv) {
                const int bit = std::countr_zero(inv);
                bitmap_[w] |= 1ULL << bit;
                lastUsedIndex_ = w;
                return &pool_[(w << 6) + bit];
            }
        }

        // Wrap around scan: from start to old index
        w = 0;
        for(; w < lastUsedIndex_; ++w) {
            const std::uint64_t inv = ~bitmap_[w];
            if(inv) {
                const int bit = std::countr_zero(inv);
                bitmap_[w] |= 1ULL << bit;
                lastUsedIndex_ = w;
                return &pool_[(w << 6) + bit];
            }
        }

        return nullptr; // Fully exhausted
    }

    void FreeSlot(std::uint32_t idx)
    {
        const std::uint32_t w = idx >> 6;
        const std::uint32_t bit = idx & 63;
        bitmap_[w] &= ~(1ULL << bit);
    }

    bool IsAllocated(std::uint32_t idx) const
    {
        const std::uint32_t w = idx >> 6;
        const std::uint32_t bit = idx & 63;
        return (bitmap_[w] & (1ULL << bit)) != 0;
    }

    std::uint32_t GetSlots()
    {
        return slots_;
    }
    std::uint32_t GetIndex(T* ptr)
    {
        return static_cast<std::uint32_t>(ptr - pool_);
    }
    T* GetPtr(std::uint32_t idx)
    {
        return &pool_[idx];
    }

private: // Constexpr stuff
    // Maximum valid 64-aligned slot count for std::uint32_t
    constexpr static std::uint32_t MAX_64_ALIGNED = 0xFFFF'FFC0u;

private: // Storage
    T* pool_{nullptr};
    std::uint64_t* bitmap_{nullptr};
    std::uint32_t slots_{0};
    std::uint32_t words_{0};
    std::uint32_t lastUsedIndex_{0};
};

} // namespace WFX::Utils

#endif // WFX_UTILS_BITMAP_POOL_HPP