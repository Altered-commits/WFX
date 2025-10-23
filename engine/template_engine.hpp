#ifndef WFX_TEMPLATE_ENGINE_HPP
#define WFX_TEMPLATE_ENGINE_HPP

#include "utils/logger/logger.hpp"
#include "utils/filesystem/filesystem.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <stack>
#include <deque>
#include <cstdint>

namespace WFX::Core {

using namespace WFX::Utils; // For 'Logger', ...

enum class TemplateType : std::uint8_t {
    FAILURE,    // Failed to compile
    STATIC,     // No template semantics / Was compiled entirely, served from build/templates/static/
    DYNAMIC     // Cannot be precompiled entierly,                served from build/templates/dynamic/
};

struct TemplateMeta {
    TemplateType type;
    std::size_t  size;
    std::string  fullPath; // Full path to the file to serve
};

// <Type, FileSize>
using TemplateResult = std::pair<TemplateType, std::size_t>;
using BufferPtr      = std::unique_ptr<char[]>;
using Tag            = std::pair<std::string_view, std::string_view>;

class TemplateEngine final {
public:
    static TemplateEngine& GetInstance();

    bool          LoadTemplatesFromCache(); // ---
    void          SaveTemplatesToCache();   //   | -> To be called in master process only
    void          PreCompileTemplates();    // ---
    TemplateMeta* GetTemplate(std::string&& relPath);

private: // Nested helper types for the parser
    enum class TagResult : std::uint8_t {
        FAILURE,                  // Ded
        SUCCESS,                  // Processed tag
        CONTROL_TO_ANOTHER_FILE,  // For include' and 'extends' tags
        PASSTHROUGH_DYNAMIC       // For dynamic tag ('if', 'for', variables, etc)
    };

    enum class OpType : std::uint8_t { 
        LITERAL, // Stream static data
        VAR,     // Stream a dynamic variable
        IF,      // Conditional jump
        ELIF,    // Conditional jump
        ELSE,    // Marker for 'else' block
        ENDIF,   // Marker for 'endif'
        JUMP     // Unconditional jump (used to skip past elif/else)
    };

    // Generic buffered I/O for writing
    struct IOContext {
        BaseFilePtr   file;
        BufferPtr     buffer;
        std::uint32_t chunkSize{0};
        std::uint32_t offset{0};

        IOContext(BaseFilePtr f, std::uint32_t chunk)
            : file(std::move(f)),
            buffer(std::make_unique<char[]>(chunk)),
            chunkSize(chunk)
        {}
    };

    struct TemplateFrame {
        BaseFilePtr   file;
        BufferPtr     readBuf;
        std::string   carry;
        std::size_t   readOffset{0};
        std::int64_t  bytesRead{0};
        bool          firstRead{true};

        TemplateFrame(BaseFilePtr f, std::uint32_t chunkSize)
            : file(std::move(f)),
            readBuf(std::make_unique<char[]>(chunkSize))
        {}
    };
    
    // Compilation context
    struct CompilationContext {
        IOContext                 io;          // Unified write buffer
        std::deque<TemplateFrame> stack;       // Recursive includes
        std::size_t               chunkSize{0};

        bool foundDynamicTag{false};    // ---
        bool inBlock{false};            //   | Aligned to-
        bool skipUntilFlag{false};      //   | -8 bytes
        bool justProcessedTag{false};   // ---

        // {% extends ... %} stuff
        std::string currentExtendsName;

        // {% block ... %} stuff
        std::unordered_map<std::string, std::string> childBlocks;
        std::string currentBlockName;
        std::string currentBlockContent;

        CompilationContext(BaseFilePtr out, std::uint32_t chunk)
            : io(std::move(out), chunk),
            chunkSize(chunk)
        {}
    };

    struct Op {
        OpType        type;
        bool          patch  = false; // Mostly for patching jump offsets in conditions
        std::string   data;           // Stores the variable identifier as a string
        std::uint64_t offset = 0;     // For LITERAL: byte offset in static file
        std::uint64_t length = 0;     // For LITERAL: byte length in static file

        std::uint32_t stateNum    = 0; // State number for State Machine thingy
        std::uint32_t targetState = 0; // State to jump to (for IF, ELIF, JUMP)
    };

    using IRCode = std::vector<Op>;

    struct IRContext {
        TemplateFrame frame;

        // Main shit
        IRCode ir;

        // Yeah man wtf
        std::stack<std::vector<std::uint32_t>>         ifPatchStack;
        std::unordered_map<std::string, std::uint32_t> varNameMap;
        std::vector<std::string>                       staticVarNames;

        // Track file offset and length relative to file start (for Literal Op)
        std::uint64_t currentLiteralStartOffset = 0;
        std::uint64_t currentLiteralLength      = 0;
        std::uint32_t currentState              = 0;

        IRContext(BaseFilePtr in, std::uint32_t chunk)
            : frame(std::move(in), chunk)
        {}
    };

private: // Helper functions
    TemplateResult CompileTemplate(BaseFilePtr inTemplate, BaseFilePtr outTemplate);
    bool           PushFile(CompilationContext& context, const std::string& relPath);
    Tag            ExtractTag(std::string_view line);
    TagResult      ProcessTag(CompilationContext& context, std::string_view tagView);
    TagResult      ProcessTagIR(IRContext& ctx, std::string_view tagView);
    std::uint32_t  GetVarNameId(IRContext& ctx, const std::string& name);

private: // Transpiler functions (Impl in template_transpiler.cpp)
    IRCode GenerateIRFromTemplate(const std::string& staticHtmlPath);
    bool GenerateCxxFromIR(
        const std::string& outCxxPath, const std::string& funcName, std::vector<Op>&& irCode
    );

private: // IO functions
    bool FlushWrite(IOContext& context, bool force = false);
    bool SafeWrite(IOContext& context, const void* data, std::size_t size, bool skipSpaces = false);

private:
    TemplateEngine()  = default;
    ~TemplateEngine() = default;

    // Don't need any copy / move semantics
    TemplateEngine(const TemplateEngine&)            = delete;
    TemplateEngine(TemplateEngine&&)                 = delete;
    TemplateEngine& operator=(const TemplateEngine&) = delete;
    TemplateEngine& operator=(TemplateEngine&&)      = delete;

private: // For ease of use across functions
    constexpr static std::string_view partialTag_     = "{% partial %}";
    constexpr static std::size_t      partialTagSize_ = partialTag_.size();

    constexpr static std::size_t      maxTagLength_   = 256 + 14;

    constexpr static const char*      cacheFile_        = "/build/templates/cache.bin";
    constexpr static const char*      staticFolder_     = "/build/templates/static";
    constexpr static const char*      dynamicCppFolder_ = "/build/templates/dynamic/cxx";
    constexpr static const char*      dynamicObjFolder_ = "/build/templates/dynamic/objs";

    constexpr static const char*      dynamicTemplateFuncPrefix_ = "__WFXRender_";

private: // Storage
    Logger& logger_ = Logger::GetInstance();

    // We don't want to save template data to cache.bin always, only save it if we-
    // -compile the templates, in which case there might be a chance the data is modified
    bool resaveCacheFile_ = false;

    // CRITICAL WARNING: The data in this map MUST be treated as immutable after initial-
    // -population. Internal engine code may store string_views that point directly to the-
    // -'fullPath' strings contained here. Modifying this map at runtime (e.g., adding,-
    // -removing, or reloading templates) will cause dangling pointers and crash the server
    std::unordered_map<std::string, TemplateMeta> templates_;
};

} // namespace WFX::Core

#endif // WFX_TEMPLATE_ENGINE_HPP