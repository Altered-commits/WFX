#include "doctor.hpp"

#include "utils/logger/logger.hpp"
#include <vector>
#include <fstream>
#include <filesystem>

#ifdef _WIN32
    #define DETECT_CMD "where "
    #define NULL_DEVICE " >nul 2>&1"
#else
    #define DETECT_CMD "which "
    #define NULL_DEVICE " >/dev/null 2>&1"
#endif

namespace WFX::CLI {

struct CompilerInfo {
    std::string_view id;
    std::string_view command;
    std::string_view displayName;
    std::string_view extraArgs;
};

// More flags to be added later for more optimization
static std::vector<CompilerInfo> compilers = {
#ifdef _WIN32
    {"msvc", "cl", "MSVC", "/LD /O2 /EHsc /I WFX"},
    {"clang", "clang++", "Clang", "-std=c++17 -O2 -shared -I./WFX"},
    {"gcc", "g++", "GCC", "-std=c++17 -O2 -shared -I./WFX"},
#else
    {"gcc", "g++", "GCC", "-std=c++17 -O2 -shared -fPIC -I./WFX"},
    {"clang", "clang++", "Clang", "-std=c++17 -O2 -shared -fPIC -I./WFX"},
#endif
};

static std::string RunCommand(std::string_view cmd)
{
    std::string result;
    FILE* pipe =
#ifdef _WIN32
        _popen(std::string(cmd).c_str(), "r");
#else
        popen(std::string(cmd).c_str(), "r");
#endif
    if(!pipe) return result;

    char buffer[256];
    while(fgets(buffer, sizeof(buffer), pipe) != nullptr)
        result += buffer;

#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return result;
}

static bool IsExecutableAvailable(std::string_view bin)
{
    std::string check = std::string(DETECT_CMD) + std::string(bin) + NULL_DEVICE;
    return std::system(check.c_str()) == 0;
}

static bool LooksLikeMinGW(std::string_view compilerCmd)
{
    std::string target = RunCommand(std::string(compilerCmd) + " -dumpmachine");
    return target.find("mingw") != std::string::npos;
}

static bool LooksLikeMSVC(std::string_view versionOutput)
{
    return versionOutput.find("Microsoft") != std::string_view::npos &&
           versionOutput.find("Compiler Version") != std::string_view::npos;
}

static std::string TryMSVCViaVsWhere()
{
    constexpr std::string_view vswherePath = R"("C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")";
    std::string installPath = RunCommand(std::string(vswherePath) +
        " -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath");

    installPath.erase(std::remove(installPath.begin(), installPath.end(), '\r'), installPath.end());
    installPath.erase(std::remove(installPath.begin(), installPath.end(), '\n'), installPath.end());
    if(installPath.empty()) return "";

    std::filesystem::path toolsPath = std::filesystem::path(installPath) / "VC" / "Tools" / "MSVC";
    if(!std::filesystem::exists(toolsPath)) return "";

    for(const auto& entry : std::filesystem::directory_iterator(toolsPath)) {
        if(!entry.is_directory()) continue;

        auto clPath = entry.path() / "bin" / "Hostx64" / "x64" / "cl.exe";
        if(std::filesystem::exists(clPath))
            return clPath.string();
    }

    return "";
}

int WFXDoctor()
{
    WFX::Utils::Logger& logger = WFX::Utils::Logger::GetInstance();
    logger.Info("--------------------------------------------");
    logger.Info("[Doctor]: Checking for compatible compilers.");
    logger.Info("--------------------------------------------");

    bool found       = false;
    bool msvcHandled = false;
    std::string msvcResolvedPath;

    for(auto& compiler : compilers) {
#ifdef _WIN32
        if(compiler.id == "msvc") {
            if(!IsExecutableAvailable(compiler.command)) {
                logger.Warn("[-] MSVC (cl.exe) not found in PATH. Trying to locate via vswhere...");

                msvcResolvedPath = TryMSVCViaVsWhere();
                if(msvcResolvedPath.empty()) {
                    logger.Warn("[-] vswhere failed to locate MSVC.");
                    logger.Info("[!] If you have Visual Studio installed, open Developer Command Prompt or add MSVC to your PATH.");
                    continue;
                }

                logger.Info("[+] MSVC found at: ", msvcResolvedPath);
                compiler.command = msvcResolvedPath;
                msvcHandled = true;
            }
        }
#endif
        if(!msvcHandled && !IsExecutableAvailable(compiler.command)) {
            logger.Warn("[-] ", compiler.displayName, " not found in PATH");
            continue;
        }

        std::string version;
        if(compiler.id == "msvc")
            version = RunCommand("\"" + std::string(compiler.command) + "\"");
        else
            version = RunCommand("\"" + std::string(compiler.command) + "\" --version");

        if(version.empty()) {
            logger.Warn("[-] ", compiler.displayName, " exists but version check failed");
            continue;
        }

        logger.Info("[+] ", compiler.displayName, " detected: ", version.substr(0, version.find('\n')));

#ifdef _WIN32
        if(compiler.id == "msvc" && !msvcHandled && !LooksLikeMSVC(version)) {
            logger.Warn("[-] Found `cl`, but it does not appear to be genuine MSVC");
            continue;
        }

        if(compiler.id == "gcc" && LooksLikeMinGW(compiler.command))
            logger.Info("[+] Detected as MinGW variant");
#endif

        std::ofstream out("toolchain.toml");
        out << "[Compiler]\n";
        out << "name    = \"" << compiler.id << "\"\n";
        out << "command = \"" << compiler.command << "\"\n";
        out << "args    = \"" << compiler.extraArgs << "\"\n";
        out.close();

        logger.Info("[Doctor]: Saved toolchain config to toolchain.toml");
        found = true;
        break;
    }

    if(!found) {
        logger.Error("[X] No supported compilers found in PATH.");
        logger.Info("[!] Please install GCC, Clang, or MSVC and try again.");
        logger.Info("[!] Or manually create toolchain.toml with your compiler config.");
    }

    return 0;
}

} // namespace WFX::CLI