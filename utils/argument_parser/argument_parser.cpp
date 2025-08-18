#include "argument_parser.hpp"

#include <iostream>
#include <sstream>

namespace WFX::Utils {

void ArgumentParser::AddCommand(const std::string& name, const std::string& description, CommandHandler handler)
{
    commands_[name] = { description, {}, std::move(handler) };
}

void ArgumentParser::AddOption(const std::string& command, const std::string& name, const std::string& description,
                                bool isFlag, const std::string& defaultValue, bool required)
{
    auto& cmd = commands_[command];
    cmd.options[name] = { description, defaultValue, isFlag, required };
}

int ArgumentParser::Parse(int argc, char* argv[])
{
    if(argc < 2) {
        PrintUsage();
        return 1;
    }

    std::string commandName = argv[1];
    auto cmdIt = commands_.find(commandName);
    if(cmdIt == commands_.end()) {
        logger_.Error("[ArgumentParser]: Unknown command: ", commandName);
        PrintUsage();
        return 1;
    }

    const auto& cmd = cmdIt->second;
    const auto& opts = cmd.options;
    std::unordered_map<std::string, std::string> parsedOptions;
    std::vector<std::string> positionalArgs;

    for(const auto& [name, opt] : opts) {
        if(!opt.defaultValue.empty())
            parsedOptions[name] = opt.defaultValue;
    }

    for(int i = 2; i < argc; ++i) {
        std::string arg = argv[i];

        if(arg.compare(0, 2, "--") == 0) {
            auto optIt = opts.find(arg);
            if(optIt == opts.end()) {
                logger_.Error("[ArgumentParser]: Unknown option: ", arg);
                return 1;
            }

            const auto& opt = optIt->second;
            if(opt.isFlag)
                parsedOptions[arg] = "true";
            else {
                if(i + 1 >= argc) {
                    logger_.Error("[ArgumentParser]: Missing value for option: ", arg);
                    return 1;
                }
                parsedOptions[arg] = argv[++i];
            }
        }
        else
            positionalArgs.emplace_back(std::move(arg));
    }

    for(const auto& [name, opt] : opts) {
        if(opt.required && parsedOptions.find(name) == parsedOptions.end()) {
            logger_.Error("[ArgumentParser]: Missing required option: ", name);
            return 1;
        }
    }

    return cmd.handler(parsedOptions, positionalArgs);
}

void ArgumentParser::PrintUsage() const
{
    std::cout << "==================================================\n"
                 "\t__        __  ______  __    __\n"
                 "\t\\ \\      / / |  ____| \\ \\  / /\n"
                 "\t \\ \\ /\\ / /  | |__     \\ \\/ /\n"
                 "\t  \\ V  V /   |  __|    / /\\ \\\n"
                 "\t   \\_/\\_/    |_|      /_/  \\_\\\n\n"
                 "\t Weird Framework? eXactly (._.)\n"
                 "    Written in C++. Because we hate ourselves.\n"
                 "==================================================\n\n";
                 
    std::cout << "[Usage]\n"
                "./wfx <command> [options]\n\n[Available Commands]\n";
    for(const auto& [name, cmd] : commands_)
        std::cout << " * " << name << "  \t" << cmd.description << '\n';
}

} // namespace WFX::Utils