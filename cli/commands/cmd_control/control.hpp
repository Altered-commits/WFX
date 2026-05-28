// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_CLI_COMMANDS_CONTROL_HPP
#define WFX_CLI_COMMANDS_CONTROL_HPP

#include <string>

namespace WFX::CLI {

int ControlCommand(const std::string& subcommand, const std::string& project);

} // namespace WFX::CLI

#endif // WFX_CLI_COMMANDS_CONTROL_HPP