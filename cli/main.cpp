#ifndef WFX_CLI_MAIN_HPP
#define WFX_CLI_MAIN_HPP

#include <string>
#include <vector>

#include "commands/cmd_build/build.hpp"
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

    // --- Command: build ---
    parser.AddCommand("build", "Pre-Build various parts of WFX",
        [](const std::unordered_map<std::string, std::string>& options,
           const std::vector<std::string>& positionalArgs) -> int {
            if(positionalArgs.empty())
                Logger::GetInstance().Fatal("[WFX]: Build type is required. Usage: wfx build [templates|source]");

            return CLI::BuildProject(positionalArgs[0], options.count("--debug") > 0);
        });
    parser.AddOption("build", "--debug", "Build in debug mode", true, "", false);

    // --- Command: dev ---
    parser.AddCommand("dev", "Start WFX dev server",
        [](const std::unordered_map<std::string, std::string>& options,
           const std::vector<std::string>&) -> int {
            auto& logger = Logger::GetInstance();
            int   port   = 8080;

            try {
                port = std::stoi(options.at("--port"));
            }
            catch (...) {
                logger.Fatal("[WFX]: Invalid port: ", options.at("--port"));
            }

            CLI::ServerConfig cfg;
            cfg.host = options.at("--host");
            cfg.port = port;

            // --no-cache combinations, either use --no-cache for everything
            // or use --no-source-cache, --no-template-cache for specific stuff
            // but not at the same time
            bool noCache         = options.count("--no-cache") > 0;
            bool noBuildCache    = options.count("--no-source-cache") > 0;
            bool noTemplateCache = options.count("--no-template-cache") > 0;

            if(noCache && (noBuildCache || noTemplateCache))
                logger.Fatal("[WFX]: Cannot set --no-source-cache or --no-template-cache while --no-cache is enabled");

            // Set flags based on CLI options
            if(noCache) {
                cfg.SetFlag(CLI::ServerFlags::NO_BUILD_CACHE);
                cfg.SetFlag(CLI::ServerFlags::NO_TEMPLATE_CACHE);
            }
            else {
                if(noBuildCache)    cfg.SetFlag(CLI::ServerFlags::NO_BUILD_CACHE);
                if(noTemplateCache) cfg.SetFlag(CLI::ServerFlags::NO_TEMPLATE_CACHE);
            }
            if(options.count("--use-https") > 0)           cfg.SetFlag(CLI::ServerFlags::USE_HTTPS);
            if(options.count("--https-port-override") > 0) cfg.SetFlag(CLI::ServerFlags::OVERRIDE_HTTPS_PORT);
            if(options.count("--debug") > 0)               cfg.SetFlag(CLI::ServerFlags::USE_DEBUG);

            return CLI::RunDevServer(cfg);
        });
    parser.AddOption("dev", "--host",                "Host to bind",                false, "127.0.0.1", false);
    parser.AddOption("dev", "--port",                "Port to bind",                false, "8080",      false);
    parser.AddOption("dev", "--no-cache",            "Disable caching",             true,  "",          false);
    parser.AddOption("dev", "--no-source-cache",     "Disable build cache",         true,  "",          false);
    parser.AddOption("dev", "--no-template-cache",   "Disable template cache",      true,  "",          false);
    parser.AddOption("dev", "--use-https",           "Use HTTPS connection",        true,  "",          false);
    parser.AddOption("dev", "--https-port-override", "Override default HTTPS port", true,  "",          false);
    parser.AddOption("dev", "--debug",               "For runtime debugging",       true,  "",          false);

    return parser.Parse(argc, argv);
}

}  // namespace WFX

// Entrypoint for the entire thing
int main(int argc, char* argv[]) {
    return WFX::WFXEntryPoint(argc, argv);
}

#endif  // WFX_CLI_MAIN_HPP