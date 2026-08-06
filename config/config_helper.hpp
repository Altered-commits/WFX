// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_CONFIG_HELPERS_HPP
#define WFX_CONFIG_HELPERS_HPP

#include "utils/diagnostics/logger.hpp"
#include <toml++/toml.hpp>
#include <string>
#include <vector>

namespace WFX::Core::ConfigHelpers {

// vvv Helper Helper Functions vvv
inline toml::node_view<const toml::node> ResolveTomlPath(const toml::table& tbl, const char* section)
{
    toml::node_view<const toml::node> node{tbl};

    const char* p = section;
    const char* segmentStart = p;

    while(true) {
        // Find the next '.' or '\0'
        while(*p != '\0' && *p != '.')
            ++p;

        const std::string_view key(segmentStart, static_cast<size_t>(p - segmentStart));

        node = node[key];
        if(!node || !node.is_table())
            return {}; // Invalid path or missing table

        if(*p == '\0')
            break; // Reached end of string

        // Skip '.', next segment starts after it
        ++p;
        segmentStart = p;
    }

    return node;
}

// vvv Helper Functions vvv
// 'fatal' is the only thing distinguishing what used to be two near-identical functions per shape
template <typename T>
bool ExtractValue(const toml::table& tbl, const char* section, const char* field, T& target, bool fatal = false)
{
    auto node = ResolveTomlPath(tbl, section);
    if(node && node.is_table()) {
        if(auto val = node[field].value<T>()) {
            target = *val;
            return true;
        }
    }

    if(fatal)
        Utils::GetLogger().Fatal("[Config]: Missing or invalid entry: [", section, "] ", field, '.');
    else
        Utils::GetLogger().Warn("[Config]: Missing or invalid entry: [", section, "] ", field,
                                ". Using default value: ", target);
    return false;
}

// A non-string element inside the array is always fatal regardless of 'fatalIfMissing', that
// case is a genuine syntax error, not an absent-and-therefore-optional field.
inline void ExtractStringArray(const toml::table& tbl, const char* section, const char* field,
                               std::vector<std::string>& target, bool fatalIfMissing = false)
{
    auto arr = tbl[section][field].as_array();
    if(!arr) {
        if(fatalIfMissing)
            Utils::GetLogger().Fatal("[Config]: Missing or invalid array: [", section, "] ", field, '.');
        return;
    }

    std::vector<std::string> parsed;
    for(const auto& val : *arr) {
        if(auto s = val.value<std::string>())
            parsed.emplace_back(*s);
        else
            Utils::GetLogger().Fatal("[Config]: Non-string value in [", section, "] ", field, " array");
    }

    target = std::move(parsed);
}

} // namespace WFX::Core::ConfigHelpers

#endif // WFX_CONFIG_HELPERS_HPP