#include "rw_buffer.hpp"
#include "utils/logger/logger.hpp"

#include <cstring>

namespace WFX::Utils {
    
// vvv Constructor and Destructor vvv
RWBuffer::RWBuffer()
{
    auto& pool = BufferPool::GetInstance();
    if(!pool.IsInitialized())
        Logger::GetInstance().Fatal("[RWBuffer]: 'BufferPool' must be initialized for 'RWBuffer' to work");
}

RWBuffer::~RWBuffer()
{
    ResetBuffer();
}

// vvv Initializer / Destructor Functions vvv
bool RWBuffer::InitReadBuffer(std::uint32_t size)
{
    // Already initialized
    if(readBuffer_) return true;

    auto& pool = BufferPool::GetInstance();

    std::size_t allocSize = sizeof(ReadMetadata) + size;
    readBuffer_ = static_cast<char*>(pool.Alloc(allocSize));
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

    auto& pool = BufferPool::GetInstance();

    std::size_t allocSize = sizeof(WriteMetadata) + size;
    writeBuffer_ = static_cast<char*>(pool.Alloc(allocSize));
    if(!writeBuffer_)
        return false;

    auto* writeMeta = reinterpret_cast<WriteMetadata*>(writeBuffer_);
    writeMeta->bufferSize    = size;
    writeMeta->dataLength    = 0;
    writeMeta->writtenLength = 0;

    return true;
}

void RWBuffer::ResetBuffer()
{
    auto& pool = BufferPool::GetInstance();

    pool.Free(readBuffer_);  readBuffer_ = nullptr;
    pool.Free(writeBuffer_); writeBuffer_ = nullptr;
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
        writeMeta->dataLength    = 0;
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
bool RWBuffer::GenericGrowBuffer(
    char*& buffer, std::uint32_t metaSize, std::uint32_t growSize, std::uint32_t maxSize
) {
    if(!buffer)
        return false;

    auto* meta = reinterpret_cast<RWBaseMetadata*>(buffer);

    if(meta->dataLength < meta->bufferSize)
        return true;

    if(meta->bufferSize >= maxSize)
        return false;

    std::uint32_t newSize = meta->bufferSize + growSize;
    if(newSize > maxSize)
        newSize = maxSize;

    auto& pool = BufferPool::GetInstance();

    std::uint32_t allocSize = static_cast<std::uint32_t>(metaSize + newSize);

    char* newBuf = static_cast<char*>(pool.Realloc(buffer, allocSize));
    if(!newBuf)
        return false;

    buffer = newBuf;

    meta = reinterpret_cast<RWBaseMetadata*>(buffer);
    meta->bufferSize = newSize;

    return true;
}

bool RWBuffer::GenericAppendData(
    char*& buffer, std::uint32_t metaSize, const char* data,
    std::uint32_t size, std::uint32_t growSize, std::uint32_t maxSize
) {
    if(!buffer || !data || size == 0)
        return false;

    auto* meta = reinterpret_cast<RWBaseMetadata*>(buffer);

    while(size > meta->bufferSize - meta->dataLength) {
        if(!GenericGrowBuffer(buffer, metaSize, growSize, maxSize))
            return false;

        meta = reinterpret_cast<RWBaseMetadata*>(buffer);
    }

    char* dest = buffer + metaSize + meta->dataLength;
    std::memcpy(dest, data, size);

    meta->dataLength += size;
    return true;
}

// vvv Read Buffer Management vvv
bool RWBuffer::GrowReadBuffer(std::uint32_t growSize, std::uint32_t maxSize)
{
    if(!readBuffer_)
        return false;

    return GenericGrowBuffer(
        readBuffer_,
        sizeof(ReadMetadata),
        growSize,
        maxSize
    );
}

bool RWBuffer::AppendReadData(
    const char* data, std::uint32_t size, std::uint32_t incSize, std::uint32_t maxSize
) {
    return GenericAppendData(
        readBuffer_,
        sizeof(ReadMetadata),
        data,
        size,
        incSize,
        maxSize
    );
}

void RWBuffer::AdvanceReadLength(std::uint32_t n) noexcept
{
    if(!readBuffer_) return;

    auto* meta = reinterpret_cast<ReadMetadata*>(readBuffer_);
    meta->dataLength = std::min(meta->dataLength + n, meta->bufferSize);
}

ValidRegion RWBuffer::GetWritableReadRegion() const noexcept
{
    if(!readBuffer_) return {nullptr, 0};

    auto* readMeta = reinterpret_cast<ReadMetadata*>(readBuffer_);
    return {
        readBuffer_ + sizeof(ReadMetadata) + readMeta->dataLength,
        readMeta->bufferSize - readMeta->dataLength
    };
}

// vvv Write Buffer Management vvv
bool RWBuffer::GrowWriteBuffer(std::uint32_t growSize, std::uint32_t maxSize)
{
    if(!writeBuffer_)
        return false;

    return GenericGrowBuffer(
        writeBuffer_,
        sizeof(WriteMetadata),
        growSize,
        maxSize
    );
}

bool RWBuffer::AppendWriteData(
    const char* data, std::uint32_t size, std::uint32_t incSize, std::uint32_t maxSize
) {
    return GenericAppendData(
        writeBuffer_,
        sizeof(WriteMetadata),
        data,
        size,
        incSize,
        maxSize
    );
}

void RWBuffer::AdvanceWriteLength(std::uint32_t n) noexcept
{
    if(!writeBuffer_) return;

    auto* meta = reinterpret_cast<WriteMetadata*>(writeBuffer_);
    meta->writtenLength = std::min(meta->writtenLength + n, meta->dataLength);
}

ValidRegion RWBuffer::GetWritableWriteRegion() const noexcept
{
    if(!writeBuffer_) return {nullptr, 0};

    auto* writeMeta = reinterpret_cast<WriteMetadata*>(writeBuffer_);
    return {
        writeBuffer_ + sizeof(WriteMetadata) + writeMeta->dataLength,
        writeMeta->bufferSize - writeMeta->dataLength
    };
}

} // namespace WFX::Utils