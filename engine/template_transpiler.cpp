#include "template_engine.hpp"
#include "config/config.hpp"
#include "utils/process/process.hpp"
#include "utils/crypt/hash.hpp"
#include "utils/backport/string.hpp"

namespace WFX::Core {

// vvv Helper Functions vvv
std::uint32_t TemplateEngine::GetVarNameId(TranspilationContext& ctx, const std::string& name)
{
    auto it = ctx.varNameMap.find(name);
    if(it == ctx.varNameMap.end()) {
        ctx.staticVarNames.emplace_back(std::move(name));
        std::uint32_t id = static_cast<std::uint32_t>(ctx.staticVarNames.size() - 1);
        ctx.varNameMap[name] = id;
        return id;
    }
    return it->second;
}

std::uint32_t TemplateEngine::GetConstId(TranspilationContext& ctx, const Value& val)
{
    auto it = ctx.constMap.find(val);
    if(it == ctx.constMap.end()) {
        ctx.staticConstants.emplace_back(std::move(val));
        std::uint32_t id = static_cast<std::uint32_t>(ctx.staticConstants.size() - 1);
        ctx.constMap[val] = id;
        return id;
    }
    return it->second;
}

TemplateEngine::TagResult TemplateEngine::ProcessTagIR(
    TranspilationContext& ctx, std::string_view tagView
)
{
    auto [tagName, tagArgs] = ExtractTag(tagView);
    if(tagName.empty()) {
        logger_.Error("[TemplateEngine].[CodeGen:IR]: Malformed tag (empty name)");
        return TagResult::FAILURE;
    }

    // For ease of use :)
    auto& ir           = ctx.ir;
    auto& ifPatchStack = ctx.ifPatchStack;

    if(tagName == "var") {
        // Parse the expression, get its index in the rpnPool
        auto [success, exprIndex] = ParseExpr(ctx, std::string(tagArgs));
        if(!success)
            return TagResult::FAILURE;

        ir.push_back({
            OpType::VAR,
            false,
            exprIndex // Payload is std::uint32_t (index to RPNBytecode)
        });
    }
    else if(tagName == "if") {
        // Parse the expression, get its index in the rpnPool
        auto [success, exprIndex] = ParseExpr(ctx, std::string(tagArgs));
        if(!success)
            return TagResult::FAILURE;

        // Create a new patch frame for this 'if' block
        ifPatchStack.push({});
        
        // Push the index of this op (which is the current size) onto the stack
        ifPatchStack.top().push_back(static_cast<std::uint32_t>(ir.size()));

        // Add the IF op, it needs patching
        ir.push_back({
            OpType::IF,
            true,
            ConditionalValue{ 0, exprIndex } // Payload is {jump_state, expr_index}
        });
    }
    else if(tagName == "elif") {
        if(ifPatchStack.empty()) {
            logger_.Error("[TemplateEngine].[CodeGen:IR]: Found 'elif' without 'if'");
            return TagResult::FAILURE;
        }
        if(ifPatchStack.top().empty()) {
            logger_.Error("[TemplateEngine].[CodeGen:IR]: Found 'elif' after 'else'");
            return TagResult::FAILURE;
        }

        // Add a JUMP op (to jump to 'endif' if the previous 'if' was true)
        // This JUMP also needs patching
        std::uint32_t jumpOpIndex = static_cast<std::uint32_t>(ir.size());
        ir.push_back({
            OpType::JUMP,
            true,
            std::uint32_t(0) // Payload is jump_state
        });
        ifPatchStack.top().push_back(jumpOpIndex);

        // Patch the previous 'if' or 'elif' to jump to this op
        std::uint32_t prevOpIndex = ifPatchStack.top().front();
        ifPatchStack.top().erase(ifPatchStack.top().begin());

        auto& prevOp = ir.at(prevOpIndex);
        prevOp.patch = false;
        // We know prevOp is IF or ELIF, so it has a ConditionalValue
        auto& conditionalPayload = std::get<ConditionalValue>(prevOp.payload);
        conditionalPayload.first = static_cast<std::uint32_t>(ir.size());

        // Parse this 'elif's expression
        auto [success, exprIndex] = ParseExpr(ctx, std::string(tagArgs));
        if(!success)
            return TagResult::FAILURE;

        // Add the ELIF op, it needs patching
        ifPatchStack.top().push_back(static_cast<std::uint32_t>(ir.size()));
        ir.push_back({
            OpType::ELIF,
            true,
            ConditionalValue{ 0, exprIndex } // Payload is {jump_state, expr_index}
        });
    }
    else if(tagName == "else") {
        if(ifPatchStack.empty()) {
            logger_.Error("[TemplateEngine].[CodeGen:IR]: Found 'else' without 'if'");
            return TagResult::FAILURE;
        }
        if(ifPatchStack.top().empty()) {
            logger_.Error("[TemplateEngine].[CodeGen:IR]: Found multiple 'else' tags");
            return TagResult::FAILURE;
        }

        // Add a JUMP op (to jump to endif)
        std::uint32_t jumpOpIndex = static_cast<std::uint32_t>(ir.size());
        ir.push_back({
            OpType::JUMP,
            true,
            std::uint32_t(0) // Payload is jump_state
        });

        // Get the index of the 'else' block
        std::uint32_t elseStateNum = static_cast<std::uint32_t>(ir.size());

        // Patch all previous 'if' and 'elif' ops to jump to this 'else' block
        for(std::uint32_t idx : ifPatchStack.top()) {
            auto& op = ir.at(idx);
            op.patch = false;

            if(op.type == OpType::IF || op.type == OpType::ELIF)
                std::get<ConditionalValue>(op.payload).first = elseStateNum;
            else if(op.type == OpType::JUMP)
                op.payload = elseStateNum; // Set the std::uint32_t payload
        }

        // Clear the stack, but add the new JUMP op index-
        // -as its the only one that needs to be patched by 'endif'
        ifPatchStack.top().clear();
        ifPatchStack.top().push_back(jumpOpIndex);

        // Add the ELSE marker op
        ir.push_back({
            OpType::ELSE,
            false,
            std::monostate{} // No payload
        });
    }
    else if(tagName == "endif") {
        if(ifPatchStack.empty()) {
            logger_.Error("[TemplateEngine].[CodeGen:IR]: Found 'endif' without 'if'");
            return {};
        }

        std::uint32_t endState = static_cast<std::uint32_t>(ir.size());

        // Patch all remaining jumps (from 'if', 'elif', or 'else')
        for(std::uint32_t idx : ifPatchStack.top()) {
            auto& op = ir.at(idx);
            op.patch = false;

            if(op.type == OpType::IF || op.type == OpType::ELIF)
                std::get<ConditionalValue>(op.payload).first = endState;
            else if(op.type == OpType::JUMP)
                op.payload = endState; // Set the std::uint32_t payload
        }

        ifPatchStack.pop(); // Pop this 'if' frame

        // Add the ENDIF marker op
        ir.push_back({
            OpType::ENDIF,
            false,
            std::monostate{} // No payload
        });
    }
    // Shouldn't happen btw
    else {
        logger_.Error("[TemplateEngine].[CodeGen:IR]: Unknown tag appeared: ", tagName);
        return TagResult::FAILURE;
    }

    return TagResult::SUCCESS;
}

// vvv Transpiler Functions vvv
//  vvv Parsing Functions vvv
TemplateEngine::ParseResult TemplateEngine::ParseExpr(
    TranspilationContext& ctx, std::string expression
)
{
    RPNBytecode outputQueue;
    std::stack<Legacy::Token> operatorStack;
    Legacy::Lexer lexer{std::move(expression)}; // My trusty old lexer :)

    auto& token = lexer.get_token();
    while(token.token_type != Legacy::TOKEN_EOF) {
        switch(token.token_type) {
            // --- Operands (Push directly to output) ---
            case Legacy::TOKEN_ID:
                outputQueue.push_back({
                    RPNOpCode::PUSH_VAR, 
                    GetVarNameId(ctx, token.token_value)
                });
                break;
            case Legacy::TOKEN_INT:
                outputQueue.push_back({
                    RPNOpCode::PUSH_CONST, 
                    GetConstId(ctx, std::stoll(token.token_value))
                });
                break;
            case Legacy::TOKEN_FLOAT:
                outputQueue.push_back({
                    RPNOpCode::PUSH_CONST, 
                    GetConstId(ctx, std::stod(token.token_value))
                });
                break;
            case Legacy::TOKEN_STRING:
                outputQueue.push_back({
                    RPNOpCode::PUSH_CONST, 
                    GetConstId(ctx, token.token_value)
                });
                break;

            // --- Parentheses ---
            case Legacy::TOKEN_LPAREN:
                operatorStack.push(token);
                break;

            case Legacy::TOKEN_RPAREN:
                while(
                    !operatorStack.empty()
                    && operatorStack.top().token_type != Legacy::TOKEN_LPAREN
                )
                    if(!PopOperator(operatorStack, outputQueue)) return { false, 0 };

                if(operatorStack.empty()) {
                    logger_.Error("[TemplateEngine].[CodeGen:EP]: Mismatched parentheses, '(' missing ')'");
                    return { false, 0 };
                }

                operatorStack.pop(); // Pop the '('
                break;
            
            // --- Dot Operator ---
            case Legacy::TOKEN_DOT: {
                // Expect identifier after '.'
                Legacy::Token attrToken = lexer.peek_next_token();
                if(attrToken.token_type != Legacy::TOKEN_ID) {
                    logger_.Error("[TemplateEngine].[CodeGen:EP]: Expected identifier after '.'");
                    return { false, 0 };
                }

                // Dot behaves as a binary operator
                std::uint32_t prec       = GetOperatorPrecedence(Legacy::TOKEN_DOT);
                bool          isRightAsc = IsRightAssociative(Legacy::TOKEN_DOT);

                while(!operatorStack.empty() && 
                    operatorStack.top().token_type != Legacy::TOKEN_LPAREN)
                {
                    std::uint32_t topPrec = GetOperatorPrecedence(operatorStack.top().token_type);
                    if((topPrec > prec || (topPrec == prec && !isRightAsc))) {
                        if(!PopOperator(operatorStack, outputQueue))
                            return { false, 0 };
                    }
                    else
                        break;
                }

                operatorStack.push(token);
                break;
            }

            // --- All Other Operators ---
            default:
                if(!IsOperator(token.token_type)) {
                    logger_.Error(
                        "[TemplateEngine].[CodeGen:EP]: Unexpected token in expression: ", token.token_value
                    );
                    return { false, 0 };
                }

                std::uint32_t prec       = GetOperatorPrecedence(token.token_type);
                bool          isRightAsc = IsRightAssociative(token.token_type);

                while(!operatorStack.empty() && 
                       operatorStack.top().token_type != Legacy::TOKEN_LPAREN)
                {
                    std::uint32_t topPrec = GetOperatorPrecedence(operatorStack.top().token_type);
                    if((topPrec > prec || (topPrec == prec && !isRightAsc))) {
                        if(!PopOperator(operatorStack, outputQueue))
                            return { false, 0 };
                    }
                    else
                        break;
                }
                operatorStack.push(token);
                break;
        }
        token = lexer.get_token();
    }

    while(!operatorStack.empty()) {
        if(operatorStack.top().token_type == Legacy::TOKEN_LPAREN) {
            logger_.Error("[TemplateEngine].[CodeGen:EP]: Mismatched parentheses, extra '('");
            return { false, 0 };
        }

        if(!PopOperator(operatorStack, outputQueue))
            return { false, 0 };
    }

    // Now that 'outputQueue' is complete, hash and pool it
    std::size_t hash = HashBytecode(outputQueue);
    
    auto it = ctx.rpnMap.find(hash);
    // We found a potential match, but still check if its the right one or not
    if(it != ctx.rpnMap.end()) {
        std::uint32_t idx = it->second;

        // Its a perfect match, return the existing index
        if(ctx.rpnPool[idx] == outputQueue)
            return { true, idx };
    }

    // Not found, add it to the pool
    ctx.rpnPool.push_back(std::move(outputQueue));
    std::uint32_t newIdx = static_cast<std::uint32_t>(ctx.rpnPool.size() - 1);
    ctx.rpnMap[hash] = newIdx;
    
    return { true, newIdx };
}

std::uint32_t TemplateEngine::GetOperatorPrecedence(Legacy::TokenType type)
{
    switch(type) {
        case Legacy::TOKEN_OR:
            return 1;
        case Legacy::TOKEN_AND:
            return 2;
        case Legacy::TOKEN_EEQ:
        case Legacy::TOKEN_NEQ:
            return 3;
        case Legacy::TOKEN_GT:
        case Legacy::TOKEN_GTEQ:
        case Legacy::TOKEN_LT:
        case Legacy::TOKEN_LTEQ:
            return 4;
        case Legacy::TOKEN_NOT:
            return 7;
        case Legacy::TOKEN_DOT:
            return 8;
        default:
            return 0;
    }
}

bool TemplateEngine::PopOperator(std::stack<Legacy::Token>& opStack, RPNBytecode& outputQueue)
{
    Legacy::TokenType type = opStack.top().token_type;
    if(!IsOperator(type)) {
        logger_.Error("[TemplateEngine].[CodeGen:EP]: Tried to pop non-operator token from stack: ", (int)type);
        opStack.pop();
        return false;
    }

    RPNOpCode op_code = TokenToOpCode(type);
    outputQueue.push_back({ op_code, 0 });
    opStack.pop();
    return true;
}

bool TemplateEngine::IsOperator(Legacy::TokenType type)
{
    switch(type) {
        case Legacy::TOKEN_OR:
        case Legacy::TOKEN_AND:
        case Legacy::TOKEN_EEQ:
        case Legacy::TOKEN_NEQ:
        case Legacy::TOKEN_GT:
        case Legacy::TOKEN_GTEQ:
        case Legacy::TOKEN_LT:
        case Legacy::TOKEN_LTEQ:
        case Legacy::TOKEN_NOT:
        case Legacy::TOKEN_DOT:
            return true;
        default:
            return false;
    }
}

bool TemplateEngine::IsRightAssociative(Legacy::TokenType type)
{
    return type == Legacy::TOKEN_NOT;
}

//  vvv Emitter Functions vvv
std::string TemplateEngine::GenerateCxxFromRPN(TranspilationContext& ctx, std::uint32_t rpnIndex)
{
    std::vector<std::vector<std::string>> pathStack;
    std::vector<std::string>              exprStack;

    // Helper lambda to select and get expr from stack
    auto GetExprFromStack = [&](std::string& out) -> void {
        if(!exprStack.empty()) {
            out = std::move(exprStack.back());
            exprStack.pop_back();
        }
        else if(!pathStack.empty()) {
            const auto& path = pathStack.back();
            out.reserve(32);

            out = "SafeGetJson(ctx, {";

            for(std::size_t i = 0; i < path.size(); ++i) {
                if(i)
                    out += ", ";
                out += "\"" + path[i] + "\"";
            }
            out += "})";
            pathStack.pop_back();
        }
    };

    for(const auto& op : ctx.rpnPool[rpnIndex]) {
        switch(op.code) {
            case RPNOpCode::PUSH_VAR: {
                pathStack.push_back({ ctx.staticVarNames[op.arg] });
                break;
            }

            case RPNOpCode::PUSH_CONST: {
                const auto& val = ctx.staticConstants[op.arg];
                if(std::holds_alternative<std::string>(val))
                    exprStack.push_back("\"" + std::get<std::string>(val) + "\"");
                else if(std::holds_alternative<std::int64_t>(val))
                    exprStack.push_back(std::to_string(std::get<std::int64_t>(val)));
                else if(std::holds_alternative<double>(val))
                    exprStack.push_back(std::to_string(std::get<double>(val)));
                else if(std::holds_alternative<bool>(val))
                    exprStack.push_back(std::get<bool>(val) ? "true" : "false");
                else
                    exprStack.push_back("null");
                break;
            }

            case RPNOpCode::OP_GET_ATTR: {
                if(pathStack.size() < 2)
                    break;

                auto rhs = std::move(pathStack.back()); pathStack.pop_back();
                auto lhs = std::move(pathStack.back()); pathStack.pop_back();

                lhs.insert(lhs.end(), rhs.begin(), rhs.end());
                pathStack.push_back(std::move(lhs));

                break;
            }

            case RPNOpCode::OP_AND:
            case RPNOpCode::OP_OR:
            case RPNOpCode::OP_EQ:
            case RPNOpCode::OP_NEQ:
            case RPNOpCode::OP_GT:
            case RPNOpCode::OP_GTE:
            case RPNOpCode::OP_LT:
            case RPNOpCode::OP_LTE: {
                std::string opStr;
                switch(op.code) {
                    case RPNOpCode::OP_AND: opStr = "&&"; break;
                    case RPNOpCode::OP_OR:  opStr = "||"; break;
                    case RPNOpCode::OP_EQ:  opStr = "=="; break;
                    case RPNOpCode::OP_NEQ: opStr = "!="; break;
                    case RPNOpCode::OP_GT:  opStr = ">";  break;
                    case RPNOpCode::OP_GTE: opStr = ">="; break;
                    case RPNOpCode::OP_LT:  opStr = "<";  break;
                    case RPNOpCode::OP_LTE: opStr = "<="; break;
                    default: break;
                }

                std::string lhs, rhs;

                GetExprFromStack(rhs);
                GetExprFromStack(lhs);

                if(!lhs.empty() && !rhs.empty())
                    exprStack.push_back("(" + lhs + " " + opStr + " " + rhs + ")");

                break;
            }

            case RPNOpCode::OP_NOT: {
                std::string out;
                GetExprFromStack(out);
                exprStack.push_back("(!" + out + ")");
                break;
            }

            default:
                break;
        }
    }

    // If we have a variable path, collapse to SafeGet
    if(!pathStack.empty() && !pathStack.back().empty()) {
        const auto& keys = pathStack.back();
        std::string outExpr = "SafeGetJson(ctx, {";
        
         for(std::size_t i = 0; i < keys.size(); ++i) {
            if(i)
                outExpr += ", ";
            outExpr += "\"" + keys[i] + "\"";
        }
        outExpr += "})";
        return outExpr;
    }

    if(exprStack.empty())
        return "false";

    return exprStack.back();
}

bool TemplateEngine::GenerateIRFromTemplate(TranspilationContext& ctx, const std::string& staticHtmlPath)
{
    auto& frame     = ctx.frame;
    auto  chunkSize = ctx.chunkSize;

    // For convinience + to properly reference tags across boundaries
    const char* bufPtr         = nullptr;
    std::size_t bufLen         = 0;
    std::size_t totalBytesRead = 0;
    std::string_view bodyView  = {};
    std::string_view tagView   = {};

    // Having to define here cuz of (my shitty coding) 'goto __ProcessTag';
    std::size_t tagStart   = 0;
    std::size_t literalEnd = 0;
    std::size_t tagEnd     = 0;

    // vvv Helper Lambdas vvv
    auto GetFilePos = [&]() -> std::size_t {
        return totalBytesRead + frame.readOffset;
    };

    auto FinalizeLiteral = [&]() {
        if(ctx.currentLiteralLength > 0) {
            ctx.ir.push_back({
                OpType::LITERAL,
                false,
                LiteralValue{
                    ctx.currentLiteralStartOffset,
                    ctx.currentLiteralLength
                }
            });
            ctx.currentLiteralLength = 0;
        }
    };

    // Main loop
    while(true) {
        if(frame.readOffset >= static_cast<size_t>(frame.bytesRead)) {
            // Finished processing previous chunk, update totalBytesRead
            totalBytesRead += frame.bytesRead;

            frame.bytesRead  = frame.file->Read(frame.readBuf.get(), chunkSize);
            frame.readOffset = 0;

            if(frame.bytesRead < 0) {
                logger_.Error("[TemplateEngine].[CodeGen:IR]: Failed to read from flat file: ", staticHtmlPath);
                return false;
            }

            if(frame.bytesRead == 0) {
                if(!frame.carry.empty()) {
                    logger_.Error("[TemplateEngine].[CodeGen:IR]: Incomplete tag at EOF: ", frame.carry);
                    return false;
                }
                break;
            }
        }

        bufPtr = frame.readBuf.get();
        bufLen = static_cast<std::size_t>(frame.bytesRead);

        // Handle carry from previous chunk
        if(!frame.carry.empty()) {
            bodyView = std::string_view(bufPtr + frame.readOffset, bufLen - frame.readOffset);

            // Check if 'frame.carry' starts with '{' and 'bodyView' starts with '%'
            // If not, its not a tag then
            if(frame.carry == "{" && bodyView[0] != '%') {
                if(ctx.currentLiteralLength == 0)
                    ctx.currentLiteralStartOffset = GetFilePos() - 1;

                ctx.currentLiteralLength += 1;

                frame.carry.clear();
                goto __DefaultReadLoop;
            }

            // Check if 'frame.carry' ends with '%' and 'bodyView' starts with '}'
            // If so, we can complete tag here and now
            else if(frame.carry.back() == '%' && bodyView[0] == '}') {
                frame.carry      += '}';
                frame.readOffset += 1;

                tagView = frame.carry;
                goto __ProcessTag;
            }

            // The normal way of finding tag end
            tagEnd = bodyView.find("%}");

            if(tagEnd == std::string_view::npos) {
                // How did we not find tag end even tho we are literally in second chunk?
                // Tf dawg, not again
                logger_.Error(
                    "[TemplateEngine].[CodeGen:IR]: We are in second chunk yet we couldn't find the tag end? Hell nah"
                );
                return false;
            }

            frame.carry.append(bodyView.data(), tagEnd + 2);
            frame.readOffset += tagEnd + 2;

            // vvv We don't add it, we go gg
            // Simply, we process tag, but before we do, handle any literal existing beforehand
            FinalizeLiteral();

            tagView = frame.carry;
            goto __ProcessTag;
        }

    __DefaultReadLoop:
        // Normal read loop
        while(frame.readOffset < bufLen) {
            bodyView = std::string_view(bufPtr + frame.readOffset, bufLen - frame.readOffset);
            tagStart = bodyView.find("{%");

            if(tagStart == std::string_view::npos) {
                // Possibly ends with '{'
                bool        maybeTag   = bodyView.back() == '{';
                std::size_t literalLen = maybeTag ? bodyView.size() - 1 : bodyView.size();

                if(ctx.currentLiteralLength == 0)
                    ctx.currentLiteralStartOffset = GetFilePos();

                ctx.currentLiteralLength += literalLen;
                frame.readOffset         += literalLen;

                if(maybeTag) {
                    frame.carry.assign("{");
                    frame.readOffset += 1;
                }

                break;
            }

            // Literal before tag
            if(tagStart > 0) {
                if(ctx.currentLiteralLength == 0)
                    ctx.currentLiteralStartOffset = GetFilePos();

                ctx.currentLiteralLength += tagStart;
                frame.readOffset         += tagStart;
            }

            // Finalize literal before processing tag
            FinalizeLiteral();

            // Process tag
            bodyView = std::string_view(bufPtr + frame.readOffset, bufLen - frame.readOffset);
            tagEnd   = bodyView.find("%}");

            if(tagEnd == std::string_view::npos) {
                frame.carry.assign(bodyView.data(), bodyView.size());
                frame.readOffset = bufLen;
                break;
            }

            tagView = bodyView.substr(0, tagEnd + 2);
            frame.readOffset += tagView.size();

        __ProcessTag:
            if(ProcessTagIR(ctx, tagView) == TagResult::FAILURE)
                return false;

            frame.carry.clear();
        }
    }

    // Finalize any trailing literal
    FinalizeLiteral();

    if(!ctx.ifPatchStack.empty()) {
        logger_.Error("[TemplateEngine].[CodeGen:IR]: Unmatched 'if' block, missing 'endif'");
        return false;
    }

    // Final check: ensure all patch flags are false
    for(std::size_t i = 0; i < ctx.ir.size(); i++) {
        if(ctx.ir[i].patch) {
            logger_.Error("[TemplateEngine].[CodeGen:IR]: Internal error: Unpatched jump target at state ", i);
            return false;
        }
    }

    logger_.Info("[TemplateEngine].[CodeGen:IR]: Successfully generated IR for: ", staticHtmlPath);
    return true;
}

bool TemplateEngine::GenerateCxxFromIR(
    TranspilationContext& ctx,
    const std::string& outCxxPath,
    const std::string& funcName
)
{
    auto& fs      = FileSystem::GetFileSystem();
    auto  outFile = fs.OpenFileWrite(outCxxPath.c_str());

    if(!outFile) {
        logger_.Error("[TemplateEngine].[CodeGen:CXX]: Failed to open output file: ", outCxxPath);
        return false;
    }
    
    // Yeah man this shits getting out of hand atp
    // Honestly fuck performance here not even going to bother, lets just hope compiler-
    // -optimizes this shit of a code
    IOContext ioCtx{std::move(outFile), ctx.chunkSize};
    const auto& irCode   = ctx.ir;
    bool needEndingBrace = false;
    bool writeResult     = true;

    // vvv EMIT HEADER vvv
    writeResult = SafeWrite(ioCtx, R"(/*
 * AUTO-GENERATED FILE, DO NOT MODIFY :)
 */
#include "engine/template_interface.hpp"
#include "shared/utils/export_macro.hpp"
#include <memory>
#include <variant>
#include <string>
#include <vector>

using Json = nlohmann::json;

// Output
using WFX::Core::StateResult;
using WFX::Core::FileChunk;
using WFX::Core::VariableChunk;

// Interface
using WFX::Core::BaseTemplateGenerator;

// Helper Functions
using WFX::Core::SafeGetJson;

)", 440);
    if(!writeResult) {
        logger_.Error("[TemplateEngine].[CodeGen:CXX]: Failed to write cxx header to: ", outCxxPath);
        return false;
    }

