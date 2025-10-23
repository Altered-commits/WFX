#ifndef WFX_ENGINE_TEMPLATE_INTERFACE_HPP
#define WFX_ENGINE_TEMPLATE_INTERFACE_HPP

/*
 * Contains interfaces and structs necessary for template communication between-
 * -engine and user compiled .dll / .so
 */

#include "include/third_party/json/json_fwd.hpp"
#include <cstdint>
#include <string_view>
#include <variant>

// For consistency :)
using Json = nlohmann::json;

namespace WFX::Core {

// So User code returns us either:
//  - FileChunk     : length [uint64_t], offset [uint64_t]
//  - VariableChunk : identifier [string_view]
struct FileChunk {
    std::uint64_t offset; // Byte offset in the file
    std::uint64_t length; // Number of bytes to read
};

struct VariableChunk {
    const Json& value; // Reference to the value in the context
};

// Actual return type, a monostate return value signifies end of generation
using TemplateChunk = std::variant<
    std::monostate,
    FileChunk,
    VariableChunk
>;

// Interface
class BaseTemplateGenerator {
public:
    virtual ~BaseTemplateGenerator() = default;

    // Returns true if a chunk was yielded, false if the stream is complete
    virtual bool Next(TemplateChunk& outChunk) = 0;
};

/*
 * This is the function type we load from the .so file
 */
using TemplateCreatorFn = std::unique_ptr<BaseTemplateGenerator>(*)(Json&& data);

} // namespace WFX::Core

#endif // WFX_ENGINE_TEMPLATE_INTERFACE_HPP