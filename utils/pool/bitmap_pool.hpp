// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_UTILS_BITMAP_POOL_HPP
#define WFX_UTILS_BITMAP_POOL_HPP

#include <cstdint>
#include <bit>
#include "utils/diagnostics/logger.hpp"

namespace WFX::Utils {

template <typename T> class BitmapPool {
public: // vvv Constructor and Destructor vvv
    BitmapPool(std::uint32_t numSlots)
    {
        // Round up to 64-bit boundary so every allocated bit always maps to valid storage
        // In simpler words_, it rounds to the next 64 divisible number pretty much
        std::uint64_t rounded = std::uint64_t(numSlots) + 63;
        rounded &= ~std::uint64_t(63);

        // Clamp to avoid exceeding valid range
        if(rounded > MAX_64_ALIGNED)
            rounded = MAX_64_ALIGNED;

        slots_ = std::uint32_t(rounded);
        words_ = slots_ >> 6;

        pool_ = new T[slots_]{};
        bitmap_ = new std::uint64_t[words_]{0};

        if(!pool_ || !bitmap_)
            GetLogger().Fatal("[BitmapPool]: Failed to create pools (Allocation returned nullptr)");
    }

    ~BitmapPool()
    {
        if(pool_) {
            delete[] pool_;
            pool_ = nullptr;
        }
        if(bitmap_) {
            delete[] bitmap_;
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
            delete[] pool_;
            delete[] bitmap_;

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
            std::uint64_t inv = ~bitmap_[w];
            if(inv) {
                int bit = std::countr_zero(inv);
                bitmap_[w] |= 1ULL << bit;
                lastUsedIndex_ = w;
                return &pool_[(w << 6) + bit];
            }
        }

        // Wrap around scan: from start to old index
        w = 0;
        for(; w < lastUsedIndex_; ++w) {
            std::uint64_t inv = ~bitmap_[w];
            if(inv) {
                int bit = std::countr_zero(inv);
                bitmap_[w] |= 1ULL << bit;
                lastUsedIndex_ = w;
                return &pool_[(w << 6) + bit];
            }
        }

        return nullptr; // Fully exhausted
    }

    void FreeSlot(std::uint32_t idx)
    {
        std::uint32_t w = idx >> 6;
        std::uint32_t bit = idx & 63;
        bitmap_[w] &= ~(1ULL << bit);
    }

    bool IsAllocated(std::uint32_t idx) const
    {
        std::uint32_t w = idx >> 6;
        std::uint32_t bit = idx & 63;
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