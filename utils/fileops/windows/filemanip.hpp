#ifndef WFX_WINDOWS_FILEMANIP_HPP
#define WFX_WINDOWS_FILEMANIP_HPP

#include "utils/fileops/filesystem.hpp"

// Common include for both filesystem.cpp and filemanip.cpp
#include <Windows.h>
#include <Shlwapi.h>  // For PathFileExistsA

#pragma comment(lib, "Shlwapi.lib")

// Don't need Windows macros, they collide with my function naming
#undef DeleteFile
#undef CreateDirectory

namespace WFX::Utils {

// 'WinFile' is not implemented right now

} // namespace WFX::Utils

#endif // WFX_WINDOWS_FILEMANIP_HPP