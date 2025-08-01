#include "process.hpp"

#include <vector>
#include <Windows.h>

namespace WFX::OSSpecific {

ProcessResult WinProcessUtils::RunProcess(std::string& cmd, const std::string& workingDirectory) const
{
    STARTUPINFOA si = {};
    si.cb = sizeof(si);

    PROCESS_INFORMATION pi = {};

    // Just for safety reasons
    if(cmd.empty() || cmd.back() != '\0')
        cmd.push_back('\0');

    // Working directory (nullptr = Current Directory)
    LPCSTR workDir = workingDirectory.empty() ? nullptr : workingDirectory.c_str();

    BOOL success = CreateProcessA(
        nullptr,          // lpApplicationName
        cmd.data(),       // lpCommandLine
        nullptr,          // lpProcessAttributes
        nullptr,          // lpThreadAttributes
        FALSE,            // bInheritHandles
        0,                // dwCreationFlags
        nullptr,          // lpEnvironment
        workDir,          // lpCurrentDirectory
        &si,              // lpStartupInfo
        &pi               // lpProcessInformation
    );

    // Let the caller handle the error
    if(!success)
        return ProcessResult{-1, GetLastError()};

    // Helper RAII objects
    struct HandleGuard {
        HANDLE handle_ = nullptr;
        ~HandleGuard() {
            if(handle_ && handle_ != INVALID_HANDLE_VALUE)
                CloseHandle(handle_);
        }
    } processHandle{pi.hProcess}, threadHandle{pi.hThread};

    // Wait until child process exits
    WaitForSingleObject(processHandle.handle_, INFINITE);

    DWORD exitCode = 0;
    if(!GetExitCodeProcess(processHandle.handle_, &exitCode))
        return ProcessResult{-2, GetLastError()};

    return ProcessResult{1, exitCode};
}

ProcessResult WinProcessUtils::RunProcess(
    const std::string &executable, const std::string &args, const std::string &workingDirectory) const
{
    // Combine executable and args into a single mutable command line
    std::string commandLine = "\"" + executable + "\" " + args;

    return RunProcess(commandLine, workingDirectory);
}

} // namespace WFX::OSSpecific