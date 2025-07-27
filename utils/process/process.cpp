#include "process.hpp"

#if defined(_WIN32)
    #include "os_specific/windows/utils/process/process.hpp"
    static WFX::OSSpecific::WinProcessUtils impl;
#else
    #include "os_specific/linux/utils/process/process.hpp"
    static WFX::OSSpecific::LinuxProcessUtils impl;
#endif

namespace WFX::Utils {
    
BaseProcessUtils& ProcessUtils::GetInstance()
{
    return impl;
}

} // namespace WFX::Utils