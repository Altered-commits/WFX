#ifndef WFX_INC_HTTP_USER_RESPONSE_HPP
#define WFX_INC_HTTP_USER_RESPONSE_HPP

#include "core/core.hpp"
#include "shared/apis/http_api.hpp"

namespace WFX::Http {

/* User side implementation of 'Response' class. 'CoreEngine' passes the API */
class Response {
public:
    Response(void* backend) : backend_(backend) {}

public:
    Response& Status(Shared::HttpStatus code)
    {
        Core::HttpApi()->SetStatus(backend_, code);
        return *this;
    }

    Response& Set(std::string_view key, std::string_view value)
    {
        auto k = ToSV(key);
        auto v = ToSV(value);

        Core::HttpApi()->SetHeader(backend_, k, v);
        return *this;
    }

    void SendText(std::string_view text)
    {
        auto sv = ToSV(text);
        Core::HttpApi()->SendText(backend_, sv);
    }

    void SendFile(std::string_view path, bool autoHandle404 = true)
    {
        auto sv = ToSV(path);
        Core::HttpApi()->SendFile(backend_, sv, autoHandle404);
    }

    template<typename Fn>
    void Stream(Fn&& fn, bool chunked)
    {
        using FnType = std::decay_t<Fn>;

        // Allocate using engine allocator
        void* raw = Core::MemoryApi()->Alloc(sizeof(FnType));
        if(!raw)
            return;

        // Placement new
        FnType* f = new(raw) FnType(std::forward<Fn>(fn));

        Shared::StreamGenerator gen{
            f,

            // Next
            [](void* ctx, Shared::StreamBuffer buffer) -> Shared::StreamResult {
                return (*static_cast<FnType*>(ctx))(buffer);
            },

            // Destroy
            [](void* ctx) {
                auto* f = static_cast<FnType*>(ctx);

                f->~FnType();
                Core::MemoryApi()->Free(f);
            }
        };

        Core::HttpApi()->Stream(backend_, gen, chunked);
    }

private:
    static Shared::StringView ToSV(std::string_view s)
    {
        return { s.data(), static_cast<std::uint64_t>(s.size()) };
    }

private:
    void* backend_;
};

} // namespace WFX::Http

#endif // WFX_INC_HTTP_USER_RESPONSE_HPP