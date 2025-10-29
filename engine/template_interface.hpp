#ifndef WFX_ENGINE_TEMPLATE_INTERFACE_HPP
#define WFX_ENGINE_TEMPLATE_INTERFACE_HPP

/*
 * Contains interfaces and structs necessary for template communication between-
 * -engine and user compiled .dll / .so
 */

#include "include/third_party/json/json.hpp"
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
    const Json* value; // Pointer to the value in the context
};

// Common return type of the chunk, a monostate return value signifies end of generation
using TemplateChunk = std::variant<
    std::monostate,
    FileChunk,
    VariableChunk
>;

// Actual return type of the function 'GetState'
// Returns the current state and the current result of the state
struct StateResult {
    std::size_t   newState;
    TemplateChunk chunk;
};

// vvv Helper Functions vvv
/*
 * NOTE: THESE FUNCTIONS ASSUMES THAT KEYS ARE ALWAYS KNOWN AT RUNTIME, WHICH THEY ARE BTW
 */
inline const Json* SafeGetJson(const Json& j, std::initializer_list<std::string_view> keys) noexcept
{
    const Json* cur = &j;
    for(const auto& k : keys) {
        if(!cur->is_object())
            return nullptr;

        auto it = cur->find(k);

        if(it == cur->end())
            return nullptr;

        cur = &(*it);
    }
    return cur;
}

// Interface
// We will make the generator class entirely stateless
class BaseTemplateGenerator {
public:
    virtual ~BaseTemplateGenerator() = default;

    // Number of states in the compiled template
    virtual std::size_t GetStateCount() const noexcept = 0;

    // Returns the TemplateResult for a given state index (0..count-1)
    virtual StateResult GetState(std::size_t index, const Json& ctx) const noexcept = 0;
};

using TemplateGeneratorPtr = std::unique_ptr<BaseTemplateGenerator>;

/*
 * Function pointer type exported by compiled template
 * Engine loads it via dlsym/GetProcAddress
 */
using TemplateCreatorFn = TemplateGeneratorPtr(*)();

} // namespace WFX::Core

#endif // WFX_ENGINE_TEMPLATE_INTERFACE_HPP