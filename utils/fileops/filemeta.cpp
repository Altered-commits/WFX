#include "filemeta.hpp"

#include "utils/fileops/filesystem.hpp"
#include "utils/backport/string.hpp"
#include <charconv>

namespace WFX::Utils {

// vvv Constructor vvv
FileMeta::FileMeta(std::string filePath)
    : filePath_(std::move(filePath))
{}

// vvv Main Functions vvv
FileMetaStatus FileMeta::Load()
{
    auto metaFile = FileSystem::OpenFileRead(filePath_.c_str());
    if(!metaFile)
        return FileMetaStatus::NOT_FOUND;

    std::size_t fileSize = metaFile->Size();
    if(fileSize > ALLOC_THRESHOLD)
        return FileMetaStatus::TOO_LARGE;

    // Ik this looks weird but what caller must do for a 'CORRUPTED'-
    // -type is invalidate cache and rebuild it pretty much
    if(fileSize == 0)
        return FileMetaStatus::CORRUPTED;

    std::vector<char> buffer{};
    buffer.resize(fileSize);

    if(metaFile->Read(buffer.data(), fileSize) < 0)
        return FileMetaStatus::IO_ERROR;

    // Clear any previous data which may have persisted
    // We use lineSize_as an estimation that each line on avg will be lineSize_bytes
    // So total file size / LINE_SIZE bytes will be the number of estimated elements
    meta_.clear();
    meta_.reserve(std::max(fileSize / LINE_SIZE, MINIMUM_ENTRIES));

    // Format (STRICT, one line per record):
    // <file><sep><size><sep><mtime><sep><hash>\n
    // ^^^ ...
    std::size_t i       = 0;
    std::size_t entries = 0;
    while(i < fileSize) {
        const std::size_t midx = FindSeparator(buffer, i + 1);
        if(midx == BUFFER_END) return FileMetaStatus::CORRUPTED;

        const std::size_t hidx = FindSeparator(buffer, midx + 1);
        if(hidx == BUFFER_END) return FileMetaStatus::CORRUPTED;

        // Expect newline immediately after hash field
        std::size_t eol = hidx + 1;
        while(eol < fileSize && buffer[eol] != '\n')
            ++eol;

        if(eol >= fileSize) return FileMetaStatus::CORRUPTED;

        // vvv Field extraction vvv
        std::string file(&buffer[i], midx - i);

        std::int64_t modifiedtime = 0;
        if(!StrToInt64(
            std::string_view(&buffer[midx + 1], hidx - midx - 1), modifiedtime
        ))
            return FileMetaStatus::CORRUPTED;

        std::string hash(&buffer[hidx + 1], eol - hidx - 1);

        meta_.emplace(
            std::move(file),
            FileMetadata{
                modifiedtime,
                std::move(hash)
            }
        );

        // Move to next record if we haven't violated constraints
        if(++entries > ENTRY_THRESHOLD)
            return FileMetaStatus::TOO_MANY_ENTRIES;

        i = eol + 1;
    }

    return FileMetaStatus::SUCCESS;
}

FileMetaStatus FileMeta::Save() const
{
    // Open file for writing (overwrite)
    auto outFile = FileSystem::OpenFileWrite(filePath_.c_str());
    if(!outFile)
        return FileMetaStatus::IO_ERROR;

    std::vector<char> buffer;
    buffer.reserve(LINE_SIZE* meta_.size()); // Rough estimate, LINE_SIZE bytes per entry

    // Reusable buffer enough for std::uint64_t
    char numBuf[32];

    for(const auto& [file, meta] : meta_) {
        // The file no longer exists, ignore it
        if(!meta.hit)
            continue;

        // File
        buffer.insert(buffer.end(), file.begin(), file.end());
        buffer.push_back(FIELD_SEPARATOR);

        // Modified time
        {
            auto [ptr, ec] = std::to_chars(numBuf, numBuf + sizeof(numBuf), meta.modifiedTime);
            if(ec != std::errc{})
                return FileMetaStatus::CORRUPTED;
            buffer.insert(buffer.end(), numBuf, ptr);
        }
        buffer.push_back(FIELD_SEPARATOR);

        // Hash
        buffer.insert(buffer.end(), meta.hash.begin(), meta.hash.end());
        buffer.push_back('\n');
    }

    if(outFile->Write(buffer.data(), buffer.size()) < 0)
        return FileMetaStatus::IO_ERROR;

    return FileMetaStatus::SUCCESS;
}

FileMetadata* FileMeta::Get(const std::string& file, bool processHit)
{
    auto it = meta_.find(file);
    if(it != meta_.end()) {
        auto ptr = &it->second;
        ptr->hit = processHit;
        return ptr;
    }

    return nullptr;
}

void FileMeta::Set(std::string file, FileMetadata meta)
{
    meta.hit = true;
    meta_[std::move(file)] = std::move(meta);
}

void FileMeta::Erase(const std::string& file)
{
    meta_.erase(file);
}

void FileMeta::Clear()
{
    meta_.clear();
}

// vvv Helper Functions vvv
std::size_t FileMeta::FindSeparator(const FileBuffer& buffer, std::size_t idx)
{
    // Find seperator before we hit a newline
    std::size_t foundIdx = BUFFER_END;

    for(std::size_t i = idx; i < buffer.size(); i++) {
        if(buffer[i] == '\n')
            break;

        if(buffer[i] == FIELD_SEPARATOR) {
            foundIdx = i;
            break;
        }
    }

    return foundIdx;
}

} // namespace WFX::Utils