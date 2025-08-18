#include "rw_buffer.hpp"

#include "utils/logger/logger.hpp"

namespace WFX::Utils {

// vvv Destructor vvv
RWBuffer::~RWBuffer()
{
    // Read buffer contains the pointer for the BufferPool
    if(readBuffer_) {
        ReadMetadata* readMeta = GetReadMeta();
        BufferPool*   pool     = readMeta ? readMeta->poolPtr : nullptr;

        // Cool, we got what we need, dealloc write then read buffers
        if(pool) {
            pool->Release(readBuffer_);
            readBuffer_ = nullptr;

            if(writeBuffer_) {
                pool->Release(writeBuffer_);
                writeBuffer_ = nullptr;
            }
        }
        // Something got corrupted, this shouldn't be happening in a normal situation
        // Crash the server, this shouldn't be possible dawg
        else
            Logger::GetInstance().Fatal("[RWBuffer]: Read buffer failed to contain valid memory pointer, Invalid Server State");
    }
    // This makes 0 sense, readBuffer_ is invalid but writeBuffer_ exists
    else if(writeBuffer_)
        Logger::GetInstance().Fatal("[RWBuffer]: Write buffer exists without a valid Read buffer, Invalid Server State");
}

} // namespace WFX::Utils