    // vvv EMIT GENERATOR CLASS vvv
    std::string generatorClass = 
        "class CLS" + funcName + " : public BaseTemplateGenerator {\n"
        "public:\n"
        "    CLS" + funcName + "() = default;\n\n"

        "    std::size_t GetStateCount() const noexcept override {\n"
        "        return " + UInt64ToStr(irCode.size()) + ";\n"
        "    }\n\n"

        "    StateResult GetState(std::size_t state, const Json& ctx) const noexcept override {\n"
        "        while(true) {\n"
        "            switch(state) {\n";

    writeResult = SafeWrite(ioCtx, generatorClass.c_str(), generatorClass.size());
    if(!writeResult) {
        logger_.Error("[TemplateEngine].[CodeGen:CXX]: Failed to write cxx generator class to: ", outCxxPath);
        return false;
    }

    // vvv EMIT IR-BASED SWITCH CASE vvv
    for(std::size_t i = 0; i < irCode.size(); ++i) {
        const auto& op = irCode[i];

        // Reset to default states every iteration
        needEndingBrace = true;
        writeResult     = true;

        // Initial 'case ...:' line
        std::string line = "                case " + UInt64ToStr(i) + ":";
        if(!SafeWrite(ioCtx, line.c_str(), line.size())) {
            logger_.Error("[TemplateEngine].[CodeGen:CXX]: Failed to write cxx switch case to: ", outCxxPath);
            return false;
        }

        switch(op.type) {
            case OpType::LITERAL: {
                needEndingBrace = false;
                auto [off, len] = std::get<LiteralValue>(op.payload);

                line = " return {" + UInt64ToStr(i + 1) +
                                ", FileChunk{" + UInt64ToStr(off) + ", " +
                                UInt64ToStr(len) + "}};\n";
                writeResult = SafeWrite(ioCtx, line.c_str(), line.size());

                break;
            }

            case OpType::VAR: {
                needEndingBrace = false;
                std::uint32_t exprIdx = std::get<std::uint32_t>(op.payload);

                line = " return {" + UInt64ToStr(i + 1) +
                                ", VariableChunk{ " + GenerateCxxFromRPN(ctx, exprIdx) +
                                " }};\n";
                writeResult = SafeWrite(ioCtx, line.c_str(), line.size());

                break;
            }

            case OpType::IF:
            case OpType::ELIF: {
                const auto& [jump, exprIdx] = std::get<ConditionalValue>(op.payload);
                std::string expr = GenerateCxxFromRPN(ctx, exprIdx);

                line =
                    " {\n"
                    "                    if(!(" + expr + ")) {\n"
                    "                        state = " + UInt64ToStr(jump) + ";\n"
                    "                        continue;\n"
                    "                    }\n"
                    "                    [[fallthrough]];\n";
                writeResult = SafeWrite(ioCtx, line.c_str(), line.size());

                break;
            }

            case OpType::ELSE:
            case OpType::ENDIF: {
                needEndingBrace = false;
                writeResult = SafeWrite(ioCtx, "\n", 1);
                break;
            }

            case OpType::JUMP: {
                std::uint32_t jump = std::get<std::uint32_t>(op.payload);

                line =
                    " {\n"
                    "                    state = " + UInt64ToStr(jump) + ";\n"
                    "                    continue;\n";
                writeResult = SafeWrite(ioCtx, line.c_str(), line.size());

                break;
            }

            default: {
                logger_.Error("[TemplateEngine].[CodeGen:CXX]: Unsupported bytecode detected: ", (int)op.type);
                return false;
            }
        }

        if(!writeResult) {
            logger_.Error("[TemplateEngine].[CodeGen:CXX]: Failed to write cxx expr to: ", outCxxPath);
            return false;
        }

        if(needEndingBrace)
            SafeWrite(ioCtx, "                }\n", 18);
    }

