// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_HTTP_CONNECTION_FACTORY_HPP
#define WFX_HTTP_CONNECTION_FACTORY_HPP

#include "http_connection.hpp"
#include <memory>
#include "os_specific/linux/epoll_connection.hpp"

namespace WFX::Http {

// The epoll backend, the only one WFX has
inline std::unique_ptr<HttpConnectionHandler> CreateConnectionHandler(bool useHttps)
{
    return std::make_unique<OSSpecific::EpollConnectionHandler>(useHttps);
}

} // namespace WFX::Http

#endif // WFX_HTTP_CONNECTION_FACTORY_HPP