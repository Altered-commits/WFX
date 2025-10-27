#include "template_engine.hpp"
#include "config/config.hpp"
#include "utils/crypt/hash.hpp"
#include "utils/backport/string.hpp"
#include <stack>

namespace WFX::Core {

// vvv Helper Functions vvv
std::uint32_t TemplateEngine::GetVarNameId(IRContext& ctx, const std::string& name)
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

std::uint32_t TemplateEngine::GetConstId(IRContext& ctx, const Value& val)
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

TemplateEngine::TagResult TemplateEngine::ProcessTagIR(IRContext& ctx, std::string_view tagView)
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
        std::uint32_t varId = GetVarNameId(ctx, std::string(tagArgs));
        ir.push_back({
            OpType::VAR,
            false,
            varId // Payload is std::uint32_t
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
TemplateEngine::ParseResult TemplateEngine::ParseExpr(IRContext& ctx, std::string expression)
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
                    PopOperator(operatorStack, outputQueue);

                if(operatorStack.empty()) {
                    logger_.Error("[TemplateEngine].[CodeGen:EP]: Mismatched parentheses, '(' missing ')'");
                    return { false, 0 };
                }

                operatorStack.pop(); // Pop the '('
                break;
            
            // --- Dot Operator ---
            case Legacy::TOKEN_DOT: {
                Legacy::Token attrToken = lexer.get_token();
                if(attrToken.token_type != Legacy::TOKEN_ID) {
                    logger_.Error("[TemplateEngine].[CodeGen:EP]: Expected identifier after '.'");
                    return { false, 0 };
                }

                outputQueue.push_back({
                    RPNOpCode::PUSH_CONST,
                    GetConstId(ctx, attrToken.token_value)
                });

                std::uint32_t prec = GetOperatorPrecedence(token.token_type);
                while(!operatorStack.empty() && 
                       operatorStack.top().token_type != Legacy::TOKEN_LPAREN)
                {
                    std::uint32_t topPrec = GetOperatorPrecedence(operatorStack.top().token_type);
                    if(topPrec > prec)
                        PopOperator(operatorStack, outputQueue);
                    else
                        break;
                }
                operatorStack.push(token);
                
                token = lexer.get_token();
                continue;
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
                    if(topPrec > prec || (topPrec == prec && !isRightAsc))
                        PopOperator(operatorStack, outputQueue);
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

        PopOperator(operatorStack, outputQueue);
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

//  vvv Generating Functions vvv
TemplateEngine::IRCode TemplateEngine::GenerateIRFromTemplate(
    const std::string& staticHtmlPath
)
{
    auto& fs     = FileSystem::GetFileSystem();
    auto& config = Config::GetInstance();

    std::uint32_t chunkSize = config.miscConfig.templateChunkSize;

    BaseFilePtr inFile = fs.OpenFileRead(staticHtmlPath.c_str(), true);

    if(!inFile) {
        logger_.Error("[TemplateEngine].[CodeGen:IR]: Failed to open static file: ", staticHtmlPath);
        return {};
    }

    IRContext ctx{std::move(inFile), chunkSize};
    auto& frame = ctx.frame;

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
                return {};
            }

            if(frame.bytesRead == 0) {
                if(!frame.carry.empty()) {
                    logger_.Error("[TemplateEngine].[CodeGen:IR]: Incomplete tag at EOF: ", frame.carry);
                    return {};
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
                return {};
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
                return {};

            frame.carry.clear();
        }
    }

    // Finalize any trailing literal
    FinalizeLiteral();

    if(!ctx.ifPatchStack.empty()) {
        logger_.Error("[TemplateEngine].[CodeGen:IR]: Unmatched 'if' block, missing 'endif'");
        return {};
    }

    // Final check: ensure all patch flags are false
    for(std::size_t i = 0; i < ctx.ir.size(); i++) {
        if(ctx.ir[i].patch) {
            logger_.Error("[TemplateEngine].[CodeGen:IR]: Internal error: Unpatched jump target at state ", i);
            return {};
        }
    }

    logger_.Info("[TemplateEngine].[CodeGen:IR]: Successfully generated IR for: ", staticHtmlPath);
    return ctx.ir;
}

bool TemplateEngine::GenerateCxxFromIR(
    const std::string& outCxxPath, const std::string& funcName, std::vector<Op>&& irCode
)
{
    return false;
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
            logger_.Fatal("[TemplateEngine].[CodeGen:EP]: Unknown operator token type");
            return RPNOpCode::OP_AND; // Just to suppress compiler warning
    }
}

void TemplateEngine::PopOperator(std::stack<Legacy::Token>& opStack, RPNBytecode& outputQueue)
{
    RPNOpCode op_code = TokenToOpCode(opStack.top().token_type);
    outputQueue.push_back({op_code, 0});
    opStack.pop();
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