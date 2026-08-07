// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_UTILS_HASH_HPP
#define WFX_INC_WFX_UTILS_HASH_HPP

// -----------------------------------------------------------------------
// wfx/utils/hash.hpp
// Fast, non-cryptographic hashing (hash tables, cache keys). For cryptographic
// hashing (SHA-2, HMAC, etc.) see wfx/utils/crypto.hpp instead
//
// Provides:
//   WFX::Xxh3(data, seed = 0)                : general-purpose hash, XXH3-64
//   WFX::WyHash(data, seed = 0)              : general-purpose hash
//   WFX::Fnv1a(data) / Fnv1aCaseInsensitive(data)
//   WFX::Murmur3Mix32(x) / Murmur3Mix64(x)   : integer finalizers
//   WFX::HashCombine(a, b)                   : combine two hashes (boost-style)
//   WFX::HashValue(val)                      : hash a trivially copyable value by its bytes
//   WFX::HashInt32(x) / HashInt64(x)
// -----------------------------------------------------------------------

#include "shared/utils/hash.hpp"

#include <cstdint>
#include <string_view>

namespace WFX {

inline std::uint64_t Xxh3(std::string_view data, std::uint64_t seed = 0) noexcept
{
    return Shared::Hasher::Xxh3(data.data(), data.size(), seed);
}

inline std::uint64_t WyHash(std::string_view data, std::uint64_t seed = 0) noexcept
{
    return Shared::Hasher::WyHash(data.data(), data.size(), seed);
}

inline std::uint64_t Fnv1a(std::string_view data) noexcept
{
    return Shared::Hasher::Fnv1a(data.data(), data.size());
}

inline std::uint64_t Fnv1aCaseInsensitive(std::string_view data) noexcept
{
    return Shared::Hasher::Fnv1aCaseInsensitive(data.data(), data.size());
}

using Shared::Hasher::HashCombine;
using Shared::Hasher::HashInt32;
using Shared::Hasher::HashInt64;
using Shared::Hasher::HashValue;
using Shared::Hasher::Murmur3Mix32;
using Shared::Hasher::Murmur3Mix64;

} // namespace WFX

#endif // WFX_INC_WFX_UTILS_HASH_HPP
