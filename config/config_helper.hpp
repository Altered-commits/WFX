#ifndef WFX_CONFIG_HELPERS_HPP
#define WFX_CONFIG_HELPERS_HPP

#include "utils/logger/logger.hpp"
#include <toml++/toml.hpp>
#include <string>
#include <vector>

namespace WFX::Core::ConfigHelpers {

template<typename T>
bool ExtractValue(const toml::table& tbl, WFX::Utils::Logger& logger,
                  const char* section, const char* field, T& target)
{
    if(auto val = tbl[section][field].value<T>()) {
        target = *val;
        return true;
    }
    logger.Warn("[Config]: Missing or invalid entry: [", section, "] ", field,
                ". Using default value: ", target);
    return false;
}

template<typename T>
void ExtractValueOrFatal(const toml::table& tbl, WFX::Utils::Logger& logger,
                         const char* section, const char* field, T& target)
{
    if(auto val = tbl[section][field].value<T>())
        target = *val;
    else
        logger.Fatal("[Config]: Missing or invalid entry: [", section, "] ", field, '.');
}

template<typename T>
bool ExtractAutoOrAll(const toml::table& tbl, WFX::Utils::Logger& logger,
                      const char* section, const char* field,
                      T& target, const T& autoValue, const T& allValue)
{
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

inline void ExtractStringArrayOrFatal(const toml::table& tbl, WFX::Utils::Logger& logger,
                               const char* section, const char* field,
                               std::vector<std::string>& target)
{
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