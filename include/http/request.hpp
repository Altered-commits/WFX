// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_HTTP_USER_REQUEST_HPP
#define WFX_INC_HTTP_USER_REQUEST_HPP

#include "core/core.hpp"
#include "shared/apis/http_api.hpp"
#include "shared/utils/memory.hpp"
#include <string_view>
#include <cstring>

namespace WFX::Http {

// Parses "key=value&key=value" once (built from Request::QueryParams() below, see Path() for
// where the raw text comes from) into pairs borrowed straight from that same buffer, no copying.
// Get() is a plain linear scan rather than a hash map: a real query string almost never carries
// more than a handful of params, and for that size a flat scan beats a hash map on every axis
// that matters
class QueryParams {
public:
    explicit QueryParams(std::string_view query) noexcept
    {
        for(std::size_t pos = 0; pos <= query.size();) {
            const auto amp = query.find('&', pos);
            const std::string_view pair =
                (amp == std::string_view::npos) ? query.substr(pos) : query.substr(pos, amp - pos);

            if(!pair.empty()) {
                const auto eq = pair.find('=');
                const std::string_view k = (eq == std::string_view::npos) ? pair : pair.substr(0, eq);
                const std::string_view v = (eq == std::string_view::npos) ? std::string_view{} : pair.substr(eq + 1);
                pairs_.push_back({k, v});
            }

            if(amp == std::string_view::npos)
                break;
            pos = amp + 1;
        }
    }

public:
    // Raw value, still percent-encoded if the client sent it that way. WFX doesn't decode this
    // for you, same as GetHeader doesn't decode header values, decode it yourself if you need to
    bool Get(std::string_view key, std::string_view& out) const noexcept
    {
        for(const auto& [k, v] : pairs_) {
            if(k == key) {
                out = v;
                return true;
            }
        }

        return false;
    }

    std::size_t Count() const noexcept
    {
        return pairs_.size();
    }

private:
    Shared::Vector<std::pair<std::string_view, std::string_view>> pairs_;
};

/* User side implementation of 'Request'. 'CoreEngine' passes the API */
class Request {
public:
    Request(void* backend) : backend_(backend)
    {}

public:
    Shared::HttpMethod Method() const
    {
        return Core::HttpApiExt1()->getMethod(backend_);
    }

    Shared::HttpVersion Version() const
    {
        return Core::HttpApiExt1()->getVersion(backend_);
    }

    std::string_view Path() const
    {
        auto sv = Core::HttpApiExt1()->getPath(backend_);
        return {sv.data, static_cast<std::size_t>(sv.length)};
    }

    std::string_view Body() const
    {
        auto sv = Core::HttpApiExt1()->getBody(backend_);
        return {sv.data, static_cast<std::size_t>(sv.length)};
    }

    // Path() keeps the raw '?...' suffix as-is. Parses it into a QueryParams once per call, so
    // if you're reading several keys, call this once and reuse the result rather than calling it
    // per key
    QueryParams GetQueryParams() const
    {
        const auto path = Path();
        const auto qpos = path.find('?');
        return QueryParams(qpos == std::string_view::npos ? std::string_view{} : path.substr(qpos + 1));
    }

public:
    bool GetHeader(std::string_view key, std::string_view& out) const
    {
        const Shared::StringView k = ToSV(key);
        Shared::StringView val{};

        const bool ok = Core::HttpApiExt1()->getHeader(backend_, k, &val);
        if(!ok)
            return false;

        out = {val.data, static_cast<std::size_t>(val.length)};
        return true;
    }

public:
    std::uint64_t SegmentCount() const
    {
        return Core::HttpApiExt1()->getSegmentCount(backend_);
    }

    Shared::SegmentVariant GetSegment(std::uint64_t index) const
    {
        return Core::HttpApiExt1()->getSegment(backend_, index);
    }

public:
    template <typename T> bool SetContext(std::string_view key, T&& value)
    {
        using U = std::decay_t<T>;

        auto k = ToSV(key);

        Shared::Any any{};
        any.typeID = Shared::Any::TypeIDOf<U>();

        if constexpr(sizeof(U) <= sizeof(void*) && alignof(U) <= alignof(void*) && std::is_trivially_copyable_v<U> &&
                     std::is_trivially_destructible_v<U>) {
            U tmp = std::forward<T>(value);
            std::memcpy(&any.data, &tmp, sizeof(U));
            any.destructor = nullptr;
        }
        else {
            U* obj = Shared::New<U>(std::forward<T>(value));
            if(!obj)
                return false;

            any.data = obj;
            any.destructor = [](void* p) { Shared::Delete(static_cast<U*>(p)); };
        }

        Core::HttpApiExt1()->setContext(backend_, k, any);
        return true;
    }

    template <typename T> auto GetContext(std::string_view key) const
    {
        using U = std::decay_t<T>;

        auto k = ToSV(key);
        Shared::Any any{};

        // For trivial types, return val + bool
        if constexpr(sizeof(U) <= sizeof(void*) && alignof(U) <= alignof(void*) && std::is_trivially_copyable_v<U> &&
                     std::is_trivially_destructible_v<U>) {
            if(!Core::HttpApiExt1()->getContext(backend_, k, &any) || any.typeID != Shared::Any::TypeIDOf<U>())
                return std::pair<U, bool>{{}, false};

            U out{};
            std::memcpy(&out, &any.data, sizeof(U));

            return std::pair<U, bool>{out, true};
        }
        // For non-trivial types, we return ptr + bool
        else {
            if(!Core::HttpApiExt1()->getContext(backend_, k, &any))
                return std::pair<U*, bool>{nullptr, false};

            return std::pair<U*, bool>{any.As<U>(), true};
        }
    }

    void EraseContext(std::string_view key)
    {
        auto k = ToSV(key);
        Core::HttpApiExt1()->eraseContext(backend_, k);
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

#endif // WFX_INC_HTTP_USER_REQUEST_HPP