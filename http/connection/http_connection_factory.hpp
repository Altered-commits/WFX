// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_HTTP_CONNECTION_FACTORY_HPP
#define WFX_HTTP_CONNECTION_FACTORY_HPP

// This is simply a helper thingy which will abstract the 'selecting'-
// -of OS specific functionality for connection handling
#include "http_connection.hpp"
#include "shared/utils/detection_macro.hpp"
#include <memory>

#ifdef WFX_PLATFORM_LINUX
#include "os_specific/linux/epoll_connection.hpp"
#else
#error "Unsupported platform - add a WFX_PLATFORM_<X> branch in HTTP factory and a new os_specific/<x>/ backend"
#endif

namespace WFX::Http {

// Factory function that returns the correct handler
inline std::unique_ptr<HttpConnectionHandler> CreateConnectionHandler(bool useHttps)
{
#ifdef WFX_PLATFORM_LINUX
    return std::make_unique<OSSpecific::EpollConnectionHandler>(useHttps);
#else
#error "Unsupported platform - add a WFX_PLATFORM_<X> branch in HTTP factory and a new os_specific/<x>/ backend"
#endif
}

} // namespace WFX::Http

#endif // WFX_HTTP_CONNECTION_FACTORY_HPP