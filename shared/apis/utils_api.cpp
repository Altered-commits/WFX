#include "utils_api.hpp"
#include "utils/logger/logger.hpp"

namespace WFX::Shared {

using Utils::Logger;

const UTILS_API_TABLE* GetUtilsAPIV1()
{
    static UTILS_API_TABLE __GlobalUtilsAPIV1 = {
        // vvv Async Functions vvv
        [](const char* msg) { // LogInfo
            Logger::GetInstance().Info(msg);
        },
        [](const char* msg) { // LogWarn
            Logger::GetInstance().Warn(msg);
        },
        [](const char* msg) { // LogError
            Logger::GetInstance().Error(msg);
        },
        [](const char* msg) { // LogFatal
            Logger::GetInstance().Fatal(msg);
        },

        // Version
        UtilsAPIVersion::V1
    };

    return &__GlobalUtilsAPIV1;
}

} // namespace WFX::Shared