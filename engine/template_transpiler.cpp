#include "template_engine.hpp"
#include "config/config.hpp"
#include <stack>

namespace WFX::Core {

// vvv Helper Functions vvv
std::uint32_t TemplateEngine::GetVarNameId(IRContext& ctx, const std::string& name)
{
    auto it = ctx.varNameMap.find(name);
    if(it == ctx.varNameMap.end()) {
        ctx.staticVarNames.push_back(name);
        std::uint32_t id = static_cast<std::uint32_t>(ctx.staticVarNames.size() - 1);
        ctx.varNameMap[name] = id;
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
        uint32_t varId = GetVarNameId(ctx, std::string(tagArgs));
        ir.push_back({
            OpType::VAR,
            false,
            std::to_string(varId),
            0, 0,
            ctx.currentState++, 0
        });
    }
    else if(tagName == "if") {
        std::uint32_t varId = GetVarNameId(ctx, std::string(tagArgs));
        ifPatchStack.push({});
        ifPatchStack.top().push_back(ctx.currentState);
        ir.push_back({
            OpType::IF,
            true,
            std::to_string(varId),
            0, 0,
            ctx.currentState++, 0
        });
    }
    else if(tagName == "elif") {
        if(ifPatchStack.empty()) {
            logger_.Error("[TemplateEngine].[CodeGen:IR]: Found 'elif' without 'if'");
            TagResult::FAILURE;
        }
        if(ifPatchStack.top().empty()) {
            logger_.Error("[TemplateEngine].[CodeGen:IR]: Found 'elif' after 'else'");
            TagResult::FAILURE;
        }

        std::uint32_t jumpIdx = ctx.currentState;
        ir.push_back({
            OpType::JUMP,
            true,
            "", 0, 0,
            ctx.currentState++, 0
        });
        ifPatchStack.top().push_back(jumpIdx);

        std::uint32_t patchIdx = ifPatchStack.top().front();
        ifPatchStack.top().erase(ifPatchStack.top().begin());

        // Patch it
        auto& op = ir.at(patchIdx);
        op.targetState = ctx.currentState;
        op.patch       = false;

        std::uint32_t varId = GetVarNameId(ctx, std::string(tagArgs));
        ifPatchStack.top().push_back(ctx.currentState);
        ir.push_back({
            OpType::ELIF,
            true,
            std::to_string(varId),
            0, 0,
            ctx.currentState++, 0
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

        std::uint32_t jumpIdx = ctx.currentState;
        ir.push_back({
            OpType::JUMP,
            true,
            "", 0, 0,
            ctx.currentState++, 0
        });
        ifPatchStack.top().push_back(jumpIdx);

        std::uint32_t elseStateNum = ctx.currentState;
        for(std::uint32_t idx : ifPatchStack.top()) {
            if(idx >= ir.size()) {
                logger_.Error("[TemplateEngine].[CodeGen:IR]: Invalid patch index for 'else'");
                return TagResult::FAILURE;
            }
            auto& op = ir.at(idx);
            op.targetState = elseStateNum;
            op.patch       = false;
        }
        ifPatchStack.top().clear();

        ir.push_back({
            OpType::ELSE,
            false,
            "", 0, 0,
            ctx.currentState++, 0
        });
    }
    else if(tagName == "endif") {
        if(ifPatchStack.empty()) {
            logger_.Error("[TemplateEngine].[CodeGen:IR]: Found 'endif' without 'if'");
            return {};
        }

        std::uint32_t endState = ctx.currentState;
        for(std::uint32_t idx : ifPatchStack.top()) {
            if(idx >= ir.size()) {
                logger_.Error("[TemplateEngine].[CodeGen:IR]: Invalid patch index for 'endif'");
                return {};
            }
            auto& op = ir.at(idx);
            op.targetState = endState;
            op.patch       = false;
        }
        ifPatchStack.pop();
        ir.push_back({
            OpType::ENDIF,
            false,
            "", 0, 0,
            ctx.currentState++, 0
        });
    }
    // Shouldn't happen btw
    else {
        logger_.Error("[TemplateEngine].[CodeGen:IR]: Unknown tag appeared: ", tagName);
        return TagResult::FAILURE;
    }

    return TagResult::SUCCESS;
}

// vvv Main Functions vvv
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
    const char* bufPtr = nullptr;
    std::size_t bufLen = 0;
    std::string_view tagView{};

    // Having to define here cuz of (my shitty coding) 'goto __ProcessTag';
    std::size_t tagStart           = 0;
    std::size_t literalEnd         = 0;
    std::size_t tagEnd             = 0;
    std::string_view remainingView = {};

    // Helper to finalize the current literal Op being built
    auto FinalizeLiteral = [&]() {
        if(ctx.currentLiteralLength > 0) {
            ctx.ir.push_back({
                OpType::LITERAL,
                false,
                "",
                ctx.currentLiteralStartOffset,
                ctx.currentLiteralLength,
                ctx.currentState++,
                0
            });
            ctx.currentLiteralLength = 0;
        }
    };

    // Main loop
    while(true) {
        // Refill buffer if needed
        if(frame.readOffset >= static_cast<size_t>(frame.bytesRead)) {
            frame.bytesRead  = frame.file->Read(frame.readBuf.get(), chunkSize);
            frame.readOffset = 0;

            if(frame.bytesRead < 0) {
                logger_.Error("[TemplateEngine].[CodeGen:IR]: Failed to read from flat file: ", staticHtmlPath);
                return {};
            }

            // EOF, No carry should exist btw
            if(frame.bytesRead == 0) {
                if(!frame.carry.empty()) {
                    logger_.Error("[TemplateEngine].[CodeGen:IR]: Incomplete tag at end of file: ", frame.carry);
                    return {};
                }
                break;
            }
        }

        bufPtr = frame.readBuf.get();
        bufLen = static_cast<std::size_t>(frame.bytesRead);

        // CASE 1: Handle carryover from previous chunk
        if(!frame.carry.empty()) {
           // Search for "%}" in the rest of the buffer
            std::string_view tailView(bufPtr, bufLen);

            std::size_t foundEnd = tailView.find("%}");
            if(foundEnd == std::string_view::npos) {
                // So ur telling me that we just started this chunk and we couldn't find the tag end?
                // Dawg fuck no
                logger_.Error(
                    "[TemplateEngine].[CodeGen:IR]: Couldn't find tags end in this chunk, it started in previous chunk. Tag: ",
                    frame.carry
                );
                return {};
            }

            // Found tag end in this chunk, but before we append, check the length of tag
            // It cannot cross maxTagLength_
            std::size_t appendCount = foundEnd + 2;
            if(frame.carry.size() + appendCount > maxTagLength_) {
                logger_.Error(
                    "[TemplateEngine].[CodeGen:IR]: Length of the tag: '",
                    frame.carry,
                    "' crosses the maxTagLength_ limit which is ", maxTagLength_
                );
                return {};
            }

            frame.carry.append(bufPtr, appendCount);
            frame.readOffset = appendCount;

            tagView = frame.carry;

            goto __ProcessTag;
        }

        // CASE 2: Normal read processing of current chunk
        while(frame.readOffset < bufLen) {
            remainingView = {bufPtr + frame.readOffset, bufLen - frame.readOffset};
            
            tagStart   = remainingView.find("{%");
            literalEnd = (tagStart == std::string_view::npos)
                ? remainingView.length()
                : tagStart;

            // Handle literal region
            if(literalEnd > 0) {
                if(ctx.currentLiteralLength == 0)
                    ctx.currentLiteralStartOffset = frame.file->Tell() - bufLen + frame.readOffset;

                ctx.currentLiteralLength += literalEnd;
                frame.readOffset += literalEnd;
            }

            if(tagStart == std::string_view::npos)
                break;

            FinalizeLiteral();

            // Re-calculate the remaining view so following logic can be relative to the current pos
            remainingView = { bufPtr + frame.readOffset, bufLen - frame.readOffset };

            tagEnd = remainingView.find("%}");
            if(tagEnd == std::string_view::npos) {
                frame.carry.assign(remainingView.data(), remainingView.size());
                frame.readOffset = bufLen;
                goto __NextChunk;
            }

            tagView = remainingView.substr(0, tagEnd + 2);

        __ProcessTag:
            TemplateEngine::TagResult res = ProcessTagIR(ctx, tagView);
            if(res == TagResult::FAILURE)
                return {};

            if(!frame.carry.empty())
                frame.carry.clear();
            else
                frame.readOffset += tagView.length();

            ctx.currentLiteralStartOffset = frame.file->Tell() - bufLen + frame.readOffset;
            ctx.currentLiteralLength      = 0;
        }

        // Same logic as 'CompileTemplate' function in template_engine.cpp
    __NextChunk:
        ;
    }

    // Finalize any trailing literal at the end of the file
    FinalizeLiteral();

    if(!ctx.ifPatchStack.empty()) {
        logger_.Error("[TemplateEngine].[CodeGen:IR]: Unmatched 'if' block, missing 'endif'");
        return {};
    }

    // Final check: ensure all patch flags are false
    for(const auto& op : ctx.ir) {
        if(op.patch) {
            logger_.Error("[TemplateEngine].[CodeGen:IR]: Internal error: Unpatched jump target at state ", op.stateNum);
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

} // namespace WFX::Core