#include "template_engine.hpp"

#include "config/config.hpp"
#include "utils/fileops/filemeta.hpp"
#include "utils/backport/string.hpp"
#include "utils/crypt/string.hpp"
#include <cstring>

#if defined(__linux__)
    #include <dlfcn.h>
#endif

namespace WFX::Core {

TemplateEngine& TemplateEngine::GetInstance()
{
    static TemplateEngine engine;
    return engine;
}

// vvv Main Functions vvv
TemplateCompilationResult TemplateEngine::PreCompileTemplates()
{
    auto& projectConfig = Config::GetInstance().projectConfig;

    bool               resaveCacheFile     = false;
    bool               hasDynamicElement   = false;
    bool               setCacheStats       = false;
    std::size_t        errors              = 0;
    const std::string& inputDir            = projectConfig.templateDir;
    const std::string  staticOutputDir     = projectConfig.projectName + STATIC_FOLDER;
    const std::string  dynamicCxxOutputDir = projectConfig.projectName + DYNAMIC_FOLDER;

    // For partial tag checking. If u see it, don't compile the .html file
    // + 1 is redundant btw, but im still keeping it to cover my paranoia
    char temp[PARTIAL_TAG_SIZE + 1] = { 0 };

    // Cache file to not compile templates everytime
    FileMeta fileMeta{projectConfig.projectName + TEMPLATE_CACHE_PATH};
    switch(fileMeta.Load()) {
        case FileMetaStatus::SUCCESS:
            break;

        case FileMetaStatus::IO_ERROR:
        case FileMetaStatus::CORRUPTED:
        case FileMetaStatus::NOT_FOUND:
        case FileMetaStatus::TOO_LARGE:
        case FileMetaStatus::TOO_MANY_ENTRIES:
            fileMeta.Clear();
            resaveCacheFile = true;
            logger_.Warn(
                "[TemplateEngine]: Template cache file was either corrupted, too large or not found, "
                "engine will rebuild the cache"
            );
            break;

        default:
            logger_.Fatal("[TemplateEngine]: Unhandled return case for template cache");
    }

    if(
        !FileSystem::DirectoryExists(staticOutputDir.c_str())
        && !FileSystem::CreateDirectory(staticOutputDir, true)
    )
        logger_.Fatal("[TemplateEngine]: Failed to create static directory: ", staticOutputDir);

    // Dynamic templates have their C++ representation in dynamic/ folder
    if(
        !FileSystem::DirectoryExists(dynamicCxxOutputDir.c_str())
        && !FileSystem::CreateDirectory(dynamicCxxOutputDir, true)
    )
        logger_.Fatal("[TemplateEngine]: Failed to create dynamic-cxx directory: ", dynamicCxxOutputDir);
    
    logger_.Info("[TemplateEngine]: Starting template compilation from: ", inputDir);

    // Traverse the entire template directory looking for .html files
    FileSystem::ListDirectory(inputDir, true, [&](std::string inPath) {
        // Reset this flag every iteration so we don't accidentally set random files in cache
        setCacheStats = false;

        if(!EndsWith(inPath, ".html") && !EndsWith(inPath, ".htm"))
            return;
        
        logger_.Info("[TemplateEngine]: Found template: ", inPath);

        // Cache checking
        FileStats     diskStats  = {};
        FileMetadata* cacheStats = fileMeta.Get(inPath);

        if(FileSystem::GetFileStats(inPath.c_str(), diskStats)) {
            if(cacheStats) {
                // Basic check, did file modified time change? if not, then no need to compile this file
                if(diskStats.modifiedNs == cacheStats->modifiedTime)
                    return;
    
                // Rn we don't check for hash and all, just modified time
                // Later we check for other cool shit
                resaveCacheFile = true;
                setCacheStats = true;
                logger_.Info("[TemplateEngine]: Template modified, recompiling");
            }
            // No cache stats available, set it at the end of processing
            else
                setCacheStats = true;
        }
        else
            logger_.Warn(
                "[TemplateEngine]: Failed to check [disk / cache] stats for file: ", inPath.c_str(),
                ". Continuing with full compilation"
            );

        // Strip leading slash cuz we will use this as key inside of templates_ map
        // We expect that user, inside of 'SendTemplate' function, inputs path without leading slash
        // And because 'SendTemplate' uses templates_ map, we cannot use leading slash
        std::string relPath = std::string(inPath.begin() + inputDir.size(), inPath.end());
        relPath.erase(0, relPath.find_first_not_of("/\\"));

        // Every file is initially written to the static folder, even the dynamic .html files.
        // After the .html file is completely stripped off static tags, and IF dynamic tags still-
        // -remain, we move onto stage two of compiling. That is when we use the dynamic folder
        const std::string outPath = staticOutputDir + "/" + relPath;

        // Ensure target directory exists
        std::string relOutputDir = outPath.substr(0, outPath.find_last_of("/\\"));
        if(
            !FileSystem::DirectoryExists(relOutputDir.c_str())
            && !FileSystem::CreateDirectory(relOutputDir, true)
        ) {
            logger_.Error(
                "[TemplateEngine]: Failed to create template output directory: ", staticOutputDir
            );
            return;
        }

        auto in = FileSystem::OpenFileRead(inPath.c_str());
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
        if(inSize >= PARTIAL_TAG_SIZE) {        
            if(in->ReadAt(temp, PARTIAL_TAG_SIZE, 0) < 0) {
                errors++;
                logger_.Error("[TemplateEngine]: Failed to read the first 13 bytes");
                return;
            }

            // No need to compile
            if(StartsWith(temp, PARTIAL_TAG))
                return;
        }

        auto out = FileSystem::OpenFileWrite(outPath.c_str());
        if(!out) {
            errors++;
            logger_.Error("[TemplateEngine]: Failed to open output template file: ", outPath);
            return;
        }

        // TODO: Delete template build if compilation failed
        auto [type, outSize] = CompileTemplate(std::move(in), std::move(out));
        if(type == TemplateType::FAILURE) {
            errors++;
            return;
        }
        
        // Add it to our template map
        if(type == TemplateType::STATIC)
            templates_.emplace(
                std::move(relPath), TemplateMeta{type, outSize, std::move(outPath)}
            );

        // Websites dynamic, sigh, get ready for some fuckery
        else {
            hasDynamicElement = true;
            logger_.Info("[TemplateEngine]: Staging dynamic template for compilation: ", relPath);
            // Create a unique, C compatible function name
            std::string funcName = 
                StringCanonical::NormalizePathToIdentifier(relPath, DYNAMIC_FUNC_PREFIX);

            // Define path for the new .cpp file
            std::string cppPath = dynamicCxxOutputDir + "/" + relPath + ".cpp";

            // Create cxx representation of templates now
            if(!GenerateCxxFromTemplate(outPath, cppPath, funcName)) {
                errors++;
                return;
            }

            // Store the newly generated cxx data
            templates_.emplace(
                std::move(relPath), TemplateMeta{type, outSize, std::move(outPath)}
            );
        }

        // Either new file or existing file has been updated
        if(setCacheStats)
            fileMeta.Set(std::move(inPath), {
                diskStats.modifiedNs,
                "" // Hash will be used later on
            });
    });

    // We will save cache files regardless of errors
    if(resaveCacheFile) {
        // Finally, save the cache, if OS allows us to :)
        switch(fileMeta.Save()) {
            case FileMetaStatus::SUCCESS:
                logger_.Info("[TemplateEngine]: Saved template cache file successfully");
                break;

            default:
                logger_.Warn("[TemplateEngine]: IO error while saving template cache file."
                            " Cache will be stale :(");
        }
    }

    if(errors > 0) {
        logger_.Warn("[TemplateEngine]: Template compilation complete with ", errors, " error(s)");
        return TemplateCompilationResult{false, false};
    }

    logger_.Info("[TemplateEngine]: Template compilation completed successfully");
    return TemplateCompilationResult{true, hasDynamicElement};
}

void TemplateEngine::LoadDynamicTemplatesFromLib()
{
    // Load the user_templates.[dll/so/dylib] from <project>/build/
    // Now its the callers responsibility to make sure it exists, if it doesn't-
    // -we go boom boom
    const std::string inputDir = config_.projectConfig.projectName + STATIC_FOLDER;
    const std::string dllPath  = config_.projectConfig.projectName + TEMPLATE_LIB_PATH;

    if(!FileSystem::FileExists(dllPath.c_str()))
        logger_.Fatal("[TemplateEngine]: Dynamic template loader couldn't find ", dllPath);

#if defined(_WIN32)
    static_assert(false, "No impl for TemplateEngine.LoadDynamicTemplatesFromLib for Windows");
#else
    // POSIX (Linux / macOS / *nix)
    // RTLD_NOW: resolve symbols immediately; RTLD_GLOBAL: let module export symbols globally if needed
    void* handle = dlopen(dllPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if(!handle) {
        const char* err = dlerror();
        logger_.Fatal("[TemplateEngine]: ", dllPath, " dlopen failed: ", (err ? err : "unknown error"));
    }

    // Loop over the templates_ and find the ones with TemplateType::DYNAMIC
    // It will contain the functions name in 'pathOrName' member, use it to get-
    // -Generator class inside of .dll/.so
    for(auto& templateMeta : templates_) {
        auto& tmpl = templateMeta.second;
        if(tmpl.type != TemplateType::DYNAMIC)
            continue;

        // Clear any existing error
        dlerror();

        // Extract the symbol name from the filePath
        std::string relPath = std::string(tmpl.filePath.begin() + inputDir.size(), tmpl.filePath.end());
        relPath.erase(0, relPath.find_first_not_of("/\\"));

        std::string symbol = StringCanonical::NormalizePathToIdentifier(relPath, DYNAMIC_FUNC_PREFIX);

        void* rawSym = dlsym(handle, symbol.c_str());
        const char* dlsymErr = dlerror();
        if(!rawSym || dlsymErr)
            logger_.Fatal("[TemplateEngine]: Failed to find '", symbol, "' in template lib. Error: ",
                        (dlsymErr ? dlsymErr : "symbol not found"));

        // Each function returns a unique_ptr to generator class as defined by 'TemplateCreatorFn'
        tmpl.gen = reinterpret_cast<TemplateCreatorFn>(rawSym)();
        if(!tmpl.gen)
            logger_.Fatal("[TemplateEngine]: Failed to create template generator for: ", symbol);
    }

#endif
    logger_.Info("[TemplateEngine]: Successfully initialized dynamic template module(s): ", dllPath);
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
                if(!PushFile(ctx, ctx.currentExtendsName))
                    return { TemplateType::FAILURE, 0 };

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
                frame.bytesRead >= PARTIAL_TAG_SIZE
                && StartsWith(std::string_view(bufPtr, PARTIAL_TAG_SIZE), PARTIAL_TAG)
            )
                frame.readOffset += PARTIAL_TAG_SIZE + 1;
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
                if(frame.carry.size() > MAX_TAG_LENGTH) {
                    logger_.Error(
                    "[TemplateEngine].[ParsingError]: OC (split); Length of the tag: '",
                    frame.carry,
                    "' crosses the MAX_TAG_LENGTH limit which is ", MAX_TAG_LENGTH
                );
                    return { TemplateType::FAILURE, 0 };
                }

                tagView = frame.carry;
                ctx.justProcessedTag = true;

                goto __ProcessTag;
            }

