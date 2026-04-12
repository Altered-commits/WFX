#ifndef WFX_UTILS_RW_BUFFER_HPP
#define WFX_UTILS_RW_BUFFER_HPP

#include "utils/pool/buffer_pool.hpp"

// Layout:
//
// [ WriteMetadata | WriteBuffer ]
// [ ReadMetadata  | ReadBuffer  ]
//
// Write buffer is constant-sized
// Read buffer is dynamically grown/shrunk

namespace WFX::Utils {

// Ease of writing C++
struct alignas(8) ValidRegion {
    char*       ptr = nullptr;
    std::size_t len = 0;
};

// For ease of generic functions
struct alignas(8) RWBaseMetadata {
    std::uint32_t bufferSize = 0;
    std::uint32_t dataLength = 0;
};

struct WriteMetadata : public RWBaseMetadata {
    std::uint32_t writtenLength = 0;
};

struct ReadMetadata : public RWBaseMetadata {};

class alignas(8) RWBuffer {
public:
    RWBuffer();
    ~RWBuffer();

public: // Init / Reset
    bool InitWriteBuffer(std::uint32_t size);
    bool InitReadBuffer(std::uint32_t size);

    void ResetBuffer();
    void ClearBuffer();

    void ClearWriteBuffer();
    void ClearReadBuffer();

public: // Getter functions
    char*          GetWriteData()        const noexcept;
    char*          GetReadData()         const noexcept;
    
    WriteMetadata* GetWriteMeta()        const noexcept;
    ReadMetadata*  GetReadMeta()         const noexcept;

    bool           IsReadInitialized()   const noexcept;
    bool           IsWriteInitialized()  const noexcept;

public: // Read buffer management
    bool        GrowReadBuffer(std::uint32_t growSize, std::uint32_t maxSize);
    bool        AppendReadData(const char* data, std::uint32_t size, std::uint32_t incSize, std::uint32_t maxSize);
    void        AdvanceReadLength(std::uint32_t n)  noexcept;
    ValidRegion GetWritableReadRegion()       const noexcept;
    
public: // Write buffer management
    bool        GrowWriteBuffer(std::uint32_t growSize, std::uint32_t maxSize);
    bool        AppendWriteData(const char* data, std::uint32_t size, std::uint32_t incSize, std::uint32_t maxSize);
    void        AdvanceWriteLength(std::uint32_t n) noexcept;
    ValidRegion GetWritableWriteRegion()      const noexcept;

private: // Internal functions
    bool GenericGrowBuffer(
        char*& buffer, std::uint32_t metaSize, std::uint32_t growSize, std::uint32_t maxSize
    );
    bool GenericAppendData(
        char*& buffer, std::uint32_t metaSize, const char* data,
        std::uint32_t size, std::uint32_t growSize, std::uint32_t maxSize
    );

private:
    char* writeBuffer_ = nullptr;
    char* readBuffer_  = nullptr;
};

static_assert(sizeof(RWBuffer) <= 16, "RWBuffer must strictly be less than or equal to 16 bytes");

} // namespace WFX::Utils

#endif // WFX_UTILS_RW_BUFFER_HPP