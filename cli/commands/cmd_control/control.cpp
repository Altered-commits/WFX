#include "control.hpp"

#include "utils/daemon/daemon_registry.hpp"
#include "utils/diagnostics/logger.hpp"

#include <ctime>
#include <cstdio>

namespace WFX::CLI {

using namespace WFX::Utils;

// Forward declarations
int CmdList();
int CmdFolder();
int CmdStop(const std::string& project);

// vvv Entrypoint vvv
int ControlCommand(const std::string& subcommand, const std::string& project)
{
    auto& logger = GetLogger();

    if(subcommand == "list")
        return CmdList();

    if(subcommand == "folder")
        return CmdFolder();

    if(subcommand == "stop")
        return CmdStop(project);

    logger.Error(
        "[WFX]: Unknown control subcommand '", subcommand, "'. "
        "Available: list, folder, stop <project>"
    );
    return 1;
}

// vvv Helpers vvv
std::string FormatUptime(std::int64_t started)
{
    std::int64_t now     = static_cast<std::int64_t>(std::time(nullptr));
    std::int64_t elapsed = now - started;

    if(elapsed < 0)
        elapsed = 0;

    std::int64_t hours   = elapsed / 3600;
    std::int64_t minutes = (elapsed % 3600) / 60;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lldh %02lldm",
        static_cast<long long>(hours),
        static_cast<long long>(minutes)
    );

    return std::string(buf);
}

static std::string Truncate(const std::string& s, std::size_t max)
{
    if(s.size() <= max)
        return s;

    return s.substr(0, max - 3) + "...";
}

// vvv Subcommands vvv
int CmdList()
{
    auto& logger = GetLogger();

    auto daemons = DaemonRegistry::List();

    if(daemons.empty()) {
        logger.Print("No running WFX servers found.");
        return 0;
    }

    logger.Print(
        "\n"
        " PROJECT              PID       HOST                PORT    HTTPS   WORKERS UPTIME\n"
        " -------              ---       ----                ----    -----   ------- ------"
    );

    std::vector<const DaemonInfo*> truncated;

    for(const auto& d : daemons) {
        std::string displayName = Truncate(d.project, 16);
        if(displayName != d.project)
            truncated.push_back(&d);

        char row[256];
        std::snprintf(row, sizeof(row),
            " %-20s %-9d %-19s %-7d %-7s %-7d %s",
            displayName.c_str(),
            static_cast<int>(d.pid),
            d.host.c_str(),
            static_cast<int>(d.port),
            d.https ? "yes" : "no",
            d.workers,
            FormatUptime(d.started).c_str()
        );

        logger.Print(row);
    }

    if(!truncated.empty()) {
        logger.Print("\n Full project names for truncated columns:");
        for(const auto* d : truncated)
            logger.Print("   ", d->project);
    }

    logger.Print("");
    return 0;
}

int CmdFolder()
{
    auto& logger = GetLogger();
    auto  dir    = DaemonRegistry::DaemonsDir();

    if(dir.empty()) {
        logger.Error("[WFX]: Could not determine WFX directories");
        return 1;
    }

    // DaemonsDir is always ~/.wfx/daemons, root is one level up
    std::string root = dir.substr(0, dir.find_last_of("/\\"));

    logger.Print("Root Dir   -> ", root);
    logger.Print("Daemon Dir -> ", dir);
    return 0;
}

int CmdStop(const std::string& project)
{
    auto& logger = GetLogger();

    if(project.empty()) {
        logger.Error("[WFX]: Project name required. Usage: wfx control stop <project>");
        return 1;
    }

    logger.Info("[WFX]: Stopping project '", project, '\'');

    switch(DaemonRegistry::Stop(project)) {
        case StopResult::STOPPED:
            logger.Info("[WFX]: '", project, "' stopped :)");
            return 0;

        case StopResult::FORCE_KILLED:
            logger.Warn("[WFX]: Force kill '", project, "'");
            return 0;

        case StopResult::NOT_FOUND:
            logger.Error("[WFX]: No running server found for '", project, "'");
            return 1;

        case StopResult::NOT_RUNNING:
            logger.Warn("[WFX]: '", project, "' is not running");
            return 0;

        case StopResult::FAILED:
            logger.Error("[WFX]: Failed to stop '", project, "'");
            return 1;
    }

    return 1;
}

} // namespace WFX::CLI