#include "template_engine.hpp"

#include "config/config.hpp"
#include "utils/backport/string.hpp"
#include "utils/filesystem/filesystem.hpp"

namespace WFX::Core {

TemplateEngine& TemplateEngine::GetInstance()
{
    static TemplateEngine engine;
    return engine;
}

void TemplateEngine::PreCompileTemplates()
{
    auto& fs     = FileSystem::GetFileSystem();
    auto& config = Config::GetInstance();

    // Traverse the entire template directory looking for .html files
    // Then compile those html files into /build/templates/(static | dynamic)/
    fs.ListDirectory(config.projectConfig.templateDir, true, [this](std::string path) {
        if(EndsWith(path, ".html"))
            logger_.Info("Compiling template: ", path);
    });
}

TemplateMeta* TemplateEngine::GetTemplate(const std::string& path) const
{
    return nullptr;
}

} // namespace WFX::Core