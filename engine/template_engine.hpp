#ifndef WFX_TEMPLATE_ENGINE_HPP
#define WFX_TEMPLATE_ENGINE_HPP

#include "template_interface.hpp"
#include "config/config.hpp"
#include "legacy/lexer.hpp"
#include "utils/logger/logger.hpp"
#include "utils/filesystem/filesystem.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <variant>
#include <stack>
#include <cstdint>

namespace WFX::Core {

using namespace WFX::Utils; // For 'Logger', ...

enum class TemplateType : std::uint8_t {
    FAILURE,    // Failed to compile
    STATIC,     // No template semantics / Was compiled entirely, served from build/templates/static/
    DYNAMIC     // Cannot be precompiled entierly,                served from build/templates/dynamic/
};

struct TemplateMeta {
    TemplateType type{TemplateType::STATIC};
    std::size_t  size{0};
    std::string  pathOrName{};
    // For dynamic templates only
    TemplateGeneratorPtr gen{nullptr};
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
        JUMP,    // Unconditional jump (used to skip past elif/else)
    };

    // OpCode for RPN State Machine
    enum class RPNOpCode : uint8_t {
        PUSH_CONST,
        PUSH_VAR,
        OP_GET_ATTR,
        OP_AND,
        OP_OR,
        OP_NOT,
        OP_EQ,
        OP_NEQ,
        OP_GT,
        OP_GTE,
        OP_LT,
        OP_LTE
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

    // A single RPN instruction (8 bytes)
    struct RPNOp {
        RPNOpCode     code = {};
        std::uint32_t arg  = 0; // Index for PUSH_CONST or PUSH_VAR

        bool operator==(const RPNOp& other) const {
            return this->code == other.code
                && this->arg == other.arg;
        }
    };

    // An 'Expression' is just a flat vector of these ops
    using RPNBytecode = std::vector<RPNOp>;

    // For constants
    using Value = std::variant<
        std::monostate, // Represents 'null'
        bool,
        double,
        std::int64_t,
        std::string
    >;

    using LiteralValue     = std::pair<std::uint64_t, std::uint64_t>; // offset, length
    using ConditionalValue = std::pair<std::uint32_t, std::uint32_t>; // jump_state, expr_index

    using ParseResult = std::pair<bool, std::uint32_t>; // success?, expr_index

    struct Op {
        OpType type  = {};
        bool   patch = false; // For conditions, whether it needs patching jump states or not

        std::variant<
            std::monostate,   // For JUMP, ELSE, ENDIF
            std::uint32_t,    // For VAR (stores expr_index), JUMP (stored unconditional jum_offset)
            LiteralValue,     // For LITERAL (stores offset, length)
            ConditionalValue  // For IF, ELIF (stores the jump_state, expr_index)
        > payload{};
    };

    using IRCode = std::vector<Op>;

    struct TranspilationContext {
        TemplateFrame frame;
        IRCode        ir;

        std::stack<std::vector<std::uint32_t>> ifPatchStack;

        // Optimization ig
        std::unordered_map<std::string, std::uint32_t> varNameMap;
        std::unordered_map<std::size_t, std::uint32_t> rpnMap;
        std::map<Value, std::uint32_t>                 constMap;
        std::vector<RPNBytecode>                       rpnPool;
        std::vector<std::string>                       staticVarNames;
        std::vector<Value>                             staticConstants;

        std::uint64_t currentLiteralStartOffset = 0;
        std::uint64_t currentLiteralLength      = 0;
        std::uint32_t chunkSize                 = 0;

        TranspilationContext(BaseFilePtr in, std::uint32_t chunk)
            : frame(std::move(in), chunk), chunkSize(chunk)
        {}
    };

private: // Helper functions
    void           LoadDynamicTemplatesFromLib();
    TemplateResult CompileTemplate(BaseFilePtr inTemplate, BaseFilePtr outTemplate);
    bool           PushFile(CompilationContext& context, const std::string& relPath);
    Tag            ExtractTag(std::string_view line);
    TagResult      ProcessTag(CompilationContext& context, std::string_view tagView);
    TagResult      ProcessTagIR(TranspilationContext& ctx, std::string_view tagView);
    std::uint32_t  GetVarNameId(TranspilationContext& ctx, const std::string& name);
    std::uint32_t  GetConstId(TranspilationContext& ctx, const Value& val);

private: // Transpiler Functions (Impl in template_transpiler.cpp)
    // Parsing Functions
    ParseResult   ParseExpr(TranspilationContext& ctx, std::string expression);
    std::uint32_t GetOperatorPrecedence(Legacy::TokenType type);
    bool          PopOperator(std::stack<Legacy::Token>& opStack, RPNBytecode& outputQueue);
    bool          IsOperator(Legacy::TokenType type);
    bool          IsRightAssociative(Legacy::TokenType type);

    // Emitter Functions
    std::string GenerateCxxFromRPN(
        TranspilationContext& ctx, std::uint32_t rpnIndex
    );
    bool GenerateIRFromTemplate(
        TranspilationContext& ctx, const std::string& staticHtmlPath
    );
    bool GenerateCxxFromIR(
        TranspilationContext& ctx, const std::string& outCxxPath, const std::string& funcName
    );
    bool GenerateCxxFromTemplate(
        const std::string& inHtmlPath, const std::string& outCxxPath, const std::string& funcName
    );

    // Final DLL Generator (If going down the path of template compilation)
    void CompileCxxToLib(
        const std::string& inCxxDir, const std::string& inObjDir
    );

    // Helper Functions
    RPNOpCode     TokenToOpCode(Legacy::TokenType type);
    std::uint64_t HashBytecode(const RPNBytecode& rpn);

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

    constexpr static std::size_t      maxTagLength_   = 300;

    constexpr static const char*      templateLib_      = "/build/dlls/user_templates.so";
    constexpr static const char*      cacheFile_        = "/build/templates/cache.bin";
    constexpr static const char*      staticFolder_     = "/build/templates/static";
    constexpr static const char*      dynamicCppFolder_ = "/build/templates/dynamic/cxx";
    constexpr static const char*      dynamicObjFolder_ = "/build/templates/dynamic/objs";

    constexpr static const char*      dynamicTemplateFuncPrefix_ = "__TmplSM_";

private: // Storage
    Logger& logger_ = Logger::GetInstance();
    Config& config_ = Config::GetInstance();

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