// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_HTTP_USER_RESPONSE_HPP
#define WFX_INC_HTTP_USER_RESPONSE_HPP

#include "core/core.hpp"
#include "shared/json/json_object_fwd.hpp"
#include "shared/apis/http_api.hpp"
#include "shared/utils/memory.hpp"
#include <string_view>
#include <charconv>

namespace WFX::Http {

/* User side implementation of 'Response' class. 'CoreEngine' passes the API */
class Response {
public:
    Response(void* backend) : backend_(backend)
    {}

public: // Status and Headers
    Response& Status(Shared::HttpStatus code)
    {
        Core::HttpApiExt1()->setStatus(backend_, code);
        return *this;
    }

    Response& Status(std::uint16_t code)
    {
        Core::HttpApiExt1()->setStatus(backend_, static_cast<Shared::HttpStatus>(code));
        return *this;
    }

    Response& Header(std::string_view key, std::string_view value)
    {
        Core::HttpApiExt1()->setHeader(backend_, ToSV(key), ToSV(value));
        return *this;
    }

    // Like Header(), but survives a later forced-error rebuild (AbortWithError, e.g. SendFile's
    // auto-404). Must be called before any regular Header() call on the same response. For
    // headers that must be on every response regardless of status, e.g. CORS headers set by
    // middleware before the handler runs
    Response& PersistentHeader(std::string_view key, std::string_view value)
    {
        Core::HttpApiExt1()->setPersistentHeader(backend_, ToSV(key), ToSV(value));
        return *this;
    }

public: // Main flow
    // Char / View types
    Response& Write(std::string_view data)
    {
        Core::HttpApiExt1()->writeBody(backend_, ToSV(data));
        return *this;
    }

    Response& Write(const char* data)
    {
        return Write(std::string_view{data});
    }

    // UUID types
    Response& Write(const Shared::UUID& uuid)
    {
        return Write(std::string_view{uuid.ToString().data, 36});
    }

    // Integral types, stack formatted
    Response& Write(std::int64_t value)
    {
        char buf[20];
        auto [end, _] = std::to_chars(buf, buf + sizeof(buf), value);
        return Write(std::string_view{buf, static_cast<std::size_t>(end - buf)});
    }

    Response& Write(std::uint64_t value)
    {
        char buf[20];
        auto [end, _] = std::to_chars(buf, buf + sizeof(buf), value);
        return Write(std::string_view{buf, static_cast<std::size_t>(end - buf)});
    }

    // Common integral promotions so user can pass int, unsigned, etc. naturally
    Response& Write(std::int32_t value)
    {
        return Write(static_cast<std::int64_t>(value));
    }
    Response& Write(std::uint32_t value)
    {
        return Write(static_cast<std::uint64_t>(value));
    }
    Response& Write(std::int16_t value)
    {
        return Write(static_cast<std::int64_t>(value));
    }
    Response& Write(std::uint16_t value)
    {
        return Write(static_cast<std::uint64_t>(value));
    }
    Response& Write(std::int8_t value)
    {
        return Write(static_cast<std::int64_t>(value));
    }
    Response& Write(std::uint8_t value)
    {
        return Write(static_cast<std::uint64_t>(value));
    }

    // Floating point, stack formatted
    Response& Write(double value)
    {
        char buf[32];
        auto [end, _] = std::to_chars(buf, buf + sizeof(buf), value);
        return Write(std::string_view{buf, static_cast<std::size_t>(end - buf)});
    }

    Response& Write(float value)
    {
        return Write(static_cast<double>(value));
    }

    // Bool
    Response& Write(bool value)
    {
        return Write(value ? std::string_view{"true", 4} : std::string_view{"false", 5});
    }

    // Locks response, patches Content-Length
    // Optional: engine auto-commits if user forgets
    void Commit()
    {
        Core::HttpApiExt1()->commit(backend_);
    }

public: // Sugar syntax
    // Plain text, sets Content-Type, writes, commits
    void SendText(std::string_view data)
    {
        Header("Content-Type", "text/plain");
        Write(data);
        Commit();
    }

    // Zero-copy sendfile path
    void SendFile(std::string_view path, bool autoHandle404 = true)
    {
        Core::HttpApiExt1()->writeFile(backend_, ToSV(path), autoHandle404);
    }
    void SendFile(Shared::StringView path, bool autoHandle404 = true)
    {
        Core::HttpApiExt1()->writeFile(backend_, path, autoHandle404);
    }

    // HTML Template, sets Content-Type, writes, commits
    void SendTemplate(std::string_view path, Shared::JsonObject&& ctx)
    {
        Core::HttpApiExt1()->writeTemplate(backend_, ToSV(path), &ctx);
    }
    void SendTemplate(Shared::StringView path, Shared::JsonObject&& ctx)
    {
        Core::HttpApiExt1()->writeTemplate(backend_, path, &ctx);
    }

    // Typed lambda, allocated via engine allocator
    template <typename Fn> void Stream(Fn&& fn, bool chunked = true)
    {
        using FnType = std::decay_t<Fn>;

        FnType* f = Shared::New<FnType>(std::forward<Fn>(fn));
        if(!f)
            return;

        Shared::StreamGenerator gen{f,

                                    // Next
                                    [](void* ctx, Shared::StreamBuffer buffer) -> Shared::StreamResult {
                                        return (*static_cast<FnType*>(ctx))(buffer);
                                    },

                                    // Destroy
                                    [](void* ctx) { Shared::Delete(static_cast<FnType*>(ctx)); }};

        Core::HttpApiExt1()->writeStream(backend_, gen, chunked);
    }

public: // Internal use
    void* GetBackend()
    {
        return backend_;
    }

private:
    static Shared::StringView ToSV(std::string_view s)
    {
        return {s.data(), static_cast<std::uint64_t>(s.size())};
    }

private:
    void* backend_;
};

} // namespace WFX::Http

#endif // WFX_INC_HTTP_USER_RESPONSE_HPP