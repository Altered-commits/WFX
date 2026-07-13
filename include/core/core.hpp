// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_CORE_HPP
#define WFX_INC_CORE_HPP

#include "shared/apis/master_api.hpp"

// vvv SHARED MACRO HELPERS vvv
#define WFX_CONCAT_INNER(a, b) a##b
#define WFX_CONCAT(a, b) WFX_CONCAT_INNER(a, b)

namespace WFX::Core {

inline const Shared::MasterAPITable* GlobalWFXApi = nullptr;

// Master
inline void SetMasterApi(const Shared::MasterAPITable* api) noexcept
{
    GlobalWFXApi = api;
}
inline const Shared::MasterAPITable* MasterApi() noexcept
{
    return GlobalWFXApi;
}

// Ext1
inline const Shared::HttpAPIExt1* HttpApiExt1() noexcept
{
    return GlobalWFXApi->getHttpAPIExt1();
}
inline const Shared::EndpointAPIExt1* EndpointApiExt1() noexcept
{
    return GlobalWFXApi->getEndpointAPIExt1();
}
inline const Shared::AsyncAPIExt1* AsyncApiExt1() noexcept
{
    return GlobalWFXApi->getAsyncAPIExt1();
}
inline const Shared::MemoryAPIExt1* MemoryApiExt1() noexcept
{
    return GlobalWFXApi->getMemoryAPIExt1();
}
inline const Shared::UtilsAPIExt1* UtilsApiExt1() noexcept
{
    return GlobalWFXApi->getUtilsAPIExt1();
}

} // namespace WFX::Core

#endif // WFX_INC_CORE_HPP