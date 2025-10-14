#include "template_engine.hpp"

#include "config/config.hpp"
#include "utils/backport/string.hpp"
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
    const std::string& inputDir  = config.projectConfig.templateDir;
    const std::string  outputDir = config.projectConfig.projectName + staticFolder_;

    // For partial tag checking. If u see it, don't compile the .html file
    // + 1 is redundant btw, but im still keeping it to cover my paranoia
    char temp[partialTagSize_ + 1] = { 0 };
    
    resaveCacheFile_ = true;

    if(!fs.DirectoryExists(outputDir.c_str()) && !fs.CreateDirectory(outputDir, true))
        logger_.Fatal("[TemplateEngine]: Failed to create base template directory: ", outputDir);
    
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

        const std::string outPath = outputDir + "/" + relPath;

        // Ensure target directory exists
        std::string relOutputDir = outPath.substr(0, outPath.find_last_of("/\\"));
        if(!fs.DirectoryExists(relOutputDir.c_str()) && !fs.CreateDirectory(relOutputDir, true)) {
            logger_.Error("[TemplateEngine]: Failed to create template output directory: ", outputDir);
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
        else
            templates_.emplace(
                std::move(relPath), TemplateMeta{type, outSize, std::move(outPath)}
            );
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

    while(!stack.empty()) {
        TemplateFrame& frame = stack.back();

        std::int64_t bytesRead = frame.file->Read(frame.readBuf.get(), ctx.chunkSize);
        if(bytesRead < 0)
            return { TemplateType::FAILURE, 0 };

        if(bytesRead == 0) {
            if(
                !frame.carry.empty()
                && !SafeWrite(ctx.io, frame.carry.data(), frame.carry.size())
            )
                return { TemplateType::FAILURE, 0 };

            // Remaining data to be flushed
            if(!FlushWrite(ctx.io, true))
                return { TemplateType::FAILURE, 0 };

            ctx.stack.pop_back();
            continue;
        }

        std::size_t pos = 0;
        char* bufPtr = frame.readBuf.get();

        if(frame.firstRead) {
            frame.firstRead = false;
            if(
                bytesRead >= partialTagSize_
                && StartsWith(std::string_view(bufPtr, partialTagSize_), partialTag_)
            )
                pos += partialTagSize_ + 1;
        }

        while(pos < static_cast<std::size_t>(bytesRead)) {
            std::string line;
            bool hasNewline = false;
            if(!frame.carry.empty()) {
                line = std::move(frame.carry);
                frame.carry.clear();
            }

            std::size_t lineStartPos = pos;
            for(; pos < static_cast<std::size_t>(bytesRead); ++pos) {
                if(bufPtr[pos] == '\n') {
                    hasNewline = true;
                    line.append(bufPtr + lineStartPos, pos - lineStartPos + 1);
                    ++pos;
                    break;
                }
            }

            if(!hasNewline)
                line.append(bufPtr + lineStartPos, pos - lineStartPos);
            
            if(hasNewline) {
                switch(ProcessLine(ctx, line)) {
                    case LineResult::FAILURE:
                        return { TemplateType::FAILURE, 0 };
                    case LineResult::REGULAR_LINE:
                        if(!SafeWrite(ctx.io, line.data(), line.size()))
                            return { TemplateType::FAILURE, 0 };
                        break;
                    case LineResult::PROCESSED_INCLUDE:
                        break; // Discard line
                }
            }
            else
                frame.carry = std::move(line);
        }
    }

    return {
        ctx.foundInclude ? TemplateType::COMPILED_STATIC : TemplateType::PURE_STATIC,
        ctx.io.file->Size()
    };
}

// vvv Helper Functions vvv
bool TemplateEngine::PushInclude(CompilationContext& context, const std::string& relPath)
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

TemplateEngine::LineResult TemplateEngine::ProcessLine(
    CompilationContext& context, const std::string& line
)
{
    std::size_t tagPos = line.find("{% include");
    if(tagPos == std::string::npos)
        return LineResult::REGULAR_LINE;

    std::size_t tagEnd = line.find("%}", tagPos);
    if(tagEnd == std::string::npos)
        return LineResult::FAILURE;

    std::string tagBody = line.substr(tagPos + 2, tagEnd - (tagPos + 2));
    TrimInline(tagBody);

    std::size_t q1 = tagBody.find_first_of("'\"");
    std::size_t q2 = tagBody.find_last_of("'\"");
    if(q1 == std::string::npos || q2 <= q1)
        return LineResult::FAILURE;

    std::string includePath = tagBody.substr(q1 + 1, q2 - q1 - 1);
    context.foundInclude = true;
    return PushInclude(context, includePath) ? LineResult::PROCESSED_INCLUDE : LineResult::FAILURE;
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

bool TemplateEngine::SafeWrite(IOContext& ctx, const void* data, std::size_t size)
{
    const char* ptr = static_cast<const char*>(data);

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