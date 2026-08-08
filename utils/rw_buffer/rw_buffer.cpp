// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "rw_buffer.hpp"
#include "shared/utils/memory.hpp"

#include <cstring>

namespace WFX::Utils {

// vvv Destructor vvv
RWBuffer::~RWBuffer()
{
    ResetBuffer();
}

// vvv Initializer / Destructor Functions vvv
bool RWBuffer::InitReadBuffer(std::uint32_t size)
{
    // Already initialized
    if(readBuffer_)
        return true;

    const std::size_t allocSize = sizeof(ReadMetadata) + size;
    readBuffer_ = static_cast<char*>(Shared::Alloc(allocSize));
    if(!readBuffer_)
        return false;

    auto* readMeta = reinterpret_cast<ReadMetadata*>(readBuffer_);
    readMeta->bufferSize = size;
    readMeta->dataLength = 0;

    return true;
}

bool RWBuffer::InitWriteBuffer(std::uint32_t size)
{
    // Already initialized
    if(writeBuffer_)
        return true;

    const std::size_t allocSize = sizeof(WriteMetadata) + size;
    writeBuffer_ = static_cast<char*>(Shared::Alloc(allocSize));
    if(!writeBuffer_)
        return false;

    auto* writeMeta = reinterpret_cast<WriteMetadata*>(writeBuffer_);
    writeMeta->bufferSize = size;
    writeMeta->dataLength = 0;
    writeMeta->writtenLength = 0;

    return true;
}

void RWBuffer::ResetBuffer()
{
    Shared::Free(readBuffer_);
    readBuffer_ = nullptr;
    Shared::Free(writeBuffer_);
    writeBuffer_ = nullptr;
}

void RWBuffer::ClearBuffer()
{
    ClearReadBuffer();
    ClearWriteBuffer();
}

void RWBuffer::ClearWriteBuffer()
{
    auto* writeMeta = GetWriteMeta();
    if(writeMeta) {
        writeMeta->dataLength = 0;
        writeMeta->writtenLength = 0;
    }
}

void RWBuffer::ClearReadBuffer()
{
    auto* readMeta = GetReadMeta();
    if(readMeta)
        readMeta->dataLength = 0;
}

// vvv Getter Functions vvv
char* RWBuffer::GetWriteData() const noexcept
{
    return writeBuffer_ ? writeBuffer_ + sizeof(WriteMetadata) : nullptr;
}

char* RWBuffer::GetReadData() const noexcept
{
    return readBuffer_ ? readBuffer_ + sizeof(ReadMetadata) : nullptr;
}

WriteMetadata* RWBuffer::GetWriteMeta() const noexcept
{
    return reinterpret_cast<WriteMetadata*>(writeBuffer_);
}

ReadMetadata* RWBuffer::GetReadMeta() const noexcept
{
    return reinterpret_cast<ReadMetadata*>(readBuffer_);
}

bool RWBuffer::IsReadInitialized() const noexcept
{
    return (!!readBuffer_);
}

bool RWBuffer::IsWriteInitialized() const noexcept
{
    return (!!writeBuffer_);
}

// vvv Generic Buffer Management vvv
bool RWBuffer::GenericGrowBuffer(char*& buffer, std::uint32_t metaSize, std::uint32_t growSize, std::uint32_t maxSize,
                                 std::uint32_t minSize)
{
    if(!buffer)
        return false;

    auto* meta = reinterpret_cast<RWBaseMetadata*>(buffer);

    // Already at the ceiling, cannot hand out any more room
    if(meta->bufferSize >= maxSize)
        return false;

    // minSize unreachable under the ceiling
    if(minSize > maxSize)
        return false;

    // growSize == 0 means jump straight to maxSize; minSize lets the caller skip straight to a
    // known target instead of looping. 64-bit to avoid wrapping bufferSize + growSize past uint32.
    std::uint64_t newSize = growSize ? static_cast<std::uint64_t>(meta->bufferSize) + growSize : maxSize;
    if(minSize > newSize)
        newSize = minSize;
    if(newSize > maxSize)
        newSize = maxSize;

    const std::uint32_t allocSize = static_cast<std::uint32_t>(metaSize + newSize);

    char* newBuf = static_cast<char*>(Shared::Realloc(buffer, allocSize));
    if(!newBuf)
        return false;

    buffer = newBuf;

    meta = reinterpret_cast<RWBaseMetadata*>(buffer);
    meta->bufferSize = static_cast<std::uint32_t>(newSize);

    return true;
}

bool RWBuffer::GenericAppendData(char*& buffer, std::uint32_t metaSize, const char* data, std::uint32_t size,
                                 std::uint32_t growSize, std::uint32_t maxSize)
{
    if(!buffer || !data || size == 0)
        return false;

    auto* meta = reinterpret_cast<RWBaseMetadata*>(buffer);

    // Total capacity this append needs. 64-bit so dataLength + size can't wrap a uint32
    const std::uint64_t required = static_cast<std::uint64_t>(meta->dataLength) + size;

    if(required > meta->bufferSize) {
        // Won't fit even fully grown, refuse rather than truncate
        if(required > maxSize)
            return false;

        // Grow to fit in a SINGLE realloc, rounding the target up to a whole number of growSize
        // steps above the current size so back-to-back appends amortize (growSize == 0 grows to
        // exactly what's needed), then clamp to the ceiling. required <= maxSize checked above
        // guarantees the clamp still leaves room for this append.
        std::uint64_t newSize{0};

        if(growSize == 0)
            newSize = required;
        else {
            const std::uint64_t deficit = required - meta->bufferSize;
            const std::uint64_t steps = (deficit + growSize - 1) / growSize;
            newSize = static_cast<std::uint64_t>(meta->bufferSize) + steps * growSize;
        }

        if(newSize > maxSize)
            newSize = maxSize;

        const std::uint32_t allocSize = static_cast<std::uint32_t>(metaSize + newSize);

        char* newBuf = static_cast<char*>(Shared::Realloc(buffer, allocSize));
        if(!newBuf)
            return false;

        buffer = newBuf;
        meta = reinterpret_cast<RWBaseMetadata*>(buffer);
        meta->bufferSize = static_cast<std::uint32_t>(newSize);
    }

    char* dest = buffer + metaSize + meta->dataLength;
    std::memcpy(dest, data, size);

    meta->dataLength += size;
    return true;
}

// vvv Read Buffer Management vvv
bool RWBuffer::GrowReadBuffer(std::uint32_t growSize, std::uint32_t maxSize, std::uint32_t minSize)
{
    if(!readBuffer_)
        return false;

    return GenericGrowBuffer(readBuffer_, sizeof(ReadMetadata), growSize, maxSize, minSize);
}

bool RWBuffer::AppendReadData(const char* data, std::uint32_t size, std::uint32_t incSize, std::uint32_t maxSize)
{
    return GenericAppendData(readBuffer_, sizeof(ReadMetadata), data, size, incSize, maxSize);
}

void RWBuffer::AdvanceReadLength(std::uint32_t n) noexcept
{
    if(!readBuffer_)
        return;

    auto* meta = reinterpret_cast<ReadMetadata*>(readBuffer_);
    meta->dataLength = std::min(meta->dataLength + n, meta->bufferSize);
}

ValidRegion RWBuffer::GetWritableReadRegion() const noexcept
{
    if(!readBuffer_)
        return {nullptr, 0};

    auto* readMeta = reinterpret_cast<ReadMetadata*>(readBuffer_);
    return {readBuffer_ + sizeof(ReadMetadata) + readMeta->dataLength, readMeta->bufferSize - readMeta->dataLength};
}

// vvv Write Buffer Management vvv
bool RWBuffer::GrowWriteBuffer(std::uint32_t growSize, std::uint32_t maxSize)
{
    if(!writeBuffer_)
        return false;

    return GenericGrowBuffer(writeBuffer_, sizeof(WriteMetadata), growSize, maxSize);
}

bool RWBuffer::AppendWriteData(const char* data, std::uint32_t size, std::uint32_t incSize, std::uint32_t maxSize)
{
    return GenericAppendData(writeBuffer_, sizeof(WriteMetadata), data, size, incSize, maxSize);
}

void RWBuffer::AdvanceWriteLength(std::uint32_t n) noexcept
{
    if(!writeBuffer_)
        return;

    auto* meta = reinterpret_cast<WriteMetadata*>(writeBuffer_);
    meta->writtenLength = std::min(meta->writtenLength + n, meta->dataLength);
}

void RWBuffer::CompactWriteBuffer() noexcept
{
    // Discards the flushed prefix [0, writtenLength), slides the unsent tail [writtenLength,
    // dataLength) down to offset 0, and rebases both watermarks so only live bytes remain.
    auto* meta = GetWriteMeta();
    if(!meta || meta->writtenLength == 0)
        return;

    const std::uint32_t remaining = meta->dataLength - meta->writtenLength;
    if(remaining > 0)
        std::memmove(GetWriteData(), GetWriteData() + meta->writtenLength, remaining);

    meta->dataLength = remaining;
    meta->writtenLength = 0;
}

ValidRegion RWBuffer::GetWritableWriteRegion() const noexcept
{
    if(!writeBuffer_)
        return {nullptr, 0};

    auto* writeMeta = reinterpret_cast<WriteMetadata*>(writeBuffer_);
    return {writeBuffer_ + sizeof(WriteMetadata) + writeMeta->dataLength,
            writeMeta->bufferSize - writeMeta->dataLength};
}

} // namespace WFX::Utils