    // ------------------------------------------------------------------------
    // Emit class footer
    // ------------------------------------------------------------------------
    SafeWrite(ioCtx, 
R"(                default:
                    return {state, std::monostate{}};
            }
        }
    }
};
)", 112);

    // ------------------------------------------------------------------------
    // Emit factory
    // ------------------------------------------------------------------------
    std::string factory =
        "\n/*\n * Creates and returns an instance of the template generator\n */"
        "\nextern \"C\"\nWFX_EXPORT\nstd::unique_ptr<BaseTemplateGenerator> "
        + funcName + "() {\n"
        "    return std::make_unique<CLS" + funcName + ">();\n"
        "}";
    SafeWrite(ioCtx, factory.c_str(), factory.size());

    // Finalize
    FlushWrite(ioCtx, true);
    logger_.Info("[TemplateEngine].[CodeGen:CXX]: Generated C++ source: ", outCxxPath);
    return true;
}

bool TemplateEngine::GenerateCxxFromTemplate(
    const std::string& inHtmlPath, const std::string& outCxxPath, const std::string& funcName
)
{
    auto& fs = FileSystem::GetFileSystem();

    std::uint32_t chunkSize = config_.miscConfig.templateChunkSize;
    BaseFilePtr   inFile    = fs.OpenFileRead(inHtmlPath.c_str(), true);
    
    TranspilationContext ctx{std::move(inFile), chunkSize};

    if(!GenerateIRFromTemplate(ctx, inHtmlPath))
        return false;

    return GenerateCxxFromIR(ctx, outCxxPath, funcName);
}

