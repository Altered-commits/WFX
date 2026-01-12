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
    if(fileSize > allocThreshold_)
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
    // So total file size / lineSize_ bytes will be the number of estimated elements
    meta_.clear();
    meta_.reserve(std::max(fileSize / lineSize_, minimumEntries_));

    // Format (STRICT, one line per record):
    // <file><sep><size><sep><mtime><sep><hash>\n
    // ^^^ ...
    std::size_t i       = 0;
    std::size_t entries = 0;
    while(i < fileSize) {
        const std::size_t sidx = FindSeparator(buffer, i);
        if(sidx == bufferEnd_) return FileMetaStatus::CORRUPTED;

        const std::size_t midx = FindSeparator(buffer, sidx + 1);
        if(midx == bufferEnd_) return FileMetaStatus::CORRUPTED;

        const std::size_t hidx = FindSeparator(buffer, midx + 1);
        if(hidx == bufferEnd_) return FileMetaStatus::CORRUPTED;

        // Expect newline immediately after hash field
        std::size_t eol = hidx + 1;
        while(eol < fileSize && buffer[eol] != '\n')
            ++eol;

        if(eol >= fileSize) return FileMetaStatus::CORRUPTED;

        // vvv Field extraction vvv
        std::string file(&buffer[i], sidx - i);

        std::uint64_t size  = 0;
        std::uint64_t mtime = 0;

        if(!StrToUInt64(std::string_view(&buffer[sidx + 1], midx - sidx - 1), size))
            return FileMetaStatus::CORRUPTED;

        if(!StrToUInt64(std::string_view(&buffer[midx + 1], hidx - midx - 1), mtime))
            return FileMetaStatus::CORRUPTED;

        std::string hash(&buffer[hidx + 1], eol - hidx - 1);

        meta_.emplace(
            std::move(file),
            FileMetadata{
                size,
                mtime,
                std::move(hash)
            }
        );

        // Move to next record if we haven't violated constraints
        if(++entries > entryThreshold_)
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
    buffer.reserve(lineSize_* meta_.size()); // Rough estimate, lineSize_ bytes per entry

    // Reusable buffer enough for std::uint64_t
    char numBuf[32];

    for(const auto& [file, meta] : meta_) {
        const auto& [size, mtime, hash] = meta;

        // File
        buffer.insert(buffer.end(), file.begin(), file.end());
        buffer.push_back(fieldSeparator_);

        // Size
        {
            auto [ptr, ec] = std::to_chars(numBuf, numBuf + sizeof(numBuf), size);
            if(ec != std::errc{})
                return FileMetaStatus::CORRUPTED;
            buffer.insert(buffer.end(), numBuf, ptr);
        }
        buffer.push_back(fieldSeparator_);

        // Modified time
        {
            auto [ptr, ec] = std::to_chars(numBuf, numBuf + sizeof(numBuf), mtime);
            if(ec != std::errc{})
                return FileMetaStatus::CORRUPTED;
            buffer.insert(buffer.end(), numBuf, ptr);
        }
        buffer.push_back(fieldSeparator_);

        // Hash
        buffer.insert(buffer.end(), hash.begin(), hash.end());
        buffer.push_back('\n');
    }

    if(outFile->Write(buffer.data(), buffer.size()) < 0)
        return FileMetaStatus::IO_ERROR;

    return FileMetaStatus::SUCCESS;
}

FileMetadata* FileMeta::Get(const std::string& file)
{
    auto it = meta_.find(file);
    if(it != meta_.end())
        return &it->second;

    return nullptr;
}

void FileMeta::Set(std::string file, FileMetadata meta)
{
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
    std::size_t foundIdx = bufferEnd_;

    for(std::size_t i = idx; i < buffer.size(); i++) {
        if(buffer[i] == '\n')
            break;

        if(buffer[i] == fieldSeparator_) {
            foundIdx = i;
            break;
        }
    }

    return foundIdx;
}

} // namespace WFX::Utils