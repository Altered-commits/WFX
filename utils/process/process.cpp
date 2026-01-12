#include "process.hpp"

#ifdef _WIN32
    #include <Windows.h>
#else
    #include <spawn.h>
    #include <unistd.h>
    #include <sys/wait.h>
    #include <cerrno>
#endif

namespace WFX::Utils {

namespace ProcessUtils {

ProcessResult RunProcess(std::string& cmd, const std::string& workingDirectory)
{
#ifdef _WIN32
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
#else
    if(cmd.empty())
        return ProcessResult{-1, 0};

    pid_t pid = 0;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    // Change working directory if provided
    if(!workingDirectory.empty())
    {
        // posix_spawnattr_t doesn’t directly change cwd, so we use file_actions workaround
        if(chdir(workingDirectory.c_str()) != 0)
        {
            posix_spawn_file_actions_destroy(&actions);
            return ProcessResult{-1, (std::uint32_t)errno};
        }
    }

    // Execute via shell
    const char* argv[] = {"/bin/sh", "-c", cmd.c_str(), nullptr};
    int spawnRes = posix_spawn(&pid, "/bin/sh", &actions, nullptr, const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);

    if(spawnRes != 0)
        return ProcessResult{-1, static_cast<std::uint32_t>(spawnRes)};

    // Wait for the child
    int status = 0;
    if(waitpid(pid, &status, 0) == -1)
        return ProcessResult{-1, (std::uint32_t)errno};

    int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return ProcessResult{exitCode, 0};
#endif
}

ProcessResult RunProcess(
    const std::string& executable, const std::string& args, const std::string& workingDirectory
) {
    std::string commandLine = "\"" + executable + "\" " + args;
    return RunProcess(commandLine, workingDirectory);
}

} // namespace ProcessUtils

} // namespace WFX::Utils