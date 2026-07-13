// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_MASTER_API_HPP
#define WFX_SHARED_MASTER_API_HPP

#include "shared/apis/http_api.hpp"
#include "shared/apis/async_api.hpp"
#include "shared/apis/memory_api.hpp"
#include "shared/apis/utils_api.hpp"

namespace WFX::Shared {

// vvv Master table to be injected into user dll vvv
struct MasterAPITable {
    const HttpAPIExt1* (*getHttpAPIExt1)();
    const EndpointAPIExt1* (*getEndpointAPIExt1)();
    const AsyncAPIExt1* (*getAsyncAPIExt1)();
    const MemoryAPIExt1* (*getMemoryAPIExt1)();
    const UtilsAPIExt1* (*getUtilsAPIExt1)();
};
static_assert(std::is_standard_layout<MasterAPITable>::value, "'MASTER_API_TABLE' must be standard layout");

// vvv Hardcoded signature to inject API table to user side vvv
using RegisterMasterAPIFn = void (*)(const MasterAPITable*);

// vvv Getter vvv
const MasterAPITable* GetMasterAPI();

} // namespace WFX::Shared

#endif // WFX_SHARED_MASTER_API_HPP