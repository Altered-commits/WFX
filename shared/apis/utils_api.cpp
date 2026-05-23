#include "utils_api.hpp"
#include "http/response/http_response.hpp"
#include "utils/diagnostics/logger.hpp"

namespace WFX::Shared {

// vvv Helper Functions vvv
static Http::HttpResponse* ToRes(void* backend) { return static_cast<Http::HttpResponse*>(backend); }

// vvv Main Shit vvv
const UTILS_API_EXT1* GetUtilsAPIExt1()
{
    static UTILS_API_EXT1 __GlobalUtilsAPIExt1 = {
        // vvv Logging vvv
        [](const char* m) { Utils::GetLogger().Trace(m); },
        [](const char* m) { Utils::GetLogger().Debug(m); },
        [](const char* m) { Utils::GetLogger().Info(m);  },
        [](const char* m) { Utils::GetLogger().Warn(m);  },
        [](const char* m) { Utils::GetLogger().Error(m); },
        [](const char* m) { Utils::GetLogger().Fatal(m); },

        // vvv Prometheus vvv
        [](void* backend) {
            // This function assumes that status and headers have been written beforehand
            // It only writes body data
            // Fatal if some other function calls 'Commit' beforehand
            Utils::GetLogger().PrometheusFlush(
                backend,
                [](void* ctx, const char* data, std::uint32_t len) {
                    ToRes(ctx)->WriteBodyData(std::string_view{data, len});
                }
            );
        }
    };

    return &__GlobalUtilsAPIExt1;
}

} // namespace WFX::Shared