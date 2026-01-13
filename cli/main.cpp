#ifndef WFX_CLI_MAIN_HPP
#define WFX_CLI_MAIN_HPP

#include <string>
#include <vector>

#include "commands/cmd_build/build.hpp"
#include "commands/cmd_new/new.hpp"
#include "commands/cmd_doctor/doctor.hpp"
#include "commands/cmd_run/run.hpp"
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
    parser.AddCommand("doctor", "Verify system requirements (Deprecated)",
        [](auto&& _, auto&& __) -> int {
            return CLI::WFXDoctor();
        });

    // --- Command: build ---
    parser.AddCommand("build", "Pre-Build various parts of WFX",
        [](const std::unordered_map<std::string, std::string>& options,
           const std::vector<std::string>& positionalArgs) -> int {
            if(positionalArgs.size() != 2)
                Logger::GetInstance().Fatal(
                    "[WFX]: Build type is required. Usage: wfx build <project-folder-name> [templates|source]"
                );

            return CLI::BuildProject(positionalArgs[0], positionalArgs[1], options.count("--debug") > 0);
        });
    parser.AddOption("build", "--debug", "Build in debug mode", true, "", false);

    // --- Command: run ---
    parser.AddCommand("run", "Start WFX server",
        [](const std::unordered_map<std::string, std::string>& options,
           const std::vector<std::string>& positionalArgs) -> int {
            auto& logger = Logger::GetInstance();
            
            if(positionalArgs.size() != 1)
                logger.Fatal(
                    "[WFX]: Project name is required. Usage: wfx run <project-folder-name> [options]"
                );

            int port = 8080;

            try {
                port = std::stoi(options.at("--port"));
            }
            catch (...) {
                logger.Fatal("[WFX]: Invalid port: ", options.at("--port"));
            }

            CLI::ServerConfig cfg;
            cfg.host = options.at("--host");
            cfg.port = port;

            // Set flags based on CLI options
            if(options.count("--use-https") > 0)           cfg.SetFlag(CLI::ServerFlags::USE_HTTPS);
            if(options.count("--https-port-override") > 0) cfg.SetFlag(CLI::ServerFlags::OVERRIDE_HTTPS_PORT);
            if(options.count("--debug") > 0)               cfg.SetFlag(CLI::ServerFlags::USE_DEBUG);

            return CLI::RunServer(positionalArgs[0], cfg);
        });
    parser.AddOption("run", "--host",                "Host to bind",                false, "127.0.0.1", false);
    parser.AddOption("run", "--port",                "Port to bind",                false, "8080",      false);
    parser.AddOption("run", "--use-https",           "Use HTTPS connection",        true,  "",          false);
    parser.AddOption("run", "--https-port-override", "Override default HTTPS port", true,  "",          false);
    parser.AddOption("run", "--debug",               "For runtime debugging",       true,  "",          false);

    return parser.Parse(argc, argv);
}

}  // namespace WFX

// Entrypoint for the entire thing
int main(int argc, char* argv[]) {
    return WFX::WFXEntryPoint(argc, argv);
}

#endif  // WFX_CLI_MAIN_HPP