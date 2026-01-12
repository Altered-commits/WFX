#ifndef WFX_UTILS_PROCESS_HPP
#define WFX_UTILS_PROCESS_HPP

#include <cstdint>
#include <string>

namespace WFX::Utils {

// Useful for debugging
struct ProcessResult {
    std::int32_t  exitCode = 0; // Function's exit code
    std::uint32_t osCode   = 0; // On Windows it is used with GetLastError
};

namespace ProcessUtils {

// NOTE: 'cmd' must be mutable string
ProcessResult RunProcess(std::string& cmd, const std::string& workingDirectory = "");
ProcessResult RunProcess(
    const std::string& executable, const std::string& args, const std::string& workingDirectory = ""
);

} // namespace ProcessUtils

} // namespace WFX::Utils

#endif // WFX_UTILS_PROCESS_HPP