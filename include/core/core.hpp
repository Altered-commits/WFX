#ifndef WFX_INC_HTTP_CORE_HPP
#define WFX_INC_HTTP_CORE_HPP

#include "shared/apis/master_api.hpp"

// Shared extern API table
extern const WFX::Shared::MASTER_API_TABLE* __wfx_api;

// vvv SHARED MACRO HELPERS vvv
#define WFX_CONCAT_INNER(a, b) a##b
#define WFX_CONCAT(a, b) WFX_CONCAT_INNER(a, b)

#endif // WFX_INC_HTTP_CORE_HPP