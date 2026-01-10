#ifndef WFX_CLI_COMMANDS_BUILD_HPP
#define WFX_CLI_COMMANDS_BUILD_HPP

#include <string>

namespace WFX::CLI {

int BuildProject(const std::string& project, const std::string& buildType, bool isDebug);

}  // namespace WFX::CLI

#endif  // WFX_CLI_COMMANDS_BUILD_HPP