void TemplateEngine::CompileCxxToLib(const std::string& inCxxDir, const std::string& outObjDir)
{
    auto& fs   = FileSystem::GetFileSystem();
    auto& proc = ProcessUtils::GetInstance();

    auto& toolchain = config_.toolchainConfig;

    // Input .cpp files will be from 'inCxxDir'
    // The final .dll / .so will be compiled to <project>/build/dlls/ folder (Name: user_templates.[so/dll])
    // The final .obj will be compiled to 'inObjDir'
    const std::string dllDir  = config_.projectConfig.projectName + "/build/dlls";
    const std::string dllPath = dllDir + "/user_templates.so";

    if(!fs.DirectoryExists(inCxxDir.c_str()))
        logger_.Fatal("[TemplateEngine].[CodeGen:OUT]: Failed to locate: ", inCxxDir);

    if(!fs.CreateDirectory(outObjDir))
        logger_.Fatal("[TemplateEngine].[CodeGen:OUT]: Failed to create obj dir: ", outObjDir);

    if(!fs.CreateDirectory(dllDir))
        logger_.Fatal("[TemplateEngine].[CodeGen:OUT]: Failed to create dll dir: ", dllDir);

    // Prebuild fixed portions of compiler and linker commands
    const std::string compilerBase = toolchain.ccmd + " " + toolchain.cargs + " ";
    const std::string objPrefix    = toolchain.objFlag + "\"";
    const std::string dllLinkTail  = toolchain.largs + " " + toolchain.dllFlag + "\"" + dllPath + '"';

    std::string linkCmd = toolchain.lcmd + " ";

    // Recurse through src/ files
    fs.ListDirectory(inCxxDir, true, [&](const std::string& cppFile) {
        if(!EndsWith(cppFile.c_str(), ".cpp") &&
            !EndsWith(cppFile.c_str(), ".cxx") &&
            !EndsWith(cppFile.c_str(), ".cc")) return;

        logger_.Info("[TemplateEngine].[CodeGen:OUT]: Compiling cxx/ file: ", cppFile);

        // Construct relative path
        std::string relPath = cppFile.substr(inCxxDir.size());
        if(!relPath.empty() && (relPath[0] == '/' || relPath[0] == '\\'))
            relPath.erase(0, 1);

        // Replace .cpp with .obj
        std::string objFile = outObjDir + "/" + relPath;
        objFile.replace(objFile.size() - 4, 4, ".obj");

        // Ensure obj subdir exists
        std::size_t slash = objFile.find_last_of("/\\");
        if(slash != std::string::npos) {
            std::string dir = objFile.substr(0, slash);
            if(!fs.DirectoryExists(dir.c_str()) && !fs.CreateDirectory(dir))
                logger_.Fatal("[TemplateEngine].[CodeGen:OUT]: Failed to create obj subdirectory: ", dir);
        }

        // Construct compile command
        std::string compileCmd = compilerBase + "\"" + cppFile + "\" " + objPrefix + objFile + "\"";
        auto result = proc.RunProcess(compileCmd);
        if(result.exitCode != 0)
            logger_.Fatal("[TemplateEngine].[CodeGen:OUT]: Compilation failed for: ", cppFile, ". OS code: ", result.osCode);

        // Append obj to link command
        linkCmd += "\"" + objFile + "\" ";
    });

    // Final link command
    linkCmd += dllLinkTail;

    auto linkResult = proc.RunProcess(linkCmd);
    if(linkResult.exitCode != 0)
        logger_.Fatal("[TemplateEngine].[CodeGen:OUT]: Linking failed. DLL not created. OS code: ", linkResult.osCode);

    logger_.Info("[TemplateEngine].[CodeGen:OUT]: Templates successfully compiled to ", dllDir);
}

