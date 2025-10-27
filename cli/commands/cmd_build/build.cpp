#include "build.hpp"

#include "cli/commands/common/common.hpp"
#include "config/config.hpp"
#include "engine/template_engine.hpp"

namespace WFX::CLI {

using namespace WFX::Core; // For 'TemplateEngine', 'Config'

// Supported build types: 'templates', 'source', ...
constexpr static const char* BUILD_TEMPLATES = "templates";
constexpr static const char* BUILD_SOURCE    = "source";

int BuildProject(const std::string& buildType)
{
    // Used by pretty much everything so yeah
    auto& config = Config::GetInstance();
    config.LoadCoreSettings("wfx.toml");

    if(buildType == BUILD_TEMPLATES) {
        auto& templateEngine = TemplateEngine::GetInstance();

        templateEngine.PreCompileTemplates();
        templateEngine.SaveTemplatesToCache();

        return 0;
    }

    if(buildType == BUILD_SOURCE) {
        // Needed for compiling shit
        config.LoadToolchainSettings("toolchain.toml");

        // Copied directly from dev.cpp
        const std::string dllDir  = config.projectConfig.projectName + "/build/dlls/";
        const std::string dllPath = dllDir + "user_entry.so";

        HandleUserSrcCompilation(dllDir.c_str(), dllPath.c_str());
        return 0;
    }

    // Invalid type
    Logger::GetInstance().Fatal(
        "[WFX]: Wrong build type provided: ", buildType.c_str(), ". Supported types: 'templates', 'source'"
    );

    // Not that this will ever get triggered but yeah
    return -1;
}

} // namespace WFX::CLI