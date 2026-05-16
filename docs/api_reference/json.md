# Json

WFX provides three JSON tools: two serializers for building and sending JSON responses, and a parser for reading incoming JSON from request bodies. No extra `#include` is needed (all three are available everywhere).

---

## Which tool do I use?

**Sending JSON in a response:**

- Use `WFX::ImJson` when you know the shape of your JSON ahead of time and want maximum performance. It writes directly into the response buffer with zero heap allocation.
- Use `WFX::RmJson` when your JSON shape depends on conditions, branches, or data you gather at runtime. It builds a full object in memory first, then sends it all at once.

**Reading JSON from a request:**

- Use `WFX::ParseJson` to parse a JSON body sent by a client.

---

## `ImJson` : Immediate mode

`ImJson` writes JSON directly into the response as you call each method. Nothing is stored in memory. The output is streamed token by token, which makes it fast and allocation-free.

```cpp
WFX_GET("/status", [](WFX::Request req, WFX::Response res) {
    auto w = WFX::ImJson(res);
    w.Write("status", "ok");
    w.Write("version", 1u);
});
```

When you call `WFX::ImJson(res)`, two things happen immediately:

- The `Content-Type: application/json` header is added.
- The opening `{` is written into the response.

When `w` goes out of scope at the end of the handler, the destructor closes any open scopes and calls `res.Commit()` for you. You do not need to commit the response manually.

!!! important
    `ImJson` holds a reference to `Response`. It must not outlive the route handler.

### Writing key-value pairs

Inside an object scope, use `Write(key, value)` to emit a field:

```cpp
auto w = WFX::ImJson(res);
w.Write("name",   "alice");
w.Write("id",     42);
w.Write("active", true);
w.Write("score",  9.5);
w.Write("token",  nullptr);   // emits null
```

**Supported value types**: `std::string_view`, `const char*`, `bool`, `std::int64_t`, `std::uint64_t`, `double`, `std::nullptr_t`, `Shared::UUID`. Narrower integer and float types are widened automatically.

### Nested objects

Open a nested object with `Obj(key)` and close it with `End()`:

```cpp
auto w = WFX::ImJson(res);
w.Write("id", 1);
w.Obj("address");
    w.Write("city",    "Berlin");
    w.Write("country", "DE");
w.End();
```

### Arrays

Open an array with `Arr(key)` and close it with `End()`. Inside an array, use `Push(value)` instead of `Write`:

```cpp
auto w = WFX::ImJson(res);
w.Arr("tags");
    w.Push("admin");
    w.Push("verified");
w.End();
```

### Objects inside arrays

When you are inside an array scope and want to emit an object element, use `Obj()` without a key:

```cpp
auto w = WFX::ImJson(res);
w.Arr("users");
    w.Obj();
        w.Write("id",   1);
        w.Write("name", "alice");
    w.End();
    w.Obj();
        w.Write("id",   2);
        w.Write("name", "bob");
    w.End();
w.End();
```

Similarly, `Arr()` without a key opens an anonymous array inside an array.

### Full example

```cpp
WFX_GET("/report", [](WFX::Request req, WFX::Response res) {
    auto w = WFX::ImJson(res);
    w.Write("status", "ok");
    w.Obj("meta");
        w.Write("region",  "eu-west");
        w.Write("version", 2u);
    w.End();
    w.Arr("items");
        w.Obj(); w.Write("id", 1); w.Write("name", "pencil"); w.End();
        w.Obj(); w.Write("id", 2); w.Write("name", "pen");    w.End();
    w.End();
});
```

Output:

```json
{
  "status": "ok",
  "meta": { "region": "eu-west", "version": 2 },
  "items": [
    { "id": 1, "name": "pencil" },
    { "id": 2, "name": "pen" }
  ]
}
```

!!! danger
    `ImJson` does not validate your call order. Calling `Write` after `End()` has closed a scope, or mismatching `Obj`, `Arr`, and `End` calls, will produce malformed JSON with no error or warning. Always make sure every `Obj` and `Arr` has a matching `End`.

---

## `RmJson` : Retained mode

`RmJson` builds a complete JSON object in memory. You populate it using familiar subscript syntax, then send it when ready. Use this when your response structure depends on runtime data or branches.

```cpp
WFX_GET("/profile", [](WFX::Request req, WFX::Response res) {
    auto o = WFX::RmJson();
    o["username"] = "alice";
    o["id"]       = 42u;
    o.Write(res);
});
```

`Write(res)` adds `Content-Type: application/json`, serializes the full object, and commits the response.

### Setting values

Assign values directly with `operator[]` and `operator=`:

```cpp
auto o = WFX::RmJson();
o["name"]   = "alice";
o["id"]     = 42u;
o["active"] = true;
o["score"]  = 9.5;
o["token"]  = nullptr;    // null
```

**Supported types**: `bool`, `std::int64_t`, `std::uint64_t`, `double`, `std::string_view`, `const char*`, `std::nullptr_t`, `Shared::UUID`, and narrower variants.

!!! tip
    `const char*` values are stored as zero-copy views. The pointed-to string must remain alive for as long as the `RmJson` object exists. `std::string_view` values are copied into internal storage and are safe to use with temporaries.

### Nested objects

Chain `operator[]` to create nested keys. Intermediate objects are created automatically:

```cpp
auto o = WFX::RmJson();
o["address"]["city"]    = "Berlin";
o["address"]["country"] = "DE";
o["meta"]["version"]    = 2u;
```

### Arrays

Access a key as a `JsonRef` and call `PushBack` on it:

