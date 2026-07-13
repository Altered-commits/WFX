// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "master_api.hpp"

namespace WFX::Shared {

const MasterAPITable* GetMasterAPI()
{
    // NOLINTNEXTLINE(readability-identifier-naming) - singleton table, treated as Global variable
    static const MasterAPITable GlobalApi = {
        GetHttpAPIExt1,     // From http_api.hpp
        GetEndpointAPIExt1, // From http_api.hpp
        GetAsyncAPIExt1,    // From async_api.hpp
        GetMemoryAPIExt1,   // From memory_api.hpp
        GetUtilsAPIExt1,    // From utils_api.hpp
    };

    return &GlobalApi;
}

} // namespace WFX::Shared