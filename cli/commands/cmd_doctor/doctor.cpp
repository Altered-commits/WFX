#include "doctor.hpp"

#include "utils/logger/logger.hpp"
#include <fstream>
#include <cstdlib>
#include <string>
#include <filesystem>
#include <algorithm>
#include <array>

#ifdef _WIN32
    #define NULL_DEVICE " >nul 2>&1"
#else
    #define NULL_DEVICE " >/dev/null 2>&1"
#endif

namespace WFX::CLI {

struct CompilerArgs {
    const char* cargs;
    const char* largs;
};

struct CompilerConfig {
    const char* id;
    const char* command;
    const char* linker;
    const char* objFlag;
    const char* dllFlag;

    CompilerArgs prod;
    CompilerArgs debug;
};

#if defined(_MSC_VER)
constexpr CompilerConfig BUILD_COMPILER{
    "msvc", "cl", "link",
    "/Fo:", "/OUT:",     // objFlag, dllFlag
    {                    // prod
        "/std:c++17 /O2 /GL /GS /EHsc /MD /Gw /Gy /I. /IWFX/include /IWFX /c",
        "/DLL /LTCG /OPT:REF /OPT:ICF /DEBUG:OFF"
    },
    {                    // debug
        "/std:c++17 /Od /EHsc /MDd /I. /IWFX/include /IWFX /c",
        "/DLL /DEBUG"
    }
};
#elif defined(__MINGW32__) || defined(__MINGW64__)
constexpr CompilerConfig BUILD_COMPILER{
    "g++[mingw]", "g++", "g++",
    "-o ", "-o ",
    {
        "-std=c++17 -fPIC -O3 -flto=auto -fno-plt -fvisibility=hidden -fvisibility-inlines-hidden "
        "-ffunction-sections -fdata-sections -I. -IWFX/include -IWFX -c",
        "-shared -fPIC -flto=auto -Wl,--gc-sections -Wl,--strip-all"
    },
    {
        "-std=c++17 -fPIC -O0 -I. -IWFX/include -IWFX -c",
        "-shared -fPIC"
    }
};
#elif defined(__clang__)
constexpr CompilerConfig BUILD_COMPILER{
    "clang++", "clang++", "clang++",
    "-o ", "-o ",
    {
        "-std=c++17 -fPIC -O3 -flto=auto -fno-plt -fvisibility=hidden -fvisibility-inlines-hidden "
        "-ffunction-sections -fdata-sections -I. -IWFX/include -IWFX -c",
        "-shared -fPIC -flto=auto -Wl,--gc-sections -Wl,--strip-all"
    },
    {
        "-std=c++17 -fPIC -O0 -I. -IWFX/include -IWFX -c",
        "-shared -fPIC"
    }
};
#elif defined(__GNUC__)
constexpr CompilerConfig BUILD_COMPILER{
    "g++[gnu]", "g++", "g++",
    "-o ", "-o ",
    {
        "-std=c++17 -fPIC -O3 -flto=auto -fno-plt -fvisibility=hidden -fvisibility-inlines-hidden "
        "-ffunction-sections -fdata-sections -I. -IWFX/include -IWFX -c",
        "-shared -fPIC -flto=auto -Wl,--gc-sections -Wl,--strip-all"
    },
    {
        "-std=c++17 -fPIC -O0 -I. -IWFX/include -IWFX -c",
        "-shared -fPIC"
    }
};
#else
#error "[Doctor]: Unsupported compiler. (__VERSION__: " __VERSION__ ")"
#endif

static std::string RunCommand(const std::string& cmd)
{
    std::array<char, 256> buffer{};
    std::string result;

#ifdef _WIN32
    FILE* pipe = _popen((cmd + " 2>&1").c_str(), "r");
#else
    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
#endif
    if(!pipe) return result;

    while(fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
        result += buffer.data();

#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return result;
}

static bool IsCompilerAvailable(const std::string& binary)
{
#ifdef _WIN32
    return std::system(("where " + binary + NULL_DEVICE).c_str()) == 0;
#else
    return std::system(("which " + binary + NULL_DEVICE).c_str()) == 0;
#endif
}

#ifdef _WIN32
static std::pair<std::string, std::string> TryMSVCCompilerAndLinker()
{
    constexpr const char* vswhere = R"("C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")";
    std::string output = RunCommand(std::string(vswhere) +
        " -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath");

    output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
    output.erase(std::remove(output.begin(), output.end(), '\n'), output.end());
    if(output.empty()) return {"", ""};

    std::filesystem::path base = output;
    base /= "VC\\Tools\\MSVC";

    if(!std::filesystem::exists(base)) return {"", ""};

    for(const auto& entry : std::filesystem::directory_iterator(base)) {
        std::filesystem::path binDir = entry.path() / "bin\\Hostx64\\x64";
        std::string cl   = (binDir / "cl.exe").string();
        std::string link = (binDir / "link.exe").string();

        if(std::filesystem::exists(cl) && std::filesystem::exists(link))
            return {cl, link};
    }

    return {"", ""};
}
#endif

int WFXDoctor()
{
    auto& logger = WFX::Utils::Logger::GetInstance();
    logger.Info("----------------------------------------------");
    logger.Info("[Doctor]: Checking for build compiler presence");
    logger.Info("----------------------------------------------");

    std::string compiler = BUILD_COMPILER.command;
    std::string linker   = BUILD_COMPILER.linker;

#ifdef _WIN32
    if(std::string(BUILD_COMPILER.id) == "msvc") {
        auto [resolvedCompiler, resolvedLinker] = TryMSVCCompilerAndLinker();
        if(resolvedCompiler.empty() || resolvedLinker.empty()) {
            logger.Error("[X] Failed to locate MSVC tools. Please open Developer Command Prompt or add MSVC to PATH");
            return 1;
        }

        compiler = resolvedCompiler;
        linker   = resolvedLinker;

        logger.Info("[+] MSVC compiler found at: ", compiler);
        logger.Info("[+] MSVC linker found at: ", linker);
    }
#endif

    // Detect absolute or relative paths (e.g., "C:/...", "./cl.exe")
    bool isPath = compiler.find('/') != std::string::npos || compiler.find('\\') != std::string::npos;
    bool exists = isPath && std::filesystem::exists(compiler);

    // Only run IsCompilerAvailable if it's not already resolved by a valid path
    if(!exists && !IsCompilerAvailable(compiler)) {
        logger.Error("[X] Compiler '", BUILD_COMPILER.id, "' not found on this system");
        logger.Info("[!] Please install it or adjust your PATH");
        return 1;
    }

    // Always quote compiler for command execution (in case path contains spaces)
    std::string quotedCompiler = "\"" + compiler + "\"";
    std::string versionCmd     = (std::string(BUILD_COMPILER.id) == "msvc")
                                    ? quotedCompiler : quotedCompiler + " --version";
    std::string version        = RunCommand(versionCmd);

    // Extract first line only
    std::string versionLine = version.substr(0, version.find('\n'));
    logger.Info("[+] Detected: [", BUILD_COMPILER.id, ": ", versionLine, ']');

    std::ofstream out("toolchain.toml");
    out << "[Compiler]\n";
    out << "name    = \"" << BUILD_COMPILER.id       << "\"\n";
    out << "ccmd    = \"" << compiler                << "\"\n";
    out << "lcmd    = \"" << linker                  << "\"\n";
    out << "objflag = \"" << BUILD_COMPILER.objFlag  << "\"\n";
    out << "dllflag = \"" << BUILD_COMPILER.dllFlag  << "\"\n";

    out << "\n[Compiler.Prod]\n";
    out << "cargs   = \"" << BUILD_COMPILER.prod.cargs  << "\"\n";
    out << "largs   = \"" << BUILD_COMPILER.prod.largs  << "\"\n";

    out << "\n[Compiler.Debug]\n";
    out << "cargs   = \"" << BUILD_COMPILER.debug.cargs << "\"\n";
    out << "largs   = \"" << BUILD_COMPILER.debug.largs << '"';

    logger.Info("[Doctor]: Saved toolchain config to toolchain.toml");
    return 0;
}

} // namespace WFX::CLI