#ifndef WFX_UTILS_ARGUMENT_PARSER_HPP
#define WFX_UTILS_ARGUMENT_PARSER_HPP

#include "utils/logger/logger.hpp"

#include <string>
#include <unordered_map>
#include <vector>
#include <functional>

using CommandHandler = std::function<int(const std::unordered_map<std::string, std::string>&, const std::vector<std::string>&)>;

namespace WFX::Utils {

class ArgumentParser {
public:
    struct Option {
        std::string description;
        std::string defaultValue;
        bool        isFlag;
        bool        required;
    };

    struct Command {
        std::string description;
        std::unordered_map<std::string, Option> options;
        CommandHandler handler;
    };

    void AddCommand(const std::string& name, const std::string& description,
                    CommandHandler handler);

    void AddOption(const std::string& command, const std::string& name, const std::string& description,
                   bool isFlag = false, const std::string& defaultValue = {}, bool required = false);

    int  Parse(int argc, char* argv[]);
    void PrintUsage() const;

private:
    std::unordered_map<std::string, Command> commands_;

    Logger& logger_ = Logger::GetInstance();
};

} // namespace WFX::Utils

#endif // WFX_UTILS_ARGUMENT_PARSER_HPP