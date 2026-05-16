#ifndef WFX_INC_CORE_HPP
#define WFX_INC_CORE_HPP

#include "shared/apis/master_api.hpp"

// vvv SHARED MACRO HELPERS vvv
#define WFX_CONCAT_INNER(a, b) a##b
#define WFX_CONCAT(a, b) WFX_CONCAT_INNER(a, b)

namespace WFX::Core {

inline const Shared::MASTER_API_TABLE* __WFXApi = nullptr;

inline void SetMasterApi(const Shared::MASTER_API_TABLE* api) noexcept { __WFXApi = api; }

inline const Shared::MASTER_API_TABLE* MasterApi() noexcept { return __WFXApi; }
inline const Shared::HTTP_API_TABLE*   HttpApi()   noexcept { return __WFXApi->GetHttpAPIV1();   }
inline const Shared::ASYNC_API_TABLE*  AsyncApi()  noexcept { return __WFXApi->GetAsyncAPIV1();  }
inline const Shared::MEMORY_API_TABLE* MemoryApi() noexcept { return __WFXApi->GetMemoryAPIV1(); }

} // namespace WFX::Core

#endif // WFX_INC_CORE_HPP