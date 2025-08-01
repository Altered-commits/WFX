#include "doctor.hpp"
#include "utils/logger/logger.hpp"

#include <fstream>
#include <cstdlib>
#include <cstring>
#include <string>
#include <filesystem>
#include <algorithm>

#ifdef _WIN32
    #define NULL_DEVICE " >nul 2>&1"
#else
    #define NULL_DEVICE " >/dev/null 2>&1"
#endif

namespace WFX::CLI {

// --- Detect which compiler this binary was built with ---
#if defined(_MSC_VER)
    constexpr const char* COMPILER_ID       = "msvc";
    constexpr const char* COMPILER_COMMAND  = "cl";
    constexpr const char* LINKER_COMMAND    = "link";
    constexpr const char* COMPILER_DISPLAY  = "MSVC";
    constexpr const char* COMPILER_CARGS    = "/std:c++17 /O2 /GL /GS- /GR- /EHsc /MD /I. /Iinclude /Iwfx /c";
    constexpr const char* COMPILER_LARGS    = "/DLL /LTCG /OPT:REF /DEBUG:NONE";
#elif defined(__MINGW32__) || defined(__MINGW64__)
    constexpr const char* COMPILER_ID       = "g++-mingw";
    constexpr const char* COMPILER_COMMAND  = "g++";
    constexpr const char* LINKER_COMMAND    = "g++";
    constexpr const char* COMPILER_DISPLAY  = "G++ (MinGW)";
    constexpr const char* COMPILER_CARGS    =
        "-std=c++17 -O2 -flto -ffunction-sections -fdata-sections "
        "-fno-rtti -fno-exceptions "
        "-fvisibility=hidden -fvisibility-inlines-hidden "
        "-I. -Iinclude -Iwfx -c";
    constexpr const char* COMPILER_LARGS    =
        "-shared -flto -Wl,--gc-sections -Wl,--strip-all";
#elif defined(__clang__)
    constexpr const char* COMPILER_ID       = "clang++";
    constexpr const char* COMPILER_COMMAND  = "clang++";
    constexpr const char* LINKER_COMMAND    = "clang++";
    constexpr const char* COMPILER_DISPLAY  = "Clang++";
    constexpr const char* COMPILER_CARGS    =
        "-std=c++17 -O2 -flto -fvisibility=hidden -fvisibility-inlines-hidden "
        "-fno-rtti -fno-exceptions "
        "-ffunction-sections -fdata-sections "
        "-I. -Iinclude -Iwfx -c";
    constexpr const char* COMPILER_LARGS    =
        "-shared -fPIC -flto -Wl,--gc-sections -Wl,--strip-all";
#elif defined(__GNUC__)
    constexpr const char* COMPILER_ID       = "g++";
    constexpr const char* COMPILER_COMMAND  = "g++";
    constexpr const char* LINKER_COMMAND    = "g++";
    constexpr const char* COMPILER_DISPLAY  = "G++";
    constexpr const char* COMPILER_CARGS    =
        "-std=c++17 -O2 -flto -fvisibility=hidden -fvisibility-inlines-hidden "
        "-fno-rtti -fno-exceptions "
        "-ffunction-sections -fdata-sections "
        "-I. -Iinclude -Iwfx -c";
    constexpr const char* COMPILER_LARGS    =
        "-shared -fPIC -flto -Wl,--gc-sections -Wl,--strip-all";
#else
    #error "[wfx doctor]: Unsupported compiler. (__VERSION__: " __VERSION__ ") Please update doctor.cpp to add support."
#endif

// MSVC requires distinct output flags for object and DLL files (/Fo and /OUT)-
// -unlike GCC/Clang's '-o'.
constexpr const char* COMPILER_OBJ_FLAG  =
#if defined(_MSC_VER)
    "/Fo:";
#else
    "-o ";
#endif

constexpr const char* COMPILER_DLL_FLAG =
#if defined(_MSC_VER)
    "/OUT:";
#else
    "-o ";
#endif

static std::string RunCommand(const std::string& cmd)
{
    std::string result;
    FILE* pipe =
    #ifdef _WIN32
        _popen(cmd.c_str(), "r");
    #else
        popen(cmd.c_str(), "r");
    #endif

    if(!pipe) return result;

    char buffer[256];
    while(fgets(buffer, sizeof(buffer), pipe))
        result += buffer;

    #ifdef _WIN32
        _pclose(pipe);
    #else
        pclose(pipe);
    #endif

    return result;
}

static bool IsCompilerAvailable(const std::string& binary)
{
    std::string check =
    #ifdef _WIN32
        "where " + binary + NULL_DEVICE;
    #else
        "which " + binary + NULL_DEVICE;
    #endif
    return std::system(check.c_str()) == 0;
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
    logger.Info("-----------------------------------------------");
    logger.Info("[Doctor]: Checking for build compiler presence.");
    logger.Info("-----------------------------------------------");

    std::string compiler = COMPILER_COMMAND;
    std::string linker   = LINKER_COMMAND;

#ifdef _WIN32
    if(std::strcmp(COMPILER_ID, "msvc") == 0) {
        // logger.Warn("[-] MSVC (cl.exe) not found in PATH. Trying to locate via vswhere...");
        auto [resolvedCompiler, resolvedLinker] = TryMSVCCompilerAndLinker();
        if(resolvedCompiler.empty() || resolvedLinker.empty()) {
            logger.Error("[X] Failed to locate MSVC tools. Please open Developer Command Prompt or add MSVC to PATH.");
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
        logger.Error("[X] Compiler '", COMPILER_ID, "' not found on this system.");
        logger.Info("[!] Please install it or adjust your PATH.");
        return 1;
    }

    // Always quote compiler for command execution (in case path contains spaces)
    std::string quotedCompiler = "\"" + compiler + "\"";

    // Run version command appropriately
    std::string version;
    if(std::string(COMPILER_ID) == "msvc")
        version = RunCommand(quotedCompiler);  // cl.exe shows version on no args
    else
        version = RunCommand(quotedCompiler + " --version");

    // Extract and print only the first line of version info
    std::string versionLine = version.substr(0, version.find('\n'));
    logger.Info("[+] Detected: ", COMPILER_DISPLAY, ": ", versionLine);

    std::ofstream out("toolchain.toml");
    out << "[Compiler]\n";
    out << "name    = \"" << COMPILER_ID       << "\"\n";
    out << "ccmd    = \"" << compiler          << "\"\n";
    out << "lcmd    = \"" << linker            << "\"\n";
    out << "cargs   = \"" << COMPILER_CARGS    << "\"\n";
    out << "largs   = \"" << COMPILER_LARGS    << "\"\n";
    out << "objflag = \"" << COMPILER_OBJ_FLAG << "\"\n";
    out << "dllflag = \"" << COMPILER_DLL_FLAG << "\"\n";

    logger.Info("[Doctor]: Saved toolchain config to toolchain.toml");
    return 0;
}

} // namespace WFX::CLI