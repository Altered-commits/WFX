# Encoding

Base64, hex, and URL (percent) encoding. Pure user-space, no engine ABI call involved.

!!! important
    All encoding functionality lives directly in the `WFX::` namespace.

    ```cpp
    #include <wfx/utils/encoding.hpp>
    ```

Every decode function returns `{bool, result}`; `false` means the input was malformed, and the result is empty in that case.

---

### Base64

```cpp
String Base64Encode(std::string_view data, bool urlSafe = false, bool padded = true);
std::pair<bool, Vector<std::uint8_t>> Base64Decode(std::string_view data);
```

- `Base64Encode`: `urlSafe` swaps `+`/`/` for `-`/`_`; `padded` controls trailing `=` characters.
- `Base64Decode`: accepts either alphabet in the same call, so it doesn't matter whether the input was produced with `urlSafe = true` or `false`. Trailing `=` is stripped if present but never required, so padded and unpadded input both decode correctly. Returns `{false, {}}` on any character outside both alphabets.

**Example**:
```cpp
auto encoded = WFX::Base64Encode(data, /* urlSafe */ true, /* padded */ false);

auto [ok, raw] = WFX::Base64Decode(encoded);
if(!ok) {
    // malformed base64 character
}
```

---

### Hex

```cpp
String HexEncode(std::string_view data, bool upper = false);
std::pair<bool, Vector<std::uint8_t>> HexDecode(std::string_view data);
```

- `HexEncode`: `upper` selects `A-F` instead of `a-f`.
- `HexDecode`: returns `{false, {}}` on an odd-length input or any non-hex character.

**Example**:
```cpp
auto hex = WFX::HexEncode(data);

auto [ok, bytes] = WFX::HexDecode(hex);
if(!ok) {
    // odd length, or a character outside 0-9a-fA-F
}
```

---

### URL (percent-encoding)

```cpp
String UrlEncode(std::string_view data);
std::pair<bool, String> UrlDecode(std::string_view data);
```

RFC 3986 percent-encoding. `UrlEncode` leaves the unreserved set (`A-Z a-z 0-9 - . _ ~`) untouched and percent-escapes everything else. `UrlDecode` also treats `+` as a space, tolerating form-encoded input alongside strict `%20`, and returns `{false, {}}` on a truncated or invalid `%XX` escape.

**Example**:
```cpp
auto encoded = WFX::UrlEncode("hello world/foo"); // "hello%20world%2Ffoo"

auto [ok, decoded] = WFX::UrlDecode(req.Query("redirect"));
if(!ok) {
    res.Status(WFX::HttpStatus::BAD_REQUEST).SendText("malformed redirect param");
    return;
}
```
