#include "http_api.hpp"

#include "http/routing/router.hpp"

namespace WFX::Shared {

const HTTP_API_TABLE* GetHttpAPIV1()
{
    static HTTP_API_TABLE __GlobalHttpAPIV1 = {
        [](HttpMethod method, std::string_view path, HttpCallbackType cb) {   // Register Route
            WFX::Http::Router::GetInstance().RegisterRoute(method, path, std::move(cb));
        },
        HttpAPIVersion::V1                                                    // Version
    };

    return &__GlobalHttpAPIV1;
}

} // namespace WFX::Shared