//  vvv Helper Functions vvv
TemplateEngine::RPNOpCode TemplateEngine::TokenToOpCode(Legacy::TokenType type)
{
    switch(type) {
        case Legacy::TOKEN_AND:  return RPNOpCode::OP_AND;
        case Legacy::TOKEN_OR:   return RPNOpCode::OP_OR;
        case Legacy::TOKEN_EEQ:  return RPNOpCode::OP_EQ;
        case Legacy::TOKEN_NEQ:  return RPNOpCode::OP_NEQ;
        case Legacy::TOKEN_GT:   return RPNOpCode::OP_GT;
        case Legacy::TOKEN_GTEQ: return RPNOpCode::OP_GTE;
        case Legacy::TOKEN_LT:   return RPNOpCode::OP_LT;
        case Legacy::TOKEN_LTEQ: return RPNOpCode::OP_LTE;
        case Legacy::TOKEN_NOT:  return RPNOpCode::OP_NOT;
        case Legacy::TOKEN_DOT:  return RPNOpCode::OP_GET_ATTR;
        default:
            logger_.Fatal("[TemplateEngine].[CodeGen:EP]: Unknown operator type: ", (int)type);
            return RPNOpCode::OP_AND; // Just to suppress compiler warning
    }
}

std::uint64_t TemplateEngine::HashBytecode(const RPNBytecode& rpn)
{
    std::uint64_t seed = rpn.size();

    for(const auto& op : rpn) {
        seed = HashUtils::Rotl(seed, std::numeric_limits<std::uint64_t>::digits / 3)
                ^ HashUtils::Distribute(static_cast<std::uint64_t>(op.code));

        seed = HashUtils::Rotl(seed, std::numeric_limits<std::uint64_t>::digits / 3)
                ^ HashUtils::Distribute(static_cast<std::uint64_t>(op.arg));
    }

    return seed;
}

} // namespace WFX::Core