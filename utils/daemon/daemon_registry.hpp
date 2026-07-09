// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_UTILS_DAEMON_REGISTRY_HPP
#define WFX_UTILS_DAEMON_REGISTRY_HPP

#include <cstdint>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/types.h>
#else
using pid_t = int;
#endif

namespace WFX::Utils {

struct DaemonInfo {
    std::string project;
    std::string path;
    std::string host;
    std::int64_t started = 0; // Unix timestamp
    pid_t pid = -1;
    int workers = 0;
    int workerShutdownTimeout = 5; // Read back by Stop() to size its own wait budget
    std::uint16_t port = 0;
    bool https = false;
};

enum class ReadResult : std::uint8_t {
    OK,        // Read and parsed successfully
    NOT_FOUND, // PID file does not exist
    CORRUPTED, // File exists but could not be parsed or PID is invalid
    IO_ERROR   // File exists but could not be opened or read
};

enum class StopResult : std::uint8_t {
    STOPPED,      // Clean shutdown via SIGTERM
    FORCE_KILLED, // Timed out, killed via SIGKILL
    NOT_FOUND,    // No PID file for that project
    NOT_RUNNING,  // PID file exists but process is dead, cleaned up
    FAILED        // Could not send signal
};

namespace DaemonRegistry {

// vvv Path Helpers vvv
std::string DaemonsDir() noexcept;
std::string PidFilePath(const std::string& project) noexcept;

// vvv File Operations vvv
bool Write(const DaemonInfo& info) noexcept;
ReadResult Read(const std::string& project, DaemonInfo& out) noexcept;
bool Delete(const std::string& project) noexcept;
std::vector<DaemonInfo> List() noexcept;

// vvv Process Operations vvv
bool IsAlive(pid_t pid) noexcept;
StopResult Stop(const std::string& project, int extraGraceSeconds = 2) noexcept;

} // namespace DaemonRegistry

} // namespace WFX::Utils

#endif // WFX_UTILS_DAEMON_REGISTRY_HPP