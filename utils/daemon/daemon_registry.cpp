// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "daemon_registry.hpp"
#include "utils/diagnostics/logger.hpp"
#include "utils/fileops/filesystem.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <signal.h>
#include <unistd.h>
#include <dirent.h>
#include <thread>
#include <chrono>

namespace WFX::Utils::DaemonRegistry {

// vvv Constants vvv
inline constexpr int MAX_FILE_SIZE = 1024;

// vvv Path Helpers vvv
std::string DaemonsDir() noexcept
{
    const char* home = std::getenv("HOME");
    if(!home || home[0] == '\0')
        return "";

    return std::string(home) + "/.wfx/daemons";
}

std::string PidFilePath(const std::string& project) noexcept
{
    return DaemonsDir() + "/" + project + ".pid";
}

// vvv File Operations vvv
bool Write(const DaemonInfo& info) noexcept
{
    const std::string dir = DaemonsDir();
    if(dir.empty())
        return false;

    if(!FileSystem::DirectoryExists(dir.c_str()) && !FileSystem::CreateDirectory(dir))
        return false;

    const std::string path = PidFilePath(info.project);

    auto f = FileSystem::OpenFileWrite(path.c_str());
    if(!f || !f->IsOpen())
        return false;

    // Build content
    char buf[MAX_FILE_SIZE];

    const int len = std::snprintf(buf, sizeof(buf),
                                  "pid=%d\n"
                                  "project=%s\n"
                                  "path=%s\n"
                                  "host=%s\n"
                                  "port=%d\n"
                                  "https=%s\n"
                                  "workers=%d\n"
                                  "worker_shutdown_timeout=%d\n"
                                  "started=%lld\n",
                                  static_cast<int>(info.pid), info.project.c_str(), info.path.c_str(),
                                  info.host.c_str(), static_cast<int>(info.port), info.https ? "true" : "false",
                                  info.workers, info.workerShutdownTimeout, static_cast<long long>(info.started));

    if(len <= 0 || len >= static_cast<int>(sizeof(buf)))
        return false;

    return f->Write(buf, static_cast<std::size_t>(len)) == len;
}

ReadResult Read(const std::string& project, DaemonInfo& out) noexcept
{
    const std::string path = PidFilePath(project);

    if(!FileSystem::FileExists(path.c_str()))
        return ReadResult::NOT_FOUND;

    auto f = FileSystem::OpenFileRead(path.c_str());
    if(!f || !f->IsOpen())
        return ReadResult::IO_ERROR;

    const std::size_t size = f->Size();
    if(size == 0 || size > MAX_FILE_SIZE)
        return ReadResult::CORRUPTED;

    std::string content(size, '\0');
    if(f->Read(content.data(), size) != static_cast<std::int64_t>(size))
        return ReadResult::IO_ERROR;

    // Parse line by line
    std::size_t pos = 0;
    while(pos < content.size()) {
        std::size_t end = content.find('\n', pos);
        if(end == std::string::npos)
            end = content.size();

        // Work with 'string_view' to avoid allocations
        std::string_view line(content.data() + pos, end - pos);

        if(!line.empty() && line.back() == '\r')
            line.remove_suffix(1);

        const std::size_t eq = line.find('=');
        if(eq != std::string::npos) {
            const std::string_view key = line.substr(0, eq);
            const std::string_view val = line.substr(eq + 1);

            if(key == "pid")
                out.pid = static_cast<pid_t>(std::strtol(val.data(), nullptr, 10));
            else if(key == "project")
                out.project.assign(val);
            else if(key == "host")
                out.host.assign(val);
            else if(key == "port")
                out.port = static_cast<std::uint16_t>(std::strtol(val.data(), nullptr, 10));
            else if(key == "https")
                out.https = (val == "true");
            else if(key == "workers")
                out.workers = static_cast<int>(std::strtol(val.data(), nullptr, 10));
            else if(key == "worker_shutdown_timeout")
                out.workerShutdownTimeout = static_cast<int>(std::strtol(val.data(), nullptr, 10));
            else if(key == "path")
                out.path.assign(val);
            else if(key == "started")
                out.started = static_cast<std::int64_t>(std::strtoll(val.data(), nullptr, 10));
        }

        pos = end + 1;
    }

    return out.pid > 0 ? ReadResult::OK : ReadResult::CORRUPTED;
}

bool Delete(const std::string& project) noexcept
{
    const std::string path = PidFilePath(project);
    return FileSystem::DeleteFile(path.c_str());
}

std::vector<DaemonInfo> List() noexcept
{
    auto& logger = GetLogger();

    std::vector<DaemonInfo> result;

    const std::string dir = DaemonsDir();
    if(!FileSystem::DirectoryExists(dir.c_str()))
        return result;

    FileSystem::ListDirectory(dir, false, [&](const std::string& entry) {
        std::string_view name = entry;

        // Extract filename from full path
        const std::size_t slash = name.find_last_of("/\\");
        if(slash != std::string_view::npos)
            name.remove_prefix(slash + 1);

        // Only process .pid files
        if(name.size() < 5 || name.substr(name.size() - 4) != ".pid")
            return;

        const std::string project(name.substr(0, name.size() - 4));

        DaemonInfo info;
        if(Read(project, info) != ReadResult::OK) {
            logger.Warn("[DaemonRegistry]: Removing corrupted PID file for '", project, "'");

            if(!Delete(project))
                logger.Error("[DaemonRegistry]: Failed to delete corrupted PID file for '", project, "'");

            return;
        }

        if(!IsAlive(info.pid)) {
            logger.Warn("[DaemonRegistry]: Removing stale PID file for '", project, "'");

            if(!Delete(project))
                logger.Error("[DaemonRegistry]: Failed to delete stale PID file for '", project, "'");

            return;
        }

        result.push_back(std::move(info));
    });

    return result;
}

// vvv Process Operations vvv
bool IsAlive(pid_t pid) noexcept
{
    if(pid <= 0)
        return false;

    return kill(pid, 0) == 0;
}

StopResult Stop(const std::string& project, int extraGraceSeconds) noexcept
{
    auto& logger = GetLogger();

    DaemonInfo info;
    switch(Read(project, info)) {
        case ReadResult::NOT_FOUND:
            return StopResult::NOT_FOUND;

        case ReadResult::IO_ERROR:
        case ReadResult::CORRUPTED:
            return StopResult::FAILED;
    }

    if(!IsAlive(info.pid)) {
        if(!Delete(project))
            logger.Warn("[DaemonRegistry]: Failed to delete stale PID file for '", project, "'");

        return StopResult::NOT_RUNNING;
    }

    if(kill(info.pid, SIGTERM) != 0) {
        logger.Error("[DaemonRegistry]: Failed to send SIGTERM to '", project, "' (pid=", info.pid,
                     "): ", strerror(errno));

        return StopResult::FAILED;
    }

    // The master waits up to 'workerShutdownTimeout' (once, for every worker together, see-
    // -the shutdown loop in 'RunServerImpl') before it force-kills stragglers itself. Wait at-
    // -least that long here too, plus a small buffer for its own exit bookkeeping, so we never-
    // -SIGKILL the master while it's still busy cleaning up its own children.
    const int timeoutSeconds = info.workerShutdownTimeout + extraGraceSeconds;

    // Poll for clean exit every 100ms
    const int polls = timeoutSeconds * 10;
    for(int i = 0; i < polls; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if(!IsAlive(info.pid))
            return StopResult::STOPPED; // Server cleaned up its own PID file
    }

    // Timed out, retry SIGKILL up to 3 times
    for(int attempt = 0; attempt < 3; attempt++) {
        if(kill(info.pid, SIGKILL) != 0) {
            logger.Error("[DaemonRegistry]: Failed to send SIGKILL to '", project, "' (pid=", info.pid, ") attempt ",
                         attempt + 1, ": ", strerror(errno));

            continue;
        }

        // Give it 500ms to die after SIGKILL
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        if(!IsAlive(info.pid)) {
            // Force killed, server couldn't clean up, we do it
            if(!Delete(project))
                logger.Warn("[DaemonRegistry]: Failed to delete PID file for '", project, "'");

            return StopResult::FORCE_KILLED;
        }
    }

    // Process survived SIGKILL, something is very wrong
    logger.Error("[DaemonRegistry]: Process '", project, "' (pid=", info.pid, ") survived SIGKILL, *wfx dies inside*");

    return StopResult::FAILED;
}

} // namespace WFX::Utils::DaemonRegistry