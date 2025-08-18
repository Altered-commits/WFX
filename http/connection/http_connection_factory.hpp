#ifndef WFX_HTTP_CONNECTION_FACTORY_HPP
#define WFX_HTTP_CONNECTION_FACTORY_HPP

// This is simply a helper thingy which will abstract the 'selecting'-
// -of OS specific functionality for connection handling
#include "http_connection.hpp"

#ifdef _WIN32
    #include "os_specific/windows/http/connection/iocp_connection.hpp"
#else
    #include "os_specific/linux/http/connection/epoll_connection.hpp"
#endif

#include <memory>

namespace WFX::Http {
    // Factory function that returns the correct handler
    inline std::unique_ptr<HttpConnectionHandler> CreateConnectionHandler()
    {
    #ifdef _WIN32
        return std::make_unique<WFX::OSSpecific::IocpConnectionHandler>();
    #else
        return std::make_unique<WFX::OSSpecific::EpollConnectionHandler>();
    #endif
    }
}

#endif