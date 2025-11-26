#ifndef WFX_CONFIG_HELPERS_HPP
#define WFX_CONFIG_HELPERS_HPP

#include "utils/logger/logger.hpp"
#include <toml++/toml.hpp>
#include <string>
#include <vector>

namespace WFX::Core::ConfigHelpers {

using WFX::Utils::Logger;

// vvv Helper Helper Functions vvv
toml::node_view<const toml::node> ResolveTomlPath(const toml::table& tbl, const char* section)
{
    toml::node_view<const toml::node> node{tbl};

    const char* p = section;
    const char* segment_start = p;

    while(true) {
        // Find the next '.' or '\0'
        while(*p != '\0' && *p != '.')
            ++p;

        std::string_view key(segment_start,
                             static_cast<size_t>(p - segment_start));

        node = node[key];
        if(!node || !node.is_table())
            return {}; // Invalid path or missing table

        if(*p == '\0')
            break;      // Reached end of string

        // Skip '.', next segment starts after it
        ++p;
        segment_start = p;
    }

    return node;
}

// vvv Helper Functions vvv
template<typename T>
bool ExtractValue(
    const toml::table& tbl, const char* section, const char* field, T& target
)
{
    auto node = ResolveTomlPath(tbl, section);
    if(node && node.is_table()) {
        if(auto val = node[field].value<T>()) {
            target = *val;
            return true;
        }
    }

    Logger::GetInstance().Warn(
        "[Config]: Missing or invalid entry: [", section, "] ", field,
        ". Using default value: ", target
    );
    return false;
}

template<typename T>
void ExtractValueOrFatal(
    const toml::table& tbl, const char* section, const char* field, T& target
)
{
    auto node = ResolveTomlPath(tbl, section);
    if(node && node.is_table()) {
        if(auto val = node[field].value<T>()) {
            target = *val;
            return;
        }
    }

    Logger::GetInstance().Fatal(
        "[Config]: Missing or invalid entry: [", section, "] ", field, '.'
    );
}

template<typename T>
bool ExtractAutoOrAll(
    const toml::table& tbl, const char* section, const char* field,
    T& target, const T& autoValue, const T& allValue
)
{
    auto& logger = Logger::GetInstance();

    if(auto val = tbl[section][field].value<T>()) {
        target = *val;
        return true;
    }

    if(auto str = tbl[section][field].value<std::string>()) {
        if(*str == "auto")
            target = autoValue;
        else if (*str == "all")
            target = allValue;
        else
            logger.Warn("[Config]: Invalid keyword in [", section, "] ", field,
                        " = ", *str, ". Using default: ", target);
        return true;
    }

    logger.Warn("[Config]: Missing or invalid entry: [", section, "] ", field,
                ". Using default: ", target);
    return false;
}

inline void ExtractStringArrayOrFatal(
    const toml::table& tbl, const char* section,
    const char* field, std::vector<std::string>& target
)
{
    auto& logger = Logger::GetInstance();

    if(auto arr = tbl[section][field].as_array()) {
        target.clear();
        for(const auto& val : *arr) {
            if(val.is_string())
                target.emplace_back(*val.value<std::string>());
            else
                logger.Fatal("[Config]: Non-string value in [", section, "] ", field, " array");
        }
    }
    else
        logger.Fatal("[Config]: Missing or invalid array: [", section, "] ", field, '.');
}

} // namespace WFX::Core::ConfigHelpers

#endif // WFX_CONFIG_HELPERS_HPP