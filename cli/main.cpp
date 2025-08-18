#ifndef WFX_CLI_MAIN_HPP
#define WFX_CLI_MAIN_HPP

#include <iostream>
#include <string>
#include <vector>

#include "commands/cmd_new/new.hpp"
#include "commands/cmd_doctor/doctor.hpp"
#include "commands/cmd_dev/dev.hpp"
#include "utils/argument_parser/argument_parser.hpp"

namespace WFX {

// For argument parser
using namespace WFX::Utils;

int WFXEntryPoint(int argc, char* argv[]) {
    ArgumentParser parser;

    // --- Command: new ---
    parser.AddCommand("new", "Create a new WFX project",
        [](const std::unordered_map<std::string, std::string>&,
           const std::vector<std::string>& positionalArgs) -> int {
            if(positionalArgs.empty())
                Logger::GetInstance().Fatal("[WFX]: Project name required. Usage: wfx new <project-name>");

            return CLI::CreateProject(positionalArgs[0]);
        });

    // --- Command: doctor ---
    parser.AddCommand("doctor", "Verify system requirements",
        [](auto&& _, auto&& __) -> int {
            return CLI::WFXDoctor();
        });

    // --- Command: dev ---
    parser.AddCommand("dev", "Start WFX dev server",
        [](const std::unordered_map<std::string, std::string>& options,
           const std::vector<std::string>&) -> int {
            int port = 8000;

            try {
                port = std::stoi(options.at("--port"));
            } catch (...) {
                Logger::GetInstance().Fatal("[WFX]: Invalid port: ", options.at("--port"));
            }

            return CLI::RunDevServer(options.at("--host"), port);
        });
    parser.AddOption("dev", "--host", "Host to bind", false, "127.0.0.1", false);
    parser.AddOption("dev", "--port", "Port to bind", false, "8080", false);

    return parser.Parse(argc, argv);
}

}  // namespace WFX

// Entrypoint for the entire thing
int main(int argc, char* argv[]) {
    return WFX::WFXEntryPoint(argc, argv);
}

#endif  // WFX_CLI_MAIN_HPP