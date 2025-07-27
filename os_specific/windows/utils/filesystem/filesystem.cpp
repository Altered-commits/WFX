#include "filesystem.hpp"

#include <Windows.h>
#include <Shlwapi.h>  // For PathFileExistsA

#pragma comment(lib, "Shlwapi.lib")

// Don't need Windows macros, they collide with my function naming
#undef DeleteFile
#undef CreateDirectory

namespace WFX::OSSpecific {

// vvv File Manipulation vvv
bool WinFileSystem::FileExists(std::string_view path) const
{
    if(path.empty()) return false;

    DWORD attrib = GetFileAttributesA(path.data());
    return (attrib != INVALID_FILE_ATTRIBUTES) && !(attrib & FILE_ATTRIBUTE_DIRECTORY);
}

bool WinFileSystem::DeleteFile(std::string_view path) const
{
    if(path.empty()) return false;

    return DeleteFileA(path.data()) != 0;
}

bool WinFileSystem::RenameFile(std::string_view from, std::string_view to) const
{
    if(from.empty() || to.empty()) return false;

    return MoveFileA(from.data(), to.data()) != 0;
}

std::size_t WinFileSystem::GetFileSize(std::string_view path) const
{
    if(path.empty()) return 0;

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
    if(path.empty()) return false;

    DWORD attrib = GetFileAttributesA(path.data());
    return (attrib != INVALID_FILE_ATTRIBUTES) && (attrib & FILE_ATTRIBUTE_DIRECTORY);
}

bool WinFileSystem::CreateDirectory(std::string_view path, bool recurseParentDir) const
{
    if(path.empty()) return false;

    if(!recurseParentDir)
        return ::CreateDirectoryA(path.data(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;

    char buffer[MAX_PATH + 1];
    size_t len = path.copy(buffer, sizeof(buffer) - 1);
    buffer[len] = '\0';

    for(size_t i = 0; i < len; ++i) {
        // Normalize slashes
        if(buffer[i] == '/' || buffer[i] == '\\') {
            buffer[i] = '\\';

            // Temporarily null terminate to isolate this level
            char temp = buffer[i];
            buffer[i] = '\0';

            // Skip empty roots like "C:\"
            if(i > 0 && !(buffer[i - 1] == ':' && i == 2)) {
                if(!CreateDirectoryA(buffer, nullptr)) {
                    DWORD err = GetLastError();
                    if(err != ERROR_ALREADY_EXISTS)
                        return false;
                }
            }

            buffer[i] = temp; // Restore
        }
    }

    // Create final leaf directory
    if(!CreateDirectoryA(buffer, nullptr)) {
        DWORD err = GetLastError();
        if(err != ERROR_ALREADY_EXISTS)
            return false;
    }

    return true;
}

bool WinFileSystem::DeleteDirectory(std::string_view path) const
{
    if(path.empty()) return false;

    return RemoveDirectoryA(path.data()) != 0;
}

DirectoryList WinFileSystem::ListDirectory(std::string_view path, bool shouldRecurse) const
{
    if(path.empty()) return DirectoryList{};

    DirectoryList files;

    ListDirectoryImpl(path, shouldRecurse, [&files](std::string&& fullPath) {
        files.emplace_back(std::move(fullPath));
    });

    return files;
}

void WinFileSystem::ListDirectory(std::string_view path, bool shouldRecurse, const FileCallback& callback) const
{
    if(path.empty()) return;

    ListDirectoryImpl(path, shouldRecurse, callback);
}

// vvv HELPER FUNCTION vvv
void WinFileSystem::ListDirectoryImpl(std::string_view path, bool shouldRecurse, const FileCallback& callback) const
{
    std::string basePath(path);
    if(!basePath.empty() && basePath.back() != '\\' && basePath.back() != '/')
        basePath += '\\';

    std::string searchPath = basePath + "*";

    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);

    if(hFind == INVALID_HANDLE_VALUE)
        return;

    // RAII guard for handle
    struct HandleGuard {
        HANDLE handle_ = nullptr;
        ~HandleGuard() {
            if(handle_ && handle_ != INVALID_HANDLE_VALUE)
                FindClose(handle_);
        }
    } guard{hFind};

    do {
        const char* name = findData.cFileName;

        if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;

        std::string fullPath = basePath + name;

        const bool isDirectory    = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const bool isReparsePoint = (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

        if(isDirectory) {
            if(shouldRecurse && !isReparsePoint)
                ListDirectoryImpl(fullPath, true, callback);
        } 
        else
            callback(std::move(fullPath));
    }
    while(FindNextFileA(hFind, &findData) != 0);
}

} // namespace WFX::OSSpecific
