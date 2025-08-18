#ifndef WFX_SHARED_MASTER_API_HPP
#define WFX_SHARED_MASTER_API_HPP

#include "shared/apis/http_api.hpp"
#include "shared/apis/config_api.hpp"

namespace WFX::Shared {

// vvv Master table to be injected into user dll vvv
struct MASTER_API_TABLE {
    const HTTP_API_TABLE*   (*GetHttpAPIV1)();
    const CONFIG_API_TABLE* (*GetConfigAPIV1)();
};

// vvv Hardcoded signature to inject api table to user side vvv
using RegisterMasterAPIFn = void (*)(const MASTER_API_TABLE*);

// vvv Getter vvv
const MASTER_API_TABLE* GetMasterAPI();

} // namespace WFX::Shared

#endif // WFX_SHARED_MASTER_API_HPP 