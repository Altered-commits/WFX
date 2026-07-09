// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_NON_ABI_TEMPLATE_INTERFACE_HPP
#define WFX_SHARED_NON_ABI_TEMPLATE_INTERFACE_HPP

#include "shared/json/json_object.hpp"
#include <cstdint>
#include <string_view>
#include <variant>
#include <memory>

namespace WFX::Core {

// So User code returns us either:
//  - FileChunk     : length [uint64_t], offset [uint64_t]
//  - VariableChunk : identifier [string_view]
struct FileChunk {
    std::uint64_t offset; // Byte offset in the file
    std::uint64_t length; // Number of bytes to read
};

struct VariableChunk {
    Shared::JsonRef value; // Resolved at GetState call time
};

// Common return type of the chunk, a monostate return value signifies end of generation
using TemplateChunk = std::variant<std::monostate, FileChunk, VariableChunk>;

// Enum representation of variant types
enum class TemplateChunkType { MONOSTATE, FILE, VARIABLE };

// Actual return type of the function 'GetState'
// Returns the next state and the result of current state
struct StateResult {
    std::size_t newState;
    TemplateChunk chunk;
};

// Helper function. Returns invalid JsonRef if any key missing
inline Shared::JsonRef SafeGetJson(Shared::JsonObject& ctx, std::initializer_list<std::string_view> keys) noexcept
{
    if(keys.size() == 0)
        return Shared::JsonRef{nullptr, Shared::JSON_NIL};

    auto it = keys.begin();
    auto ref = ctx.Get(*it++);

    for(; it != keys.end(); ++it) {
        if(!ref.Valid() || !ref.IsObject())
            return Shared::JsonRef{nullptr, Shared::JSON_NIL};

        ref = ref.Get(*it);
    }

    return ref;
}

// Interface
// We will make the generator class entirely stateless
class BaseTemplateGenerator {
public:
    virtual ~BaseTemplateGenerator() = default;

    // Number of states in the compiled template
    virtual std::size_t GetStateCount() const noexcept = 0;

    // 'ctx' is the JsonObject passed by user in 'SendTemplate'
    virtual StateResult GetState(std::size_t index, Shared::JsonObject& ctx) const noexcept = 0;
};

/*
 * Function pointer type exported by compiled template
 * Engine loads it via dlsym/GetProcAddress
 */
using TemplateGeneratorPtr = std::unique_ptr<BaseTemplateGenerator>;
using TemplateCreatorFn = TemplateGeneratorPtr (*)();

} // namespace WFX::Core

#endif // WFX_SHARED_NON_ABI_TEMPLATE_INTERFACE_HPP