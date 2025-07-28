#include "config_api.hpp"

namespace WFX::Shared {

const CONFIG_API_TABLE* GetConfigAPIV1()
{
    static CONFIG_API_TABLE __GlobalConfigAPIV1 = {
        // Instance
        []() -> Config& { return Config::GetInstance(); },

        // Version
        ConfigAPIVersion::V1
    };

    return &__GlobalConfigAPIV1;
}

} // namespace WFX::Shared