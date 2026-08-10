// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_CLI_MAIN_HPP
#define WFX_CLI_MAIN_HPP

#include <string>
#include <vector>

#include "commands/cmd_build/build.hpp"
#include "commands/cmd_new/new.hpp"
#include "commands/cmd_doctor/doctor.hpp"
#include "commands/cmd_run/run.hpp"
#include "commands/cmd_control/control.hpp"
#include "utils/argument_parser/argument_parser.hpp"

namespace WFX {

// For argument parser
using namespace WFX::Utils;

int BeginAwesomeness(int argc, char* argv[])
{
    ArgumentParser parser;

    // clang-format off
    // --- Command: new ---
    parser.AddCommand("new", "Create a new WFX project",
        [](const std::unordered_map<std::string, std::string>&,
           const std::vector<std::string>& positionalArgs) -> int {
            if(positionalArgs.empty())
                GetLogger().Fatal("[WFX]: Project name required. Usage: wfx new <project-name>");

            return CLI::CreateProject(positionalArgs[0]);
        });

    // --- Command: doctor ---
    parser.AddCommand("doctor", "Verify system requirements (Deprecated)",
        [](auto&&, auto&&) -> int {
            return CLI::WFXDoctor();
        });

    // --- Command: build ---
    parser.AddCommand("build", "Pre-Build various parts of WFX",
        [](const std::unordered_map<std::string, std::string>& options,
           const std::vector<std::string>& positionalArgs) -> int {
            if(positionalArgs.size() != 2)
                GetLogger().Fatal(
                    "[WFX]: Build type is required. Usage: wfx build <project-folder-name> [templates|source]"
                );

            return CLI::BuildProject(positionalArgs[0], positionalArgs[1], options.at("--env"));
        });
    parser.AddOption("build", "--env", "Environment to build", false, "local", false);

    // --- Command: run ---
    parser.AddCommand("run", "Start WFX server",
        [](const std::unordered_map<std::string, std::string>& options,
           const std::vector<std::string>& positionalArgs) -> int {
            auto& logger = GetLogger();
            
            if(positionalArgs.size() != 1)
                logger.Fatal(
                    "[WFX]: Project name is required. Usage: wfx run <project-folder-name> [options]"
                );

            std::uint16_t port = 8080;

            try {
                port = std::stoi(options.at("--port"));
            }
            catch (...) {
                logger.Fatal("[WFX]: Invalid port: ", options.at("--port"));
            }

            CLI::ServerConfig cfg;
            cfg.host = options.at("--host");
            cfg.port = port;
            cfg.env = options.at("--env");

            // Set flags based on CLI options
            if(options.count("--pin-to-cpu") > 0)          cfg.SetFlag(CLI::ServerFlags::PIN_TO_CPU);
            if(options.count("--use-https") > 0)           cfg.SetFlag(CLI::ServerFlags::USE_HTTPS);
            if(options.count("--https-port-override") > 0) cfg.SetFlag(CLI::ServerFlags::OVERRIDE_HTTPS_PORT);
            if(options.count("--detach") > 0)              cfg.SetFlag(CLI::ServerFlags::USE_DAEMON);

            return CLI::RunServer(positionalArgs[0], cfg);
        });
    parser.AddOption("run", "--host",                "Host to bind",                false, "127.0.0.1", false);
    parser.AddOption("run", "--port",                "Port to bind",                false, "8080",      false);
    parser.AddOption("run", "--env",                 "Environment to run",          false, "local",     false);
    parser.AddOption("run", "--pin-to-cpu",          "Pin worker to CPU core",      true,  "",          false);
    parser.AddOption("run", "--use-https",           "Use HTTPS connection",        true,  "",          false);
    parser.AddOption("run", "--https-port-override", "Override default HTTPS port", true,  "",          false);
    parser.AddOption("run", "--detach",              "Run server as daemon",        true,  "",          false);

    // --- Command: control ---
    parser.AddCommand("control", "Manage running WFX servers",
        [](const std::unordered_map<std::string, std::string>&,
           const std::vector<std::string>& positionalArgs) -> int {
            if(positionalArgs.empty())
                GetLogger().Fatal(
                    "[WFX]: Subcommand required. Usage: wfx control <list|folder|stop> [project-folder-name]"
                );

            const std::string& subcommand = positionalArgs[0];
            const std::string project     = positionalArgs.size() > 1 ? positionalArgs[1] : "";

            return CLI::ControlCommand(subcommand, project);
        });
    // clang-format on

    return parser.Parse(argc, argv);
}

} // namespace WFX

// Entrypoint for the entire thing
int main(int argc, char* argv[])
{
    return WFX::BeginAwesomeness(argc, argv);
}

#endif // WFX_CLI_MAIN_HPP