#include "master_api.hpp"

namespace WFX::Shared {

const MASTER_API_TABLE* GetMasterAPI()
{
    static MASTER_API_TABLE api = {
        GetHttpAPIExt1,    // From http_api.hpp
        GetAsyncAPIExt1,   // From async_api.hpp
        GetMemoryAPIExt1,  // From memory_api.hpp
        GetUtilsAPIExt1,   // From utils_api.hpp
    };

    return &api;
}

} // namespace WFX::Shared