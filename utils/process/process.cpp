// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "process.hpp"

#include <spawn.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cerrno>

namespace WFX::Utils {

namespace ProcessUtils {

ProcessResult RunProcess(std::string& cmd, const std::string& workingDirectory)
{
    if(cmd.empty())
        return ProcessResult{-1, 0};

    pid_t pid = 0;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    // Change working directory if provided
    if(!workingDirectory.empty()) {
        // posix_spawnattr_t doesn’t directly change cwd, so we use file_actions workaround
        if(chdir(workingDirectory.c_str()) != 0) {
            posix_spawn_file_actions_destroy(&actions);
            return ProcessResult{-1, (std::uint32_t)errno};
        }
    }

    // Execute via shell
    const char* argv[] = {"/bin/sh", "-c", cmd.c_str(), nullptr};
    const int spawnRes = posix_spawn(&pid, "/bin/sh", &actions, nullptr, const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);

    if(spawnRes != 0)
        return ProcessResult{-1, static_cast<std::uint32_t>(spawnRes)};

    // Wait for the child
    int status = 0;
    if(waitpid(pid, &status, 0) == -1)
        return ProcessResult{-1, (std::uint32_t)errno};

    const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return ProcessResult{exitCode, 0};
}

ProcessResult RunProcess(const std::string& executable, const std::string& args, const std::string& workingDirectory)
{
    std::string commandLine = "\"" + executable + "\" " + args;
    return RunProcess(commandLine, workingDirectory);
}

} // namespace ProcessUtils

} // namespace WFX::Utils