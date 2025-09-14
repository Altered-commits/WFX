#ifndef WFX_LINUX_PROCESS_HPP
#define WFX_LINUX_PROCESS_HPP

#include "utils/process/process.hpp"

namespace WFX::OSSpecific {

using namespace WFX::Utils; // For 'ProcessUtils'

class LinuxProcessUtils : public BaseProcessUtils {
    ProcessResult RunProcess(std::string& cmd, const std::string& workingDirectory = "") const override;
    ProcessResult RunProcess(
                    const std::string& executable, const std::string& args, const std::string& workingDirectory = ""
                ) const override;
};

} // namespace WFX::OSSpecific


#endif // WFX_LINUX_PROCESS_HPP