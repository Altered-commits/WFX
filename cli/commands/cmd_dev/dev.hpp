#ifndef WFX_CLI_COMMANDS_DEV_HPP
#define WFX_CLI_COMMANDS_DEV_HPP

#include <string>

namespace WFX::CLI {

int RunDevServer(const std::string& host, int port);

}  // namespace WFX::CLI

#endif  // WFX_CLI_COMMANDS_DEV_HPP