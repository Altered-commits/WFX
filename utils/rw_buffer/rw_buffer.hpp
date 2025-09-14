#ifndef WFX_UTILS_RW_BUFFER_HPP
#define WFX_UTILS_RW_BUFFER_HPP

#include "utils/buffer_pool/buffer_pool.hpp"

// Layout explanation:
//
// [ WriteMetadata | WriteBuffer ]
// [ ReadMetadata(+BufferPool ptr) | ReadBuffer ]
//
// Write buffer is constant-sized (per connection setup).
// Read buffer is dynamically grown/shrunk.
// Read metadata stores BufferPool* for independent memory management.

namespace WFX::Utils {

// Ease of writing C++
struct alignas(8) ValidRegion {
    char*       ptr = nullptr;
    std::size_t len = 0;
};

// For write buffer: 8-byte aligned, minimal
struct alignas(8) WriteMetadata {
    std::uint32_t bufferSize    = 0;
    std::uint32_t dataLength    = 0;
    std::uint32_t writtenLength = 0;
};

// For read buffer: includes buffer pool pointer
struct alignas(8) ReadMetadata {
    std::uint32_t bufferSize = 0;
    std::uint32_t dataLength = 0;
    BufferPool*   poolPtr    = nullptr;
};

class alignas(16) RWBuffer {
public:
    RWBuffer() = default;
    ~RWBuffer();

public: // Init / Reset
    bool InitWriteBuffer(std::uint32_t size);
    bool InitReadBuffer(BufferPool& pool, std::uint32_t size);

    void ResetBuffer();

public: // Getter functions
    char*          GetWriteData()        const noexcept;
    char*          GetReadData()         const noexcept;
    
    WriteMetadata* GetWriteMeta()        const noexcept;
    ReadMetadata*  GetReadMeta()         const noexcept;

    bool           IsReadInitialized()   const noexcept;
    bool           IsWriteInitialized()  const noexcept;

public: // Read buffer management
    bool        GrowReadBuffer(std::uint32_t defaultSize, std::uint32_t maxSize);
    ValidRegion GetWritableReadRegion()       const noexcept;
    ValidRegion GetWritableWriteRegion()      const noexcept;
    void        AdvanceReadLength(std::uint32_t n)  noexcept;
    void        AdvanceWriteLength(std::uint32_t n) noexcept;

public: // Write buffer management
    bool AppendData(const char* data, std::uint32_t size);

private:
    char* writeBuffer_ = nullptr;
    char* readBuffer_  = nullptr;
};

static_assert(sizeof(RWBuffer) <= 16, "RWBuffer must strictly be less than or equal to 16 bytes");

} // namespace WFX::Utils

#endif // WFX_UTILS_RW_BUFFER_HPP