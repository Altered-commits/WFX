#ifndef WFX_TEMPLATE_ENGINE_HPP
#define WFX_TEMPLATE_ENGINE_HPP

#include "utils/logger/logger.hpp"
#include <string>
#include <unordered_map>
#include <cstdint>

namespace WFX::Core {

using namespace WFX::Utils; // For 'Logger', ...

enum class TemplateType : std::uint8_t {
    PURE_STATIC,     // No template semantics, serve from build/templates/static/
    COMPILED_STATIC  // Precompiled includes,  serve from build/templates/static/
};

struct TemplateMeta {
    TemplateType type;
    std::string  fullPath; // Full path to the file to serve
};

class TemplateEngine final {
public:
    static TemplateEngine& GetInstance();

    void          PreCompileTemplates();
    TemplateMeta* GetTemplate(const std::string&) const;

private:
    TemplateEngine()  = default;
    ~TemplateEngine() = default;

    // Don't need any copy / move semantics
    TemplateEngine(const TemplateEngine&)            = delete;
    TemplateEngine(TemplateEngine&&)                 = delete;
    TemplateEngine& operator=(const TemplateEngine&) = delete;
    TemplateEngine& operator=(TemplateEngine&&)      = delete;

private: // Helper Functions


private: // Storage
    Logger& logger_ = Logger::GetInstance();

    std::unordered_map<std::string, TemplateMeta> templates_;
};

} // namespace WFX::Core

#endif // WFX_TEMPLATE_ENGINE_HPP