```cpp
auto o = WFX::RmJson();
auto tags = o["tags"];
tags.PushBack("admin");
tags.PushBack("verified");
tags.PushBack(42u);
```

`PushBack` accepts the same types as `operator=`, plus `std::nullptr_t`.

### Reading values back

You can read values out of an `RmJson` object using `Get` and type accessors. This is useful when building responses that depend on data you stored earlier in the same object.

```cpp
auto ref = o.Get("id");

if(ref.IsUInt())
    auto id = ref.AsUInt();
```

**Type checks**: `IsNull()`, `IsBool()`, `IsInt()`, `IsUInt()`, `IsDouble()`, `IsString()`, `IsArray()`, `IsObject()`.

**Value accessors**: `AsBool()`, `AsInt()`, `AsUInt()`, `AsDouble()`, `AsString()` which returns `std::string_view`.

For arrays and objects, `Length()` returns the number of elements or keys. Array elements are accessed by index:

```cpp
auto first = o["tags"][0u];
```

### Erasing keys

```cpp
o.Erase("token");
```

For arrays, pass an index instead:

```cpp
o["tags"].Erase(1u);   // removes element at index 1
```

### Merging two objects

`Merge` copies all top-level keys from another `RmJson` object into the current one. Existing keys are overwritten:

```cpp
auto base  = WFX::RmJson();
base["type"] = "user";

auto extra   = WFX::RmJson();
extra["role"] = "admin";

base.Merge(extra);
base.Write(res);   // {"type":"user","role":"admin"}
```

Merge is shallow. Nested objects inside the source are aliased, not deep-copied.

### Capacity hints

If you know roughly how large your object will be, you can pass hints to avoid internal reallocations:

```cpp
auto o = WFX::RmJson(
    32,    // expected number of nodes
    16,    // expected number of key-value pairs
    256    // expected bytes of string data
);
```

The object grows automatically if these limits are exceeded. Hints are purely advisory and exist for performance tuning.

### Full example

```cpp
WFX_GET("/order", [](WFX::Request req, WFX::Response res) {
    auto o = WFX::RmJson();
    o["id"]     = 1001u;
    o["status"] = "confirmed";

    auto items = o["items"];
    items.PushBack("notebook");
    items.PushBack("pen");

    o["shipping"]["city"]   = "Berlin";
    o["shipping"]["method"] = "express";

    o.Write(res);
});
```

---

## Parsing request bodies

`ParseJson` parses a JSON string into an `RmJson` compatible object that you can read using the same `Get`, `IsX`, and `AsX` methods described above.

```cpp
inline Shared::JsonParseResult ParseJson(
    std::string_view body,
    bool             view     = false,
    std::uint32_t    maxDepth = 64
) noexcept;
```

### Parameters

- **`body`** -> the raw JSON text to parse, typically from `req.Body()`.
- **`view`** -> controls how string values are stored internally. When `false` (the default), all strings are copied into the object's internal storage and remain valid even after `body` is gone. When `true`, strings reference the original input directly, which is faster but requires `body` to remain alive for as long as you use the parsed object.
- **`maxDepth`** -> maximum allowed nesting depth. Inputs that exceed this are rejected. Defaults to 64.

### Return value

`ParseJson` returns a `JsonParseResult` which contains:

- **`object`** -> the parsed `JsonObject`, valid only if parsing succeeded.
- **`error`** -> a `const char*` that is `nullptr` on success, or a short error message on failure.
- **`offset`** -> the byte offset into `body` where the failure occurred, when `error` is set.
- **`IsValid()`** -> convenience method that returns `true` when `error` is `nullptr`.

### Basic usage

```cpp
WFX_POST("/login", [](WFX::Request req, WFX::Response res) {
    auto result = WFX::ParseJson(req.Body());

    if(!result.IsValid()) {
        res.Status(WFX::HttpStatus::BAD_REQUEST)
           .SendText("Invalid JSON");
        return;
    }

    auto username = result.object.Get("username");
    auto password = result.object.Get("password");

    if(!username.IsString() || !password.IsString()) {
        res.Status(WFX::HttpStatus::BAD_REQUEST)
           .SendText("Missing fields");
        return;
    }

    // username.AsString() and password.AsString() are std::string_view
});
```

### Reading nested data

The parsed object uses the same interface as `RmJson`. You can chain `Get` calls or use `operator[]` to reach nested values:

```cpp
auto city = result.object.Get("address").Get("city");

if(city.IsString())
    auto name = city.AsString();
```

### Reading arrays

Use `Length()` to get the element count and `operator[]` with an index to access elements:

```cpp
auto tags = result.object.Get("tags");

if(tags.IsArray()) {
    for(std::uint32_t i = 0; i < tags.Length(); i++) {
        auto tag = tags[i];
        if(tag.IsString()) {
            // use tag.AsString()
        }
    }
}
```

### Error handling

Always check `IsValid()` before accessing `object`. Accessing a failed result's object is undefined behavior.

```cpp
auto result = WFX::ParseJson(req.Body());

if(!result.IsValid()) {
    // result.error  is a short description of what went wrong
    // result.offset is the byte position in the input where it failed
    res.Status(WFX::HttpStatus::BAD_REQUEST).SendText("Bad JSON");
    return;
}
```

!!! important
    The JSON root must be an object `{}` or an array `[]`. Bare values like `"hello"` or `42` at the top level are rejected.

!!! note
    When `view = false` (the default), you can safely discard `req.Body()` or let it go out of scope after parsing. When `view = true`, do not access the parsed object after the request lifecycle ends, as the underlying string data will no longer be valid.