            tagEnd = bodyView.find("%}");
            if(tagEnd == std::string_view::npos) {
                // So the min chunk size is about 512 bytes, and MAX_TAG_LENGTH is like what, 300?
                // And ur telling me that we just started this chunk and we couldn't find the tag end?
                // Dawg fuck no
                logger_.Error(
                    "[TemplateEngine].[ParsingError]: Couldn't find tag end in this chunk, it started in previous chunk. Tag: ",
                    frame.carry
                );
                return { TemplateType::FAILURE, 0 };
            }

            // Found tag end in this chunk, but before we append, check the length of tag
            // It cannot cross MAX_TAG_LENGTH
            std::size_t appendCount = tagEnd + 2;
            if(frame.carry.size() + appendCount > MAX_TAG_LENGTH) {
                logger_.Error(
                    "[TemplateEngine].[ParsingError]: OC; Length of the tag: '",
                    frame.carry,
                    "' crosses the 'MAX_TAG_LENGTH' limit which is ", MAX_TAG_LENGTH
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
            // Either to block
            if(ctx.inBlock && isExtending)
                ctx.currentBlockContent.append(bodyView.data(), tagStart);
            // Or to file directly
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

            // Tag cannot be larger than 'MAX_TAG_LENGTH', so uk people don't just 'accidentally'-
            // -make it a billion bytes :)
            if(tagView.size() > MAX_TAG_LENGTH) {
                logger_.Error(
                    "[TemplateEngine].[ParsingError]: IC; Length of the tag: '",
                    tagView,
                    "' crosses the 'MAX_TAG_LENGTH' limit which is ", MAX_TAG_LENGTH
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
                // Either saved to block
                if(ctx.inBlock && isExtending)
                    ctx.currentBlockContent.append(tagView);

                // Or written to file
                else if(!skipLiterals && !SafeWrite(ctx.io, tagView.data(), tagView.size()))
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
    auto& config = Config::GetInstance();

    std::string fullPath = config.projectConfig.templateDir + "/" + relPath;

    BaseFilePtr newFile = FileSystem::OpenFileRead(fullPath.c_str());
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
) {
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

    // Get the tag type we working with
    auto it = tagViewToType.find(tagName);
    if(it == tagViewToType.end())
        goto __Failure;

    // Some dictionary type shit
    switch(it->second) {
        case TagType::INCLUDE:
        {
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
        case TagType::EXTENDS:
        {
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
        case TagType::BLOCK:
        {
            if(tagArgs.empty()) {
                logger_.Error(
                    "[TemplateEngine].[ParsingError]: {% block ... %} expects an identifier as an argument, found nothing"
                );
                return TagResult::FAILURE;
            }

            // We do not allow nested block statements, if we are inside of a block, fail it
            if(context.inBlock) {
                logger_.Error(
                    "[TemplateEngine].[ParsingError]: Nested block statements are not allowed, but found {% block ", tagArgs,
                    "%} inside of {% block ", context.currentBlockName, "%}"
                );
                return TagResult::FAILURE;
            }

            std::string blockName = std::string(tagArgs);

            // Try to find the current block in the block list, if u can, substitute it
            auto it = context.childBlocks.find(blockName);
            if(it != context.childBlocks.end()) {
                if(!SafeWrite(context.io, it->second.data(), it->second.size()))
                    return TagResult::FAILURE;

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
        case TagType::ENDBLOCK:
        {
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
            context.childBlocks.emplace(
                std::move(context.currentBlockName),
                std::move(context.currentBlockContent)
            );
            context.currentBlockName.clear();
            context.currentBlockContent.clear();

            return TagResult::SUCCESS; // Skip writing
        }
        case TagType::VAR:     // ---
        case TagType::IF:      //   |
        case TagType::ELIF:    //   |
        case TagType::ELSE:    //   | All these are parsed later in transpilation process
        case TagType::ENDIF:   //   |
        case TagType::FOR:     //   |
        case TagType::ENDFOR:  // ---
            return TagResult::PASSTHROUGH_DYNAMIC;

        // Shouldn't happen but yeah    
        default:
            break;
    }

__Failure:
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

    if(
        ctx.file->Write(ctx.buffer.get(), ctx.offset)
        != static_cast<std::int64_t>(ctx.offset)
    ) {
        logger_.Error("[TemplateEngine]: 'FlushWrite' failed to write data to current file");
        return false;
    }

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