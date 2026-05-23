#ifndef WFX_SHARED_MASTER_API_HPP
#define WFX_SHARED_MASTER_API_HPP

#include "shared/apis/http_api.hpp"
#include "shared/apis/async_api.hpp"
#include "shared/apis/memory_api.hpp"
#include "shared/apis/utils_api.hpp"

namespace WFX::Shared {

// vvv Master table to be injected into user dll vvv
struct MASTER_API_TABLE {
    const HTTP_API_EXT1*   (*GetHttpAPIExt1)();
    const ASYNC_API_EXT1*  (*GetAsyncAPIExt1)();
    const MEMORY_API_EXT1* (*GetMemoryAPIExt1)();
    const UTILS_API_EXT1*  (*GetUtilsAPIExt1)();
};
static_assert(std::is_standard_layout<MASTER_API_TABLE>::value, "'MASTER_API_TABLE' must be standard layout");

// vvv Hardcoded signature to inject API table to user side vvv
using RegisterMasterAPIFn = void (*)(const MASTER_API_TABLE*);

// vvv Getter vvv
const MASTER_API_TABLE* GetMasterAPI();

} // namespace WFX::Shared

#endif // WFX_SHARED_MASTER_API_HPP 