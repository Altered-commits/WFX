#ifndef WFX_INC_CORE_HPP
#define WFX_INC_CORE_HPP

#include "shared/apis/master_api.hpp"

// vvv SHARED MACRO HELPERS vvv
#define WFX_CONCAT_INNER(a, b) a##b
#define WFX_CONCAT(a, b) WFX_CONCAT_INNER(a, b)

namespace WFX::Core {

inline const Shared::MASTER_API_TABLE* __WFXApi = nullptr;

// Master
inline void SetMasterApi(const Shared::MASTER_API_TABLE* api) noexcept { __WFXApi = api; }
inline const Shared::MASTER_API_TABLE* MasterApi() noexcept { return __WFXApi; }

// Ext1
inline const Shared::HTTP_API_EXT1*   HttpApiExt1()   noexcept { return __WFXApi->GetHttpAPIExt1();   }
inline const Shared::ASYNC_API_EXT1*  AsyncApiExt1()  noexcept { return __WFXApi->GetAsyncAPIExt1();  }
inline const Shared::MEMORY_API_EXT1* MemoryApiExt1() noexcept { return __WFXApi->GetMemoryAPIExt1(); }
inline const Shared::UTILS_API_EXT1*  UtilsApiExt1()  noexcept { return __WFXApi->GetUtilsAPIExt1();  }

} // namespace WFX::Core

#endif // WFX_INC_CORE_HPP