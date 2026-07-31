# JWK

Turns a JWKS JSON document (RFC 7517) into an `AsymKey` by `kid`. This is the JOSE-specific layer built on top of [`AsymKey`](crypto.md#asymmetric-signverify); it exists for the common case of a third party (an identity provider, an OAuth server, Cloudflare Access, etc.) publishing its public keys as a JWKS endpoint instead of a plain PEM file.

!!! important
    ```cpp
    #include <wfx/utils/jwk.hpp>
    ```

**Signature:**
```cpp
std::pair<CryptoStatus, AsymKey> LoadJwk(std::string_view jwksJson, std::string_view kid);
```

- `jwksJson`: the raw JWKS document, a JSON object with a top-level `"keys"` array.
- `kid`: the key ID to look for. Every entry in `"keys"` is scanned until one has a matching `"kid"` field.
- Returns `{CryptoOk, key}` on success, `key` is always a public-only `AsymKey`.

### Status codes

- **`CryptoInvalidArg`**: the document isn't valid JSON, `"keys"` isn't an array, `kid` isn't found in any entry, a matched entry's `n`/`e`/`x`/`y` field isn't valid base64, or the resulting key fails `AsymKey`'s own construction (see the public-key validation note in [Asymmetric](crypto.md#asymmetric-signverify)).
- **`CryptoUnsupported`**: the matched entry's `kty` isn't `"RSA"` or `"EC"`, or its `crv` isn't `"P-256"` or `"P-384"`.

Only `"RSA"` and `"EC"` key types are supported, matching the two RSA/EC branches of `AsymKey::FromRsaPublic`/`FromEcPublic`. Ed25519 JWKS entries (`kty: "OKP"`) are not handled.

---

### Example (verifying a Cloudflare Access token)

Cloudflare Access exposes its signing keys at `/cdn-cgi/access/certs`. A JWT issued by Access carries a `kid` in its header identifying which of those keys signed it. This caches resolved `AsymKey`s per `kid`, `LoadJwk` only runs on a cache miss, and the JWKS body is only refetched over the network when the `kid` isn't in it either (a real rotation):

```cpp
inline const auto CfAccess = WFX::HttpEndpoint{"myteam.cloudflareaccess.com:443"};

static WFX::String g_jwksBody;
static std::unordered_map<WFX::String, WFX::AsymKey> g_keyCache;

WFX_POST("/protected", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view authHeader;
    if(!req.GetHeader("Authorization", authHeader) || !authHeader.starts_with("Bearer ")) {
        res.Status(WFX::HttpStatus::UNAUTHORIZED).SendText("missing bearer token");
        co_return;
    }

    auto token = authHeader.substr(7);

    // header.payload.signature
    auto dot1 = token.find('.');
    auto dot2 = token.find('.', dot1 == std::string_view::npos ? 0 : dot1 + 1);
    if(dot1 == std::string_view::npos || dot2 == std::string_view::npos) {
        res.Status(WFX::HttpStatus::BAD_REQUEST).SendText("malformed token");
        co_return;
    }

    auto signingInput = token.substr(0, dot2);
    auto [hdrOk, headerJson] = WFX::Base64Decode(token.substr(0, dot1));
    auto [sigOk, sig] = WFX::Base64Decode(token.substr(dot2 + 1));

    if(!hdrOk || !sigOk) {
        res.Status(WFX::HttpStatus::BAD_REQUEST).SendText("malformed token");
        co_return;
    }

    auto header = WFX::ParseJson({reinterpret_cast<char*>(headerJson.data()), headerJson.size()});
    if(!header.IsValid()) {
        res.Status(WFX::HttpStatus::BAD_REQUEST).SendText("malformed token header");
        co_return;
    }

    auto kid = WFX::String(header.object.Get("kid").AsString());
    auto it = g_keyCache.find(kid); // cache hit: no LoadJwk call at all

    if(it == g_keyCache.end()) {
        auto [status, key] = WFX::LoadJwk(g_jwksBody, kid);

        if(status != WFX::CryptoOk) {
            // kid not even in our cached JWKS body: could be a genuine rotation, refetch once
            auto [fetchStatus, out] = co_await CfAccess.Get("/cdn-cgi/access/certs");
            if(fetchStatus != WFX::EpOk || !out->IsSuccess()) {
                res.Status(WFX::HttpStatus::UNAUTHORIZED).SendText("could not refresh signing keys");
                co_return;
            }

            g_jwksBody = out->body;

            // already declared, hence the usage of tie
            std::tie(status, key) = WFX::LoadJwk(g_jwksBody, kid);

            if(status != WFX::CryptoOk) {
                res.Status(WFX::HttpStatus::UNAUTHORIZED).SendText("unknown signing key");
                co_return;
            }
        }

        it = g_keyCache.emplace(kid, std::move(key)).first;
    }

    auto sigView = std::string_view(reinterpret_cast<char*>(sig.data()), sig.size());
    if(it->second.Verify(WFX::CryptoRs256, signingInput, sigView) != WFX::CryptoOk) {
        res.Status(WFX::HttpStatus::UNAUTHORIZED).SendText("invalid token");
        co_return;
    }

    res.SendText("welcome");
    co_return;
});
```

!!! danger "Do not call `LoadJwk` per request"
    `LoadJwk` reparses the full JWKS document and reconstructs an `AsymKey` from scratch on every call. Cache the resulting `AsymKey` per `kid`, as above, so a warm request never calls it at all. The same reasoning as [Asymmetric](crypto.md#asymmetric-signverify)'s key-caching note applies here too.
