#include "filesystem.hpp"

#include <Windows.h>
#include <Shlwapi.h>  // For PathFileExistsA
#include <cassert>

#pragma comment(lib, "Shlwapi.lib")

// Don't need Windows macros. I'm using 'A' variant functions always here
#undef DeleteFile
#undef CreateDirectory

namespace WFX::OSSpecific {

// vvv File Manipulation vvv
bool WinFileSystem::FileExists(std::string_view path) const
{
    DWORD attrib = GetFileAttributesA(path.data());
    return (attrib != INVALID_FILE_ATTRIBUTES) && !(attrib & FILE_ATTRIBUTE_DIRECTORY);
}

bool WinFileSystem::DeleteFile(std::string_view path) const
{
    return DeleteFileA(path.data()) != 0;
}

bool WinFileSystem::RenameFile(std::string_view from, std::string_view to) const
{
    return MoveFileA(from.data(), to.data()) != 0;
}

std::size_t WinFileSystem::GetFileSize(std::string_view path) const
{
    WIN32_FILE_ATTRIBUTE_DATA fileInfo;
    if(!GetFileAttributesExA(path.data(), GetFileExInfoStandard, &fileInfo))
        return 0;

    LARGE_INTEGER size;
    size.HighPart = fileInfo.nFileSizeHigh;
    size.LowPart  = fileInfo.nFileSizeLow;
    
    return static_cast<std::size_t>(size.QuadPart);
}

// vvv Directory Manipulation vvv
bool WinFileSystem::DirectoryExists(std::string_view path) const
{
    DWORD attrib = GetFileAttributesA(path.data());
    return (attrib != INVALID_FILE_ATTRIBUTES) && (attrib & FILE_ATTRIBUTE_DIRECTORY);
}

bool WinFileSystem::CreateDirectory(std::string_view path) const
{
    return CreateDirectoryA(path.data(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool WinFileSystem::DeleteDirectory(std::string_view path) const
{
    return RemoveDirectoryA(path.data()) != 0;
}

std::vector<std::string> WinFileSystem::ListDirectory(std::string_view path) const
{
    std::vector<std::string> files;

    std::string searchPath(path);
    if(!searchPath.empty() && searchPath.back() != '\\' && searchPath.back() != '/')
        searchPath += '\\';
    searchPath += "*";

    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);

    if(hFind == INVALID_HANDLE_VALUE)
        return files;

    do {
        const char* name = findData.cFileName;
        if(strcmp(name, ".") != 0 && strcmp(name, "..") != 0)
            files.emplace_back(name);
    }
    while(FindNextFileA(hFind, &findData) != 0);

    FindClose(hFind);
    return files;
}

} // namespace WFX::OSSpecific
