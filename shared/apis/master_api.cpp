#include "master_api.hpp"

namespace WFX::Shared {

const MASTER_API_TABLE* GetMasterAPI()
{
    static MASTER_API_TABLE api = {
        GetHttpAPIV1, // From http_api.hpp
    };

    return &api;
}

} // namespace WFX::Shared