#include "http_master_state.hpp"

namespace WFX::Http {

static WFXMasterState __GlobalMasterState;

WFXMasterState& GetMasterState() noexcept
{
    return __GlobalMasterState;
}

} // namespace WFX::Http