#include "template_engine.hpp"

#include "config/config.hpp"
#include "utils/backport/string.hpp"
#include "utils/crypt/string.hpp"
#include <cstring>
#include <deque>

namespace WFX::Core {

TemplateEngine& TemplateEngine::GetInstance()
{
    static TemplateEngine engine;
    return engine;
}

// vvv Main Functions vvv
bool TemplateEngine::LoadTemplatesFromCache()
{
    auto& config = Config::GetInstance();
    auto& fs     = FileSystem::GetFileSystem();
    
    std::string   cacheFile = config.projectConfig.projectName + cacheFile_;
    std::uint16_t chunkSize = config.miscConfig.cacheChunkSize;

    auto inCache = fs.OpenFileRead(cacheFile.c_str(), true);
    // No cache, tell server to recompile templates
    if(!inCache)
        return false;

    templates_.clear();
    IOContext ctx = { std::move(inCache), chunkSize };

    std::uint64_t totalTemplates = 0;
    std::int64_t  bytesRead = 0;

    // Lambda to read primitive values safely from buffer, refilling if needed
    auto ReadValue = [&](void* dst, std::size_t size) -> bool {
        std::size_t copied = 0;
        char* outPtr = static_cast<char*>(dst);

        while(copied < size) {
            if(ctx.offset >= static_cast<std::uint32_t>(bytesRead)) {
                bytesRead = ctx.file->Read(ctx.buffer.get(), chunkSize);
                ctx.offset = 0;
                if(bytesRead <= 0)
                    return false;
            }

            std::size_t available = bytesRead - ctx.offset;
            std::size_t toCopy    = std::min(size - copied, available);

            std::memcpy(outPtr + copied, ctx.buffer.get() + ctx.offset, toCopy);

            copied     += toCopy;
            ctx.offset += toCopy;
        }
        return true;
    };

    // Read number of templates
    if(!ReadValue(&totalTemplates, sizeof(totalTemplates)))
        goto __Failure;

    // Format given in 'SaveTemplatesToCache' function: Line 121
    for(std::uint64_t i = 0; i < totalTemplates; ++i) {
        std::uint64_t relLen = 0;
        if(!ReadValue(&relLen, sizeof(relLen)))       goto __Failure;

        std::string relPath(relLen, '\0');
        if(!ReadValue(relPath.data(), relLen))        goto __Failure;

        std::uint8_t typeInt = 0;
        if(!ReadValue(&typeInt, sizeof(typeInt)))     goto __Failure;

        std::uint64_t sizeBytes = 0;
        if(!ReadValue(&sizeBytes, sizeof(sizeBytes))) goto __Failure;

        std::uint64_t fullLen = 0;
        if(!ReadValue(&fullLen, sizeof(fullLen)))     goto __Failure;

        std::string fullPath(fullLen, '\0');
        if(!ReadValue(fullPath.data(), fullLen))      goto __Failure;

        templates_.emplace(
            std::move(relPath),
            TemplateMeta{static_cast<TemplateType>(typeInt), sizeBytes, std::move(fullPath)}
        );
    }

    logger_.Info("[TemplateEngine]: Successfully loaded template data from cache.bin");
    return true;

__Failure:
    logger_.Error("[TemplateEngine]: Failed to read template data from cache.bin");
    ctx.file->Close();

    // We don't want corrupted cache lying around randomly
    if(!fs.DeleteFile(cacheFile.c_str()))
        logger_.Error("[TemplateEngine]: Failed to delete corrupted cache.bin");

    return false;
}

void TemplateEngine::SaveTemplatesToCache()
{
    if(!resaveCacheFile_)
        return;

    auto& config = Config::GetInstance();
    auto& fs     = FileSystem::GetFileSystem();

    std::string   cacheFile = config.projectConfig.projectName + cacheFile_;
    std::uint16_t chunkSize = config.miscConfig.cacheChunkSize;

    auto outCache = fs.OpenFileWrite(cacheFile.c_str(), true);
    if(!outCache) {
        logger_.Error("[TemplateEngine]: Failed to open cache file for writing: ", cacheFile);
        return;
    }
    /*
     * Format of cache.bin:
     * - uint64_t: Number of templates
     * - [for each template:]
     *     - uint64_t: Length of relative path
     *     - char[]:   Relative path
     *     - uint8_t:  TemplateType enum value
     *     - uint64_t: Size of the template file in bytes
     *     - uint64_t: Length of full path
     *     - char[]:   Full path
     */
    IOContext ctx = { std::move(outCache), chunkSize };

    // 1) Number of templates
    std::uint64_t numTemplates = templates_.size();
    if(!SafeWrite(ctx, &numTemplates, sizeof(numTemplates)))
        return;

    // 2. Serialize each template; the lambda handles flushing automatically.
    for(const auto& [relativePath, meta] : templates_) {
        // Write Relative Path (key)
        std::uint64_t relativePathLen = relativePath.length();
        if(!SafeWrite(ctx, &relativePathLen, sizeof(relativePathLen))) goto __Failure;
        if(!SafeWrite(ctx, relativePath.data(), relativePathLen))      goto __Failure;

        // Write Template Meta (value)
        auto typeAsInt    = static_cast<std::uint8_t>(meta.type);
        auto templateSize = static_cast<std::uint64_t>(meta.size);
        if(!SafeWrite(ctx, &typeAsInt, sizeof(typeAsInt)))             goto __Failure;
        if(!SafeWrite(ctx, &templateSize, sizeof(templateSize)))       goto __Failure;

        // Write Full Path (from meta)
        std::uint64_t fullPathLen = meta.fullPath.length();
        if(!SafeWrite(ctx, &fullPathLen, sizeof(fullPathLen)))         goto __Failure;
        if(!SafeWrite(ctx, meta.fullPath.data(), fullPathLen))         goto __Failure;
    }

    // Any extra data which still remains, flush it
    if(!FlushWrite(ctx, true))
        goto __Failure;

    logger_.Info("[TemplateEngine]: Successfully wrote template data to cache.bin");
    return;

__Failure:
    logger_.Error("[TemplateEngine]: Failed to write template data to cache.bin");
    ctx.file->Close();

    // We don't want corrupted cache lying around randomly
    if(!fs.DeleteFile(cacheFile.c_str()))
        logger_.Error("[TemplateEngine]: Failed to delete corrupted cache.bin");
}

void TemplateEngine::PreCompileTemplates()
{
    auto& fs     = FileSystem::GetFileSystem();
    auto& config = Config::GetInstance();

    std::size_t        errors    = 0;
    const std::string& inputDir            = config.projectConfig.templateDir;
    const std::string  staticOutputDir     = config.projectConfig.projectName + staticFolder_;
    const std::string  dynamicCppOutputDir = config.projectConfig.projectName + dynamicCppFolder_;
    const std::string  dynamicObjOutputDir = config.projectConfig.projectName + dynamicObjFolder_;

    // For partial tag checking. If u see it, don't compile the .html file
    // + 1 is redundant btw, but im still keeping it to cover my paranoia
    char temp[partialTagSize_ + 1] = { 0 };
    
    resaveCacheFile_ = true;

    if(!fs.DirectoryExists(staticOutputDir.c_str()) && !fs.CreateDirectory(staticOutputDir, true))
        logger_.Fatal("[TemplateEngine]: Failed to create static directory: ", staticOutputDir);

    // Dynamic templates have 2 folders, their C++ representation in cpp/ and compiled obj files in objs/
    if(!fs.DirectoryExists(dynamicCppOutputDir.c_str()) && !fs.CreateDirectory(dynamicCppOutputDir, true))
        logger_.Fatal("[TemplateEngine]: Failed to create dynamic-cpp directory: ", dynamicCppOutputDir);

    if(!fs.DirectoryExists(dynamicObjOutputDir.c_str()) && !fs.CreateDirectory(dynamicObjOutputDir, true))
        logger_.Fatal("[TemplateEngine]: Failed to create dynamic-obj directory: ", dynamicObjOutputDir);
    
    logger_.Info("[TemplateEngine]: Starting template precompilation from: ", inputDir);

    // Traverse the entire template directory looking for .html files
    // Then compile those html files into /build/templates/(static | dynamic)/
    fs.ListDirectory(inputDir, true, [&](std::string inPath) {
        if(!EndsWith(inPath, ".html") && !EndsWith(inPath, ".htm"))
            return;
        
        logger_.Info("[TemplateEngine]: Compiling template: ", inPath);

        // Strip leading slash cuz we will use this as key inside of templates_ map
        // We expect that user inside of SendTemplate function inputs path without leading slash
        // And because SendTemplate uses templates_ map, we cannot use leading slash
        std::string relPath = std::string(inPath.begin() + inputDir.size(), inPath.end());
        relPath.erase(0, relPath.find_first_not_of("/\\"));

        // Every file is initially written to the static folder, even the dynamic .html files.
        // After the .html file is completely stripped off static tags, and IF dynamic tags still-
        // -remain, we move onto stage two of compiling. That is when we use the dynamic folder
        const std::string outPath = staticOutputDir + "/" + relPath;

        // Ensure target directory exists
        std::string relOutputDir = outPath.substr(0, outPath.find_last_of("/\\"));
        if(!fs.DirectoryExists(relOutputDir.c_str()) && !fs.CreateDirectory(relOutputDir, true)) {
            logger_.Error("[TemplateEngine]: Failed to create template output directory: ", staticOutputDir);
            return;
        }

        auto in = fs.OpenFileRead(inPath.c_str());
        if(!in) {
            errors++;
            logger_.Error("[TemplateEngine]: Failed to open input template file: ", inPath);
            return;
        }
        auto inSize = in->Size();

        // File empty
        if(inSize == 0)
            return;

        // Partial tag can exist, check its existence first
        if(inSize >= partialTagSize_) {        
            if(in->Read(temp, partialTagSize_) < 0) {
                errors++;
                logger_.Error("[TemplateEngine]: Failed to read the first 14 bytes");
                return;
            }

            // No need to compile
            if(StartsWith(temp, partialTag_))
                return;

            // Seek back to start to not miss those bytes we skipped
            if(!in->Seek(0)) {
                errors++;
                logger_.Error("[TemplateEngine]: Failed to revert back to the start, lost 14 bytes");
                return;
            }
        }

        auto out = fs.OpenFileWrite(outPath.c_str());
        if(!out) {
            errors++;
            logger_.Error("[TemplateEngine]: Failed to open output template file: ", outPath);
            return;
        }

        // TODO: Delete template build if compilation failed
        auto [type, outSize] = CompileTemplate(std::move(in), std::move(out));
        if(type == TemplateType::FAILURE)
            errors++;
        
        // Add it to our template map
        else if(type == TemplateType::STATIC)
            templates_.emplace(
                std::move(relPath), TemplateMeta{type, outSize, std::move(outPath)}
            );

        // Websites dynamic, sigh, get ready for some fuckery
        else {
            logger_.Info("[TemplateEngine]: Staging dynamic template for compilation: ", relPath);
            // Create a unique, C compatible function name
            std::string funcName = 
                StringSanitizer::NormalizePathToIdentifier(relPath, dynamicTemplateFuncPrefix_);

            // Define path for the new .cpp file
            std::string cppPath = dynamicCppOutputDir + "/" + relPath + ".cpp";

            auto irCode = GenerateIRFromTemplate(outPath);
            if(!irCode.empty())
                logger_.Info("[TemplateEngine-Debug]: Output function name: ", funcName, ", Output path: ", cppPath);
        }
    });

    if(errors > 0)
        logger_.Warn("[TemplateEngine]: Template compilation complete with ", errors, " error(s)");
    else
        logger_.Info("[TemplateEngine]: Template compilation completed successfully");
}

TemplateMeta* TemplateEngine::GetTemplate(std::string&& relPath)
{
    auto templateMeta = templates_.find(std::move(relPath));
    if(templateMeta != templates_.end())
        return &(templateMeta->second);

    return nullptr;
}

// vvv Helper Functions vvv
TemplateResult TemplateEngine::CompileTemplate(BaseFilePtr inTemplate, BaseFilePtr outTemplate)
{
    std::uint32_t      chunkSize = Config::GetInstance().miscConfig.templateChunkSize;
    CompilationContext ctx       = { std::move(outTemplate), chunkSize };

    // Initialize stack with main template
    ctx.stack.emplace_back(std::move(inTemplate), chunkSize);
    auto& stack = ctx.stack;

    // Pre declare variables to allow my shitty code (goto __ProcessTag) to compile
    // These are reused and aren't bound to a specific frame so no issue tho
    bool isExtending  = false;
    bool skipLiterals = false;

    std::size_t      outSize  = 0;
    std::size_t      tagStart = 0;
    std::size_t      tagEnd   = 0;
    std::string_view bodyView = {};

    // Used by either tag [inside of frame.carry or inside of frame.readBuf]
    std::string_view tagView{};

    while(!stack.empty()) {
        TemplateFrame& frame = stack.back();

        // If we have any readOffset remaining, complete it before reading in more data
        if(frame.readOffset > 0 && frame.bytesRead > 0)
            goto __ContinueReading;

        frame.bytesRead = frame.file->Read(frame.readBuf.get(), ctx.chunkSize);
        if(frame.bytesRead < 0)
            return { TemplateType::FAILURE, 0 };

        if(frame.bytesRead == 0) {
            if(
                !frame.carry.empty()
                && !SafeWrite(ctx.io, frame.carry.data(), frame.carry.size())
            )
                return { TemplateType::FAILURE, 0 };

            // Remaining data to be flushed
            if(!FlushWrite(ctx.io, true))
                return { TemplateType::FAILURE, 0 };

            ctx.stack.pop_back();

            // If current frame had an extends file, push it now
            if(!ctx.currentExtendsName.empty()) {
                PushFile(ctx, ctx.currentExtendsName);
                ctx.currentExtendsName.clear();
            }
            continue;
        }

__ContinueReading:
        // Convinience purpose
        char*       bufPtr = frame.readBuf.get();
        std::size_t bufLen = static_cast<std::size_t>(frame.bytesRead);

        // CASE 0: If the first 13 bytes are {% partial %}, skip them + skip '\n' with +1
        // Quite strict ({% partial %} needs to be written perfectly)
        if(frame.firstRead) {
            frame.firstRead = false;
            if(
                frame.bytesRead >= partialTagSize_
                && StartsWith(std::string_view(bufPtr, partialTagSize_), partialTag_)
            )
                frame.readOffset += partialTagSize_ + 1;
        }

        // CASE 1: Tag incomplete from previous frame
        if(!frame.carry.empty()) {
            bodyView = {bufPtr, bufLen};

            // Check if we chunked at '{' to '%'
            if(frame.carry == "{" && bodyView[0] != '%') {
                // Not a tag
                if(!SafeWrite(ctx.io, frame.carry.c_str(), frame.carry.size()))
                    return { TemplateType::FAILURE, 0 };
                frame.carry.clear();
                goto __DefaultChunkProcessing;
            }

            // Now before we do a 'find()', check if 'frame.carry' ends with '%'
            else if(frame.carry.back() == '%' && bodyView[0] == '}') {
                // Tag is complete
                frame.carry      += '}';
                frame.readOffset += 1;

                // Check max length
                if(frame.carry.size() > maxTagLength_) {
                    logger_.Error(
                    "[TemplateEngine].[ParsingError]: OC (split); Length of the tag: '",
                    frame.carry,
                    "' crosses the maxTagLength_ limit which is ", maxTagLength_
                );
                    return { TemplateType::FAILURE, 0 };
                }

                tagView = frame.carry;
                ctx.justProcessedTag = true;

                goto __ProcessTag;
            }

            tagEnd = bodyView.find("%}");
            if(tagEnd == std::string_view::npos) {
                // So the min chunk size is about 512 bytes, and maxTagLength_ is like what, 270?
                // And ur telling me that we just started this chunk and we couldn't find the tag end?
                // Dawg fuck no
                logger_.Error(
                    "[TemplateEngine].[ParsingError]: Couldn't find tag end in this chunk, it started in previous chunk. Tag: ",
                    frame.carry
                );
                return { TemplateType::FAILURE, 0 };
            }

            // Found tag end in this chunk, but before we append, check the length of tag
            // It cannot cross maxTagLength_
            std::size_t appendCount = tagEnd + 2;
            if(frame.carry.size() + appendCount > maxTagLength_) {
                logger_.Error(
                    "[TemplateEngine].[ParsingError]: OC; Length of the tag: '",
                    frame.carry,
                    "' crosses the maxTagLength_ limit which is ", maxTagLength_
                );
                return { TemplateType::FAILURE, 0 };
            }

            frame.carry.append(bodyView.data(), appendCount);
            frame.readOffset += appendCount;

            // Complete tag ready, now process it
            tagView = frame.carry;
            ctx.justProcessedTag = true;

            goto __ProcessTag;
        }

__DefaultChunkProcessing:
        // CASE 2: Normal reading of data from current frame
        while(frame.readOffset < bufLen) {
            bodyView = {bufPtr + frame.readOffset, frame.bytesRead - frame.readOffset};

            // If we are processing a child template (one that extends a parent)-
            // -or skipUntilFlag is set, we should NOT write any content from it
            isExtending  = !ctx.currentExtendsName.empty();
            skipLiterals = isExtending || ctx.skipUntilFlag;
            tagStart     = bodyView.find("{%");

            // No tag found, literal chunk
            if(tagStart == std::string::npos) {
                // Now we need to check for one thing, are we ending this chunk with '{'
                // Because if we are, then maybe the next chunk contains the rest of the tag uk
                // Example -> Data: <...> {% block id %} <...>
                //         -> Chunk 1: "<...> {" , Chunk 2: "% block id %} <...>"
                // So we write what we know is a literal to output and throw '{' inside of carry
                bool maybeTag = EndsWith(bodyView, "{");
                outSize = maybeTag ? bodyView.size() - 1 : bodyView.size();

                // We only append content to block if we aren't in parent template
                // For parent template we simply just write out the content if there is no replacement
                // This condition is helpful when stuffs across boundary (like block statement across two chunks)
                if(ctx.inBlock && isExtending)
                    ctx.currentBlockContent.append(bodyView.data(), outSize);

                else if(
                    !skipLiterals
                    && !SafeWrite(ctx.io, bodyView.data(), outSize, ctx.justProcessedTag)
                )
                    return { TemplateType::FAILURE, 0 };

                if(maybeTag)
                    frame.carry.assign("{");

                ctx.justProcessedTag = false;
                break; // Break inner loop, go to __NextChunk
            }

            // Write everything before tag as literal
            if(ctx.inBlock && isExtending)
                ctx.currentBlockContent.append(bodyView.data(), tagStart);
            else if(
                tagStart > 0
                && !skipLiterals
                && !SafeWrite(ctx.io, bodyView.data(), tagStart, ctx.justProcessedTag)
            )
                return { TemplateType::FAILURE, 0 };

            ctx.justProcessedTag = false;

            // Advance offset to the start of the tag
            frame.readOffset += tagStart;

            // Recalculate view from the tag start
            bodyView = {bufPtr + frame.readOffset, bufLen - frame.readOffset};
            tagEnd   = bodyView.find("%}");

            // Incomplete tag, carry over for next read
            // Why use assign? Tags can only span one chunk at max, so either the '{%' or '%}' spans-
            // -at a single time, not both at the same time
            if(tagEnd == std::string::npos) {
                frame.carry.assign(bodyView.data(), bodyView.size());
                goto __NextChunk;
            }

            tagView = {bodyView.data(), tagEnd + 2};

            // Tag cannot be larger than 'maxTagLength_', so uk people don't just 'accidentally'-
            // -make it a billion bytes :)
            if(tagView.size() > maxTagLength_) {
                logger_.Error(
                    "[TemplateEngine].[ParsingError]: IC; Length of the tag: '",
                    tagView,
                    "' crosses the maxTagLength_ limit which is ", maxTagLength_
                );
                return { TemplateType::FAILURE, 0 };
            }

            ctx.justProcessedTag = true;

            // Common functionality for both partial and fully completed tags
        __ProcessTag:
            TemplateEngine::TagResult tagResult = ProcessTag(ctx, tagView);
            if(tagResult == TagResult::FAILURE)
                return { TemplateType::FAILURE, 0 };

            // Preserve the dynamic tags for future compilation
            if(tagResult == TagResult::PASSTHROUGH_DYNAMIC) {
                ctx.foundDynamicTag = true;
                if(!skipLiterals && !SafeWrite(ctx.io, tagView.data(), tagView.size()))
                    return { TemplateType::FAILURE, 0 };
            }

            if(!frame.carry.empty())
                frame.carry.clear();

            // We add tagView's length because 'tagEnd' is relative to
            // 'bodyView', which starts at 'frame.readOffset'
            else
                frame.readOffset += tagView.length();

            if(tagResult == TagResult::CONTROL_TO_ANOTHER_FILE)
                goto __ContinueOuterLoop;
        }
        // Set the read offset to be zero before reading more bytes
    __NextChunk:
        frame.readOffset = 0;

        // Skip the read loop entirely if needed. Also the ';' is there for suppressing compiler warnings-
        // -because empty label is ig not allowed in cxx version < C++2b
    __ContinueOuterLoop:
        ;
    }

    return {
        ctx.foundDynamicTag ? TemplateType::DYNAMIC : TemplateType::STATIC,
        ctx.io.file->Size()
    };
}

// vvv Helper Functions vvv
bool TemplateEngine::PushFile(CompilationContext& context, const std::string& relPath)
{
    auto& fs     = FileSystem::GetFileSystem();
    auto& config = Config::GetInstance();

    std::string fullPath = config.projectConfig.templateDir + "/" + relPath;

    BaseFilePtr newFile = fs.OpenFileRead(fullPath.c_str());
    if(!newFile) {
        logger_.Error("[TemplateEngine]: Cannot open include '", fullPath, '\'');
        return false;
    }
    context.stack.emplace_back(std::move(newFile), context.chunkSize);
    return true;
}

Tag TemplateEngine::ExtractTag(std::string_view line)
{
    // Find the content between {% and %}
    std::size_t start = line.find("{%");
    std::size_t end   = line.rfind("%}");

    if(
        start == std::string_view::npos
        || end == std::string_view::npos
        || start >= end
    )
        return {};

    // Get the inner content, e.g., "  extends   'file-a.html'  "
    std::string_view content = line.substr(start + 2, end - (start + 2));

    // Trim leading whitespace to find the start of the tag name
    std::size_t nameStart = content.find_first_not_of(" \t\n\r");
    if(nameStart == std::string_view::npos)
        return {};

    // Find the end of the tag name (the next whitespace)
    std::size_t nameEnd = content.find_first_of(" \t\n\r", nameStart);

    // Tag has no arguments
    if(nameEnd == std::string_view::npos)
        return { content.substr(nameStart), {} };

    std::string_view tagName = content.substr(nameStart, nameEnd - nameStart);
    std::string_view tagArgs = content.substr(nameEnd);

    // Trim leading space from arguments
    std::size_t argsStart = tagArgs.find_first_not_of(" \t\n\r");

    // Tag has no arguments
    if(argsStart == std::string_view::npos)
        return { tagName, {} };
    
    // Return the trimmed tag name and its arguments
    return { tagName, tagArgs.substr(argsStart) };
}

TemplateEngine::TagResult TemplateEngine::ProcessTag(
    CompilationContext& context, std::string_view tagView
)
{
    auto [tagName, tagArgs] = ExtractTag(tagView);

    // Empty tags are not allowed
    if(tagName.empty()) {
        logger_.Error("[TemplateEngine].[ParsingError]: Empty tags are not allowed");
        return TagResult::FAILURE;
    }

    // --- Skip Mode ---
    if(context.skipUntilFlag) {
        if(tagName == "endblock")
            context.skipUntilFlag = false;

        return TagResult::SUCCESS; // Discard everything while skipping
    }

    // Some dictionary type shit
    switch(tagName[0])
    {
        // Tags available: 'block'
        case 'b':
            if(tagName == "block") {
                if(tagArgs.empty()) {
                    logger_.Error(
                        "[TemplateEngine].[ParsingError]: {% block ... %} expects an identifier as an argument, found nothing"
                    );
                    return TagResult::FAILURE;
                }

                std::string blockName = std::string(tagArgs);

                // Try to find the current block in the block list, if u can, substitute it
                auto it = context.childBlocks.find(blockName);
                if(it != context.childBlocks.end()) {
                    SafeWrite(context.io, it->second.data(), it->second.size());
                    context.skipUntilFlag = true; // Now just skip everything until endblock
                    return TagResult::SUCCESS;
                }

                // Now in another case, we would want to substitute the original content inplace-
                // -if we couldn't find a replacement above, that is if we are in original parent file now
                if(context.currentExtendsName.empty()) {
                    context.inBlock = true;
                    return TagResult::SUCCESS;
                }

                // Else create a new block
                context.inBlock          = true;
                context.currentBlockName = std::move(blockName);
                context.currentBlockContent.clear();

                return TagResult::SUCCESS; // Don't write line yet
            }
            break;

        // Tags available: 'elif', 'else', 'endblock', 'endif', 'extends'
        case 'e':
            if(
                tagName == "elif"
                || tagName == "else"
                || tagName == "endif"
            )
                return TagResult::PASSTHROUGH_DYNAMIC;

            if(tagName == "endblock") {
                // Endblock doesn't take in arguments
                if(!tagArgs.empty()) {
                    logger_.Error(
                        "[TemplateEngine].[ParsingError]: {% endblock %} does not take any arguments, found: ", tagArgs
                    );
                    return TagResult::FAILURE;
                }

                if(!context.inBlock) {
                    logger_.Error(
                        "[TemplateEngine].[ParsingError]: {% endblock %} found without its corresponding {% block ... %}"
                    );
                    return TagResult::FAILURE;
                }

                // Trim the content a bit for cleaner output
                TrimInline(context.currentBlockContent);

                context.inBlock = false;
                context.childBlocks[std::move(context.currentBlockName)] = std::move(context.currentBlockContent);
                context.currentBlockName.clear();
                context.currentBlockContent.clear();

                return TagResult::SUCCESS; // Skip writing
            }

            if(tagName == "extends") {
                if(tagArgs.empty()) {
                    logger_.Error(
                        "[TemplateEngine].[ParsingError]: {% extends ... %} expects a file name as an argument, found nothing"
                    );
                    return TagResult::FAILURE;
                }

                std::size_t q1 = tagArgs.find_first_of("'\"");
                std::size_t q2 = tagArgs.find_last_of("'\"");
                
                if(q1 == std::string::npos || q2 <= q1) {
                    logger_.Error(
                        "[TemplateEngine].[ParsingError]: {% extends ... %} got an improperly formatted file name."
                        " Usage example: {% extends 'base.html' %}"
                    );
                    return TagResult::FAILURE;
                }

                // The order of operations for extends is different from include
                // We want current file to be processed first before the parent file does
                // Unlike include where parent file is processed first
                context.currentExtendsName = std::string(tagArgs.substr(q1 + 1, q2 - q1 - 1));;

                return TagResult::SUCCESS;
            }
            break;

        // Tags available: 'if', 'include'
        case 'i':
            if(tagName == "if") return TagResult::PASSTHROUGH_DYNAMIC;

            if(tagName == "include") {
                if(tagArgs.empty()) {
                    logger_.Error(
                        "[TemplateEngine].[ParsingError]: {% include ... %} expects a file name as an argument, found nothing"
                    );
                    return TagResult::FAILURE;
                }

                std::size_t q1 = tagArgs.find_first_of("'\"");
                std::size_t q2 = tagArgs.find_last_of("'\"");
            
                if(q1 == std::string::npos || q2 <= q1) {
                    logger_.Error(
                        "[TemplateEngine].[ParsingError]: {% include ... %} got an improperly formatted file name."
                        " Usage example: {% include 'base.html' %}"
                    );
                    return TagResult::FAILURE;
                }

                std::string includePath = std::string(tagArgs.substr(q1 + 1, q2 - q1 - 1));

                return PushFile(context, includePath)
                        ? TagResult::CONTROL_TO_ANOTHER_FILE
                        : TagResult::FAILURE;
            }
            break;

        // Tags available: 'var'
        case 'v':
            if(tagName == "var") return TagResult::PASSTHROUGH_DYNAMIC;
            break;
    }

    // Unknown tags are not allowed
    logger_.Error("[TemplateEngine].[ParsingError]: Unknown tag found: ", tagName);
    return TagResult::FAILURE;
}

// vvv IO Functions vvv
bool TemplateEngine::FlushWrite(IOContext& ctx, bool force)
{
    if(!force && ctx.offset < ctx.chunkSize)
        return true;

    if(ctx.offset == 0)
        return true;

    if(ctx.file->Write(ctx.buffer.get(), ctx.offset)
        != static_cast<std::int64_t>(ctx.offset))
        return false;

    ctx.offset = 0;
    return true;
}

bool TemplateEngine::SafeWrite(IOContext& ctx, const void* data, std::size_t size, bool skipSpaces)
{
    const char* ptr = static_cast<const char*>(data);

    if(skipSpaces) {
        std::size_t firstChar = 0;
        while(firstChar < size && std::isspace(ptr[firstChar]))
            firstChar++;

        ptr  += firstChar;
        size -= firstChar;
    }

    // Nothing to write, move on
    if(size == 0)
        return true;

    while(size > 0) {
        std::size_t available = ctx.chunkSize - ctx.offset;
        std::size_t toCopy    = std::min(size, available);

        std::memcpy(ctx.buffer.get() + ctx.offset, ptr, toCopy);
        ctx.offset += toCopy;
        ptr        += toCopy;
        size       -= toCopy;

        if(!FlushWrite(ctx))
            return false;
    }
    return true;
}

} // namespace WFX::Core