#ifndef WFX_INC_HTTP_ENPOINT_HPP
#define WFX_INC_HTTP_ENPOINT_HPP

#include "async/promise.hpp"
#include "core/core.hpp"
#include "core/deferred_init_vector.hpp"
#include <span>

namespace WFX::Http {

using WFX::Shared::EndpointTLSConfig;

// vvv Metadata vvv
struct Metadata {
    std::string_view __Url = {};
    std::uint16_t __IntrnlIdx = 0;
};

// vvv Awaitables vvv
struct WritePayloadAwaitable {
    using EndpointStatus = WFX::Shared::EndpointStatus;

public: // Storage
    std::span<const std::byte> payload{};
    std::uint16_t endpointIdx{0};
    EndpointStatus status{};

public: // Main setup
    // Always suspend
    bool await_ready() const noexcept
    {
        return false;
    }

    void await_suspend(std::coroutine_handle<> h) noexcept
    {
        status = Core::HttpApiExt1()->WriteEndpoint(Core::HttpApiExt1()->GetGlobalPtrData(), endpointIdx,
                                                    payload.data(), payload.size());

        // On failure, resume the coroutine so user can handle the error
        if(status != EndpointStatus::PENDING)
            h.resume();
    }

    // Return status
    EndpointStatus await_resume() const noexcept
    {
        return status;
    }
};

struct Resolve {
public: // Constructor
    constexpr Resolve(std::string_view url, std::uint32_t connLimit = 64, std::uint32_t inFlightLimit = 64,
                      EndpointTLSConfig tlsConfig = EndpointTLSConfig::AUTO)
    {
        value_.__Url = url;

        Core::__WFXDeferred.emplace_back([=, this] {
            value_.__IntrnlIdx = Core::HttpApiExt1()->AllocateEndpoint(Shared::StringView{url.data(), url.size()},
                                                                       connLimit, inFlightLimit, tlsConfig);
        });
    }

public: // vvv Main Functions vvv
    WritePayloadAwaitable SendPayload(std::span<const std::byte> rawPayload) const
    {
        return WritePayloadAwaitable{rawPayload, value_.__IntrnlIdx};
    }

    WritePayloadAwaitable SendPayload(std::string_view str) const
    {
        return SendPayload(std::span<const std::byte>(reinterpret_cast<const std::byte*>(str.data()), str.size()));
    }

private:
    Metadata value_ = {};
};

} // namespace WFX::Http

#endif // WFX_INC_HTTP_ENPOINT_HPP