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
using ReadRegion = std::pair<char*, std::size_t>;

// For write buffer: 8-byte aligned, minimal
struct alignas(8) WriteMetadata {
    std::uint32_t bufferSize = 0;
    std::uint32_t dataLength = 0;
};

// For read buffer: includes buffer pool pointer
struct alignas(8) ReadMetadata {
    std::uint32_t bufferSize = 0;
    std::uint32_t dataLength = 0;
    BufferPool*   poolPtr    = nullptr; // Sacrifices 8 bytes from payload space
};

class alignas(16) RWBuffer {
public:
    RWBuffer() = default;
    ~RWBuffer();

public: // Init
    bool InitWriteBuffer(BufferPool& pool, std::size_t size);
    bool InitReadBuffer(BufferPool& pool, std::size_t size);

public: // Getter functions
    char*          GetWriteData() const noexcept;
    char*          GetReadData()  const noexcept;
    
    WriteMetadata* GetWriteMeta() const noexcept;
    ReadMetadata*  GetReadMeta()  const noexcept;

public: // Read buffer management
    bool       GrowReadBuffer(std::size_t defaultSize, std::size_t maxSize);
    ReadRegion GetWritableReadRegion()    const noexcept;
    void       AdvanceReadLength(std::size_t n) noexcept;

private:
    char* writeBuffer_ = nullptr;
    char* readBuffer_  = nullptr;
};

static_assert(sizeof(RWBuffer) <= 16, "RWBuffer must strictly be less than or equal to 16 bytes");

} // namespace WFX::Utils

#endif // WFX_UTILS_RW_BUFFER_HPP