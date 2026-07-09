// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_CLI_COMMANDS_BUILD_HPP
#define WFX_CLI_COMMANDS_BUILD_HPP

#include <string>

namespace WFX::CLI {

int BuildProject(const std::string& project, const std::string& buildType);

} // namespace WFX::CLI

#endif // WFX_CLI_COMMANDS_BUILD_HPP