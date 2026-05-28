// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_UTILS_DOTENV_HPP
#define WFX_UTILS_DOTENV_HPP

#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace WFX::Utils {

using StringMap = std::unordered_map<std::string, std::string>;

enum class EnvFlags : std::uint64_t {
    OVERWRITE_EXISTING = 1ull << 0,
    REQUIRE_OWNER_UID = 1ull << 1,
    REQUIRE_PERMS_600 = 1ull << 2,
    UNLINK_AFTER_LOAD = 1ull << 3,
    MLOCK_BUFFER = 1ull << 4,
};

struct EnvConfig {
    std::uint64_t flags = 0;

    inline bool GetFlag(EnvFlags f) const noexcept
    {
        return (flags & static_cast<std::uint64_t>(f)) != 0;
    }

    inline void SetFlag(EnvFlags f, bool enable = true) noexcept
    {
        const auto bit = static_cast<std::uint64_t>(f);
        flags = enable ? (flags | bit) : (flags & ~bit);
    }
};

class Dotenv {
public:
    static bool LoadFromFile(const std::string& path, const EnvConfig& opts = {});

private: // Helper functions
    static StringMap ParseFromBuffer(const std::vector<char>& buf);
};

} // namespace WFX::Utils

#endif // WFX_UTILS_DOTENV_HPP