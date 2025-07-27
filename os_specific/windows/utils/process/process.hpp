#ifndef WFX_WINDOWS_PROCESS_HPP
#define WFX_WINDOWS_PROCESS_HPP

#include "utils/process/process.hpp"

namespace WFX::OSSpecific {

using namespace WFX::Utils; // For 'ProcessUtils'

class WinProcessUtils : public BaseProcessUtils {
    ProcessResult RunProcess(std::string& cmd, const std::string& workingDirectory = "") const override;
    ProcessResult RunProcess(
                    const std::string& executable, const std::string& args, const std::string& workingDirectory = ""
                ) const override;
};

} // namespace WFX::OSSpecific


#endif // WFX_WINDOWS_PROCESS_HPP