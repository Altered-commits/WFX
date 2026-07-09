// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "build.hpp"

#include "cli/commands/common/common.hpp"
#include "config/config.hpp"
#include "engine/template_engine.hpp"

namespace WFX::CLI {

using namespace WFX::Core; // For 'TemplateEngine', 'Config'

int BuildProject(const std::string& project, const std::string& buildType)
{
    // Used by pretty much everything so yeah
    auto& config = GetConfig();
    auto& logger = GetLogger();

    if(!FileSystem::DirectoryExists(project.c_str()))
        logger.Fatal("[WFX]: '", project, "' directory does not exist");

    config.LoadCoreSettings(project + "/wfx.toml");
    config.LoadFinalSettings(project);

    HandleBuildDirectory();

    if(buildType == "templates") {
        auto& templateEngine = GetTemplateEngine();

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
    logger.Fatal("[WFX]: Wrong build type provided: ", buildType.c_str(), ". Supported types: 'templates', 'source'");

    // Not that this will ever get triggered but yeah
    return -1;
}

} // namespace WFX::CLI