// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "http_master_state.hpp"

namespace WFX::Http {

static WFXMasterState __GlobalMasterState;

WFXMasterState& GetMasterState() noexcept
{
    return __GlobalMasterState;
}

} // namespace WFX::Http