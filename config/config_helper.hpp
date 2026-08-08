// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_CONFIG_HELPERS_HPP
#define WFX_CONFIG_HELPERS_HPP

#include "config.hpp"
#include "utils/diagnostics/logger.hpp"
#include <toml++/toml.hpp>

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

inline void ExtractCors(const toml::table& tbl, CORSConfig& cors)
{
    ExtractValue(tbl, "CORS", "enabled", cors.enabled);
    ExtractValue(tbl, "CORS", "allow_credentials", cors.allowCredentials);

    std::vector<std::string> rawOrigins;
    ExtractStringArray(tbl, "CORS", "allowed_origins", rawOrigins);
    for(auto& origin : rawOrigins) {
        if(origin == "*")
            cors.wildcardOrigin = true;
        else
            cors.allowedOrigins.insert(std::move(origin));
    }

    ExtractValue(tbl, "CORS", "allowed_methods", cors.allowedMethods);

    auto joinCsv = [](const std::vector<std::string>& items) {
        std::string joined;
        for(const auto& item : items) {
            if(!joined.empty())
                joined += ", ";
            joined += item;
        }
        return joined;
    };

    std::vector<std::string> rawAllowedHeaders;
    ExtractStringArray(tbl, "CORS", "allowed_headers", rawAllowedHeaders);
    cors.allowedHeaders = joinCsv(rawAllowedHeaders);

    std::vector<std::string> rawExposedHeaders;
    ExtractStringArray(tbl, "CORS", "exposed_headers", rawExposedHeaders);
    cors.exposedHeaders = joinCsv(rawExposedHeaders);

    std::uint32_t maxAgeSeconds = 600;
    ExtractValue(tbl, "CORS", "max_age", maxAgeSeconds);
    cors.maxAge = std::to_string(maxAgeSeconds);

    // Browsers refuse "*" combined with credentials, catch it at load time
    if(cors.allowCredentials && cors.wildcardOrigin)
        Utils::GetLogger().Fatal("[Config]: [CORS] allow_credentials = true cannot be combined with a '*' entry "
                                 "in allowed_origins, browsers refuse that combination. List exact origins instead");
}

} // namespace WFX::Core::ConfigHelpers

#endif // WFX_CONFIG_HELPERS_HPP