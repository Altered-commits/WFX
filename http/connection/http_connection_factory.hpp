#ifndef WFX_HTTP_CONNECTION_FACTORY_HPP
#define WFX_HTTP_CONNECTION_FACTORY_HPP

// This is simply a helper thingy which will abstract the 'selecting'-
// -of OS specific functionality for connection handling
#include "http_connection.hpp"
#include <memory>

#ifdef _WIN32
    #include "os_specific/windows/iocp_connection.hpp"
#else
    #ifdef WFX_LINUX_USE_IO_URING
        #include "os_specific/linux/io_uring_connection.hpp"
    #else
        #include "os_specific/linux/epoll_connection.hpp"
    #endif
#endif

namespace WFX::Http {

// Factory function that returns the correct handler
inline std::unique_ptr<HttpConnectionHandler> CreateConnectionHandler(bool useHttps)
{
#ifdef _WIN32
    return std::make_unique<WFX::OSSpecific::IocpConnectionHandler>();
#else
    #ifdef WFX_LINUX_USE_IO_URING
        return std::make_unique<WFX::OSSpecific::IoUringConnectionHandler>();
    #else
        return std::make_unique<WFX::OSSpecific::EpollConnectionHandler>(useHttps);
    #endif
#endif
}

} // namespace WFX::Http

#endif // WFX_HTTP_CONNECTION_FACTORY_HPP