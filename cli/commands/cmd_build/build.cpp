#include "build.hpp"

#include "cli/commands/common/common.hpp"
#include "config/config.hpp"
#include "engine/template_engine.hpp"

namespace WFX::CLI {

using namespace WFX::Core; // For 'TemplateEngine', 'Config'

int BuildProject(const std::string& project, const std::string& buildType, bool isDebug)
{
    // Used by pretty much everything so yeah
    auto& config = Config::GetInstance();
    auto& logger = Logger::GetInstance();
    auto& fs     = FileSystem::GetFileSystem();

    if(!fs.DirectoryExists(project.c_str()))
        logger.Fatal("[WFX]: '", project, "' directory does not exist");

    logger.Info("[WFX]: Build mode: ", isDebug ? "debug" : "prod");

    config.LoadCoreSettings(project + "/wfx.toml");
    config.LoadFinalSettings(project);

    HandleBuildDirectory();

    if(buildType == "templates") {
        auto& templateEngine = TemplateEngine::GetInstance();

        auto [success, hasDynamic] = templateEngine.PreCompileTemplates();
        if(!success)
            return 1;

        if(hasDynamic)
            HandleUserCxxCompilation(CxxCompilationOption::TEMPLATES_ONLY);

        return 0;
    }

    if(buildType == "source") {
        HandleUserCxxCompilation(CxxCompilationOption::SOURCE_ONLY);
        return 0;
    }

    // Invalid type
    logger.Fatal(
        "[WFX]: Wrong build type provided: ", buildType.c_str(), ". Supported types: 'templates', 'source'"
    );

    // Not that this will ever get triggered but yeah
    return -1;
}

} // namespace WFX::CLI