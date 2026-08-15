// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "filemeta.hpp"

#include "utils/fileops/filesystem.hpp"
#include "utils/string/string.hpp"
#include <charconv>

namespace WFX::Utils {

// vvv Constructor vvv
FileMeta::FileMeta(std::string filePath) : filePath_(std::move(filePath))
{}

// vvv Main Functions vvv
FileMetaStatus FileMeta::Load()
{
    auto metaFile = FileSystem::OpenFileRead(filePath_.c_str());
    if(!metaFile)
        return FileMetaStatus::NOT_FOUND;

    const std::size_t fileSize = metaFile->Size();
    if(fileSize > ALLOC_THRESHOLD)
        return FileMetaStatus::TOO_LARGE;

    // Ik this looks weird but what caller must do for a 'CORRUPTED'
    // type is invalidate cache and rebuild it pretty much.
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
    // <file><sep><mtime><sep><hash><sep><ud_size><sep><ud_bytes>\n
    // ^^^ ...
    std::size_t i = 0;
    std::size_t entries = 0;
    while(i < fileSize) {
        const std::size_t midx = FindSeparator(buffer, i);
        const std::size_t hidx = FindSeparator(buffer, midx + 1);
        const std::size_t sidx = FindSeparator(buffer, hidx + 1);
        const std::size_t didx = FindSeparator(buffer, sidx + 1);

        if(midx == BUFFER_END || hidx == BUFFER_END || sidx == BUFFER_END || didx == BUFFER_END)
            return FileMetaStatus::CORRUPTED;

        std::string file(&buffer[i], midx - i);

        std::int64_t modifiedTime = 0;
        if(!StringUtils::StrToInt64({&buffer[midx + 1], hidx - midx - 1}, modifiedTime))
            return FileMetaStatus::CORRUPTED;

        std::string hash(&buffer[hidx + 1], sidx - hidx - 1);

        std::uint64_t udSize = 0;
        if(!StringUtils::StrToUInt64({&buffer[sidx + 1], didx - sidx - 1}, udSize))
            return FileMetaStatus::CORRUPTED;

        const std::size_t dataStart = didx + 1;
        const std::size_t dataEnd = dataStart + udSize;

        if(dataEnd >= fileSize || buffer[dataEnd] != '\n')
            return FileMetaStatus::CORRUPTED;

        FileMetadata meta;
        meta.modifiedTime = modifiedTime;
        meta.hash = std::move(hash);
        meta.userData.assign(&buffer[dataStart], &buffer[dataEnd]);

        meta_.emplace(std::move(file), std::move(meta));

        if(++entries > ENTRY_THRESHOLD)
            return FileMetaStatus::TOO_MANY_ENTRIES;

        i = dataEnd + 1;
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
    buffer.reserve(LINE_SIZE * meta_.size()); // Rough estimate, LINE_SIZE bytes per entry

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
        buffer.push_back(FIELD_SEPARATOR);

        // User data size
        const std::uint64_t udSize = meta.userData.size();
        {
            auto [ptr, ec] = std::to_chars(numBuf, numBuf + sizeof(numBuf), udSize);
            if(ec != std::errc{})
                return FileMetaStatus::CORRUPTED;
            buffer.insert(buffer.end(), numBuf, ptr);
        }
        buffer.push_back(FIELD_SEPARATOR);

        // User data raw bytes
        buffer.insert(buffer.end(), meta.userData.begin(), meta.userData.end());
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

const FileMetaMap& FileMeta::Entries() const noexcept
{
    return meta_;
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