#include "filesystem.hpp"

#if defined(_WIN32)
    #include "os_specific/windows/utils/filesystem/filesystem.hpp"
    static WFX::OSSpecific::WinFileSystem impl;
#else
    #include "os_specific/linux/utils/filesystem/filesystem.hpp"
    static WFX::OSSpecific::LinuxFileSystem impl;
#endif

namespace WFX::Utils {
    
BaseFileSystem& FileSystem::GetFileSystem()
{
    return impl;
}

} // namespace WFX::Utils