# JWT

Compact JWT (RFC 7519) parsing, standard claim checks, and signature verification. This is what you run a token through once you already have the right key, [JWK](jwk.md) is what gets you that key in the first place. Kept as free functions rather than one class since a caller may only need some of these: a token whose issuer never rotates keys might skip `JwtAlgToScheme`'s dispatch entirely and hardcode a scheme, for instance.

!!! important
    ```cpp
    #include <wfx/utils/jwt.hpp>
    ```

Fetching or caching the signing key itself (polling a JWKS endpoint, refreshing on a `kid` miss) is out of scope here, same as [JWK](jwk.md), that always depends on a specific provider's endpoint and HTTP client, and is left to the caller. See the example below for how the pieces fit together.

---

## Parsing

**Signature:**
```cpp
std::pair<CryptoStatus, JwtParts> ParseJwt(std::string_view token);
```

Splits `token` on its two dots, base64url-decodes each segment, and JSON-parses the header and payload. Does not verify the signature and does not check any claim, both are separate steps below.

- Returns `{CryptoOk, parts}` on success.
- **`CryptoInvalidArg`**: `token` doesn't have exactly two dots delimiting three segments, any segment fails to base64url-decode, or the header/payload isn't valid JSON once decoded.

`JwtParts`:

| Field | Type | Meaning |
|---|---|---|
| `header` | `JsonObject` | The decoded header, e.g. `{"alg": "RS256", "kid": "..."}` |
| `payload` | `JsonObject` | The decoded payload (claims) |
| `signature` | `Vector<std::uint8_t>` | The raw, decoded signature bytes |
| `signingInput` | `std::string_view` | `header + "." + payload`, exactly as it appeared on the wire, before decoding. This is what a signature actually covers |
| `alg` | `std::string_view` | The header's `"alg"`, e.g. `"RS256"`. Empty if absent |
| `kid` | `std::string_view` | The header's `"kid"`. Empty if absent |

`alg`/`kid` stay valid for as long as `parts.header` is alive, they're views into its own internal storage, not the original `token`.

---

## Claim checks

**Signatures:**
```cpp
bool JwtTimeClaimsValid(const JsonObject& payload);
bool JwtAudienceMatches(const JsonObject& payload, std::string_view expectedAud);
```

- **`JwtTimeClaimsValid`**: `exp` is required, a missing or already-passed `exp` fails. `nbf` is optional, checked only when present.
- **`JwtAudienceMatches`**: checks `payload`'s `"aud"` against `expectedAud`. Per RFC 7519, `aud` may be a bare string or an array of strings, both are handled, `expectedAud` only needs to match one entry of an array.

Neither function touches the signature. Call both before `VerifyJwtSignature` if you want to reject an obviously-invalid token before spending a public-key operation on it, or after, if you'd rather not leak timing information about which check failed first, whichever your threat model prefers.

---

## Signature verification

**Signature:**
```cpp
CryptoStatus VerifyJwtSignature(const JwtParts& parts, const AsymKey& key);
```

Verifies `parts.signingInput` against `parts.signature` using `key`, dispatching on `parts.alg` rather than assuming one.

- **`CryptoOk`**: the signature is valid.
- **`CryptoAuthFailed`**: `parts.alg` was recognized but the signature didn't verify under `key`.
- **`CryptoUnsupported`**: `parts.alg` wasn't one of `RS256`/`RS384`/`RS512`/`PS256`/`PS384`/`PS512`/`ES256`/`ES384`/`EdDSA`. `key` is never touched in this case, an unrecognized `alg` (including `"none"` or `"HS256"`) is rejected before any crypto operation runs, not by chance.

!!! important "Every alg your issuer might use has to be checked for explicitly"
    `VerifyJwtSignature` only accepts the exact `alg` values listed above. If a token's issuer signs with something else, it fails closed with `CryptoUnsupported`, it will never fall back to a different scheme or guess based on the key type.

---

## Example (verifying a Cloudflare Access token)

The four calls in this file, once you already have the right `AsymKey`:

```cpp
auto [ps, parts] = WFX::ParseJwt(token);
if(ps != WFX::CryptoOk)
    return false; // malformed token

if(!WFX::JwtAudienceMatches(parts.payload, Init::CloudflareAudienceTag) ||
   !WFX::JwtTimeClaimsValid(parts.payload))
    return false; // wrong audience, expired, or not yet valid

return WFX::VerifyJwtSignature(parts, key) == WFX::CryptoOk;
```

Getting `key` in the first place, resolving `parts.kid` against a JWKS endpoint with a cache in front of it, is [JWK](jwk.md)'s job. See [JWK's example](jwk.md#example-verifying-a-cloudflare-access-token) for the complete route, cache lookup, on-miss refetch, and these four calls together.
