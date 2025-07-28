#ifndef WFX_SHARED_CONFIG_API_HPP
#define WFX_SHARED_CONFIG_API_HPP

#include "config/config.hpp"

namespace WFX::Shared {

using namespace WFX::Core; // For 'Config'

enum class ConfigAPIVersion : std::uint8_t {
    V1 = 1,
};

// vvv All aliases for clarity vvv
using GetConfigurationFn = Config& (*)();

// vvv API declarations vvv
struct CONFIG_API_TABLE {
    GetConfigurationFn GetConfig;

    ConfigAPIVersion apiVersion;
};

// vvv Getter vvv
const CONFIG_API_TABLE* GetConfigAPIV1();

} // namespace WFX::Shared

#endif // WFX_SHARED_CONFIG_API_HPP