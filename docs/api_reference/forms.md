# Forms

Forms in WFX provide a **statically-defined, explicit, and deterministic** way to **parse, validate, sanitize, and render** structured user input.  
They are designed to **fail fast**, avoid ambiguity, and prevent malformed data from ever reaching application logic.

!!! important
    All form functionality in WFX lives inside the `Form::` namespace.

    To use forms, you **must** include the forms header at the top of your file:
    ```cpp
    #include <core/forms.hpp>
    ```

---

## Overview

A form in WFX consists of four conceptual layers:

- [**Schema**](#schema): the static, immutable definition of the form
- [**Fields**](#fields): named inputs inside the schema
- [**Rules**](#rules): specifies how each field should be parsed and constrained
- [**Cleaning**](#cleaning): validation + sanitization (both mandatory, fail as a single step)

Optionally, forms may also define **rendering** logic for SSR.

Each layer is explicit and user-controllable.

---

## Form Errors

Every form operation results in a `FormError`.  
Forms **never throw** and **never partially succeed**.

```cpp
enum class FormError : std::uint8_t {
    NONE,
    UNSUPPORTED_CONTENT_TYPE,
    MALFORMED,
    CLEAN_FAILED
};
```

- `NONE`: Parsing, validation, and sanitization succeeded
- `UNSUPPORTED_CONTENT_TYPE`: The request either lacks a `Content-Type` header or uses a type the form system does not support
- `MALFORMED`: The request body could not be decoded into key-value input (e.g. broken URL encoding)
- `CLEAN_FAILED`: One or more fields failed validation or sanitization

If the error is not `NONE`, application logic must not continue.

---

## Form Layers

Each layer has a single responsibility and a clearly defined failure boundary.
Understanding these layers is mandatory to use the form system correctly.

### **Schema**

A **schema** is the complete, immutable definition of a form.

In WFX, schemas are represented by `Form::FormSchema` and are intended to be defined statically.

It is constructed with:

- The form name (`const char*`)
- One or more `Form::Field(...)` definitions, **in order**

**Example**:
```cpp
inline const auto LoginForm = Form::FormSchema{
    "login",
    Form::Field("username", Form::Text{ .ascii = true, .max = 64 }),
    Form::Field("password", Form::Text{ .ascii = true, .min = 6, .max = 64 })
};
```

This defines a form schema named `login` with two text fields.
This schema is **immutable**. **Fields and rules** cannot be changed later.

!!! important
    **Field ordering is critical.**

    Fields are evaluated **strictly in the order they are defined** inside the schema.  
    This order is used for:

    - Input parsing and evaluation
    - Validation and sanitization
    - Cleaned tuple layout
    - Field rendering via `Render()`

    Changing field order or introducing mismatches can **invalidate form input**, break tuple access,
    and produce incorrect or unintended rendering output.

    Always define fields in the exact order you expect them to be processed and rendered.

### **Fields**

A **field** represents a single named input expected by the form.

Fields are declared using the `Form::Field` factory function and are always part of a schema.  
They do not exist independently.

A field is constructed with:

- The field name (`const char*`)
- A rule instance describing the expected input type (e.g. `Form::Text`, `Form::Int`)

**Example**:
```cpp
Form::Field("username", Form::Text{ .ascii = true, .max = 64 })
```

**Custom Overrides**

All built-in field types provide engine-defined default validators and sanitizers.
In most cases, no additional configuration is required.

Chaining functions are provided **only to override** these defaults when needed.

**Example**:
```cpp
// Custom validator and sanitizer
Form::Field("username", Form::Text{ .ascii = true, .max = 64 })
        .CustomValidator(&MyValidator)
        .CustomSanitizer(&MySanitizer)

// Or individually
Form::Field("username", Form::Text{ .ascii = true, .max = 64 })
        .CustomValidator(&MyValidator)

Form::Field("password", Form::Text{ .ascii = true, .max = 64 })
        .CustomSanitizer(&MySanitizer)
```

!!! note
    - Overrides replace the default behavior
    - Overrides do not stack; the last override wins

    For details on validators, sanitizers, default behavior, and custom type support,
    see the [**Cleaning**](#cleaning) section.

### **Rules**

A **rule** defines the expected type and constraints of a field's input.

Rules are plain configuration objects. They do **not** parse input themselves.
They are consumed by the form system to perform **validation, sanitization, and rendering**.

Every field must be constructed with **exactly one rule instance**.

**Example**:
```cpp
Form::Int{ .min = 18, .max = 120 }
```

#### **Base Interface**

All rules **MUST inherit** the following common property:

```cpp
struct BaseRule {
    bool required = true;
};
```

- `required = true`: input must be present
- `required = false`: input may be omitted

#### **Decayed Type**

Each rule is associated with a **decayed type**, representing the **final C++ type** produced
by the form system after sanitization and validation.
This enables **compile-time type safety** for all form operations

The decayed type is determined via the `Form::DecayedType<Rule>` trait.

#### **Builtins**

WFX provides the following built-in rules:

- `Text`

    **Usage**:
    ```cpp
    Form::Text{
        .ascii = /* true | false */,
        .min   = /* minimum length */,
        .max   = /* maximum length */
    }
    ```

    - `ascii` (`bool`): restrict input to ASCII characters only  
    - `min` (`std::uint32_t`): minimum allowed length  
    - `max` (`std::uint32_t`): maximum allowed length  

    **Decayed type**: `std::string_view`

- **`Email`**

    **Usage**:
    ```cpp
    Form::Email{
        .strict = /* true | false */
    }
    ```

    - `strict` (`bool`): enable strict email validation rules  

    **Decayed type**: `std::string_view`

- **`Int`**

    **Usage**:
    ```cpp
    Form::Int{
        .min = /* minimum value */,
        .max = /* maximum value */
    }
    ```

    - `min` (`std::int64_t`): minimum allowed value  
    - `max` (`std::int64_t`): maximum allowed value  

    **Decayed type**: `std::int64_t`

- **`UInt`**

    **Usage**:
    ```cpp
    Form::UInt{
        .min = /* minimum value */,
        .max = /* maximum value */
    }
    ```

    - `min` (`std::uint64_t`): minimum allowed value  
    - `max` (`std::uint64_t`): maximum allowed value  

    **Decayed type**: `std::uint64_t`

- **`Float`**

    **Usage**:
    ```cpp
    Form::Float{
        .min = /* minimum value */,
        .max = /* maximum value */
    }
    ```

    - `min` (`double`): minimum allowed value  
    - `max` (`double`): maximum allowed value  

    **Decayed type**: `double`

!!! note
    The members shown here are only the members specific to the builtin rule itself.  
    All **common members** (e.g. `.required`, etc.) are defined in the **`BaseRule`** struct, which is documented **earlier** in the *Base interface* section.  
    They are **not repeated** for each builtin rule to avoid duplication.  
    If a member is not listed here, assume it comes from **`BaseRule`** and **still applies**.

#### **Custom**

Custom rules may be defined when built-in rules are insufficient.

A custom rule **must**:

- Inherit from `Form::BaseRule`
- Define its own configuration fields
- Provide a `DecayedType` specialization mapping the rule to a concrete value type
- Have a default sanitizer and validator available to the engine

**Example**:
```cpp
struct MyRule : Form::BaseRule {
    std::size_t limit;
};

template<>
struct Form::DecayedType<MyRule> {
    using Type = std::size_t;
};
```

!!! note
    More details on default sanitizers, validators, and how to define them are provided in the [**Cleaning**](#cleaning) section.

### **Cleaning**

The form system processes input in two stages: **validation** and **sanitization**,
to produce the final C++ value for each field.  

!!! important
    The **order of stages is critical**: **validation runs before sanitization**.  
    The form system first checks that input satisfies the rule constraints, then applies sanitization to produce the final C++ value.  
    All logic should follow this sequence to ensure correct and type-safe processing.

#### **Validation**

Validation is the **first stage** of the cleaning pipeline.  
It determines whether the raw input is *acceptable* for a given rule **without producing a value**.

At this stage:

- Input is treated as raw `std::string_view`
- No type conversion is performed
- The rule is accessed via a **type-erased pointer**
- The result is a simple success/failure signal

If validation fails, the pipeline **terminates immediately** and sanitization is never executed.

**Validator signature**:
```cpp
// Input: Form data, Form field (type erased)
// Output: Validation success via bool return value
using ValidatorFn = bool (*)(std::string_view, const void*);
```

#### **Sanitization**

Sanitization is the **second stage** of the cleaning pipeline.  
It is responsible for **normalizing and converting already-validated input** into a concrete C++ value.

At this stage:

- Input is still received as `std::string_view`
- Validation has already succeeded
- Type conversion and normalization occur here
- The final decayed C++ value is produced

**Sanitizer signature**:
```cpp
// Input: raw field data and a type-erased field pointer
// Output: success via bool return value
//         sanitized value written to `out` of type T
template<typename T>
using SanitizerFn = bool (*)(std::string_view input, const void* fieldPtr, T& out);
```

#### **Custom**

Custom rules must explicitly provide **validation** and **sanitization** logic.
Only the minimum theory required to implement them correctly is covered here.

A custom rule must expose:

- A **validator** (`ValidatorFn`)
- A **sanitizer** (`SanitizerFn<T>`)
- Dispatcher overloads for both
- A `DecayedType` mapping to the final C++ type

**Example (Defining custom rules)**:
```cpp
#include <form/forms.hpp>
#include <cctype>
#include <charconv>

// Custom Rule
struct HexUInt : Form::BaseRule {
    std::uint64_t max = UINT64_MAX;
};

// Decayed Type
template<>
struct Form::DecayedType<HexUInt> {
    using Type = std::uint64_t;
};

// Validator: Checks that the input is a valid hexadecimal string
static inline bool ValidateHexUInt( // Must follow validator signature
    std::string_view sv,
    const void* fieldPtr
) {
    const HexUInt& r = *static_cast<const HexUInt*>(fieldPtr);

    if(sv.empty())
        return false;

    for(char c : sv) {
        if(!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
    }

    return true;
}

// Validator Dispatcher
static constexpr Form::ValidatorFn DefaultValidatorFor(const HexUInt&)
{
    return ValidateHexUInt;
}

// Sanitizer: Performs the actual conversion and range checking
static inline bool SanitizeHexUInt( // Must follow sanitizer signature
    std::string_view sv,
    const void* fieldPtr,
    std::uint64_t& out              // Here 'T' is our decayed type
) {
    const HexUInt& r = *static_cast<const HexUInt*>(fieldPtr);

    std::uint64_t value = 0;
    auto [ptr, ec] = std::from_chars(
        sv.data(), sv.data() + sv.size(), value, 16
    );

    if(ec != std::errc{})
        return false;

    if(value > r.max)
        return false;

    out = value;
    return true;
}

// Sanitizer Dispatcher
static constexpr
Form::SanitizerFn<std::uint64_t> DefaultSanitizerFor(const HexUInt&)
{
    return SanitizeHexUInt;
}

// Now your custom rule is ready to be used :)
```

!!! tip
    - You may intentionally **skip validation** if the same checks would be repeated during sanitization.
    For example, if sanitization already performs string-to-number conversion and failure detection,
    duplicating that work in validation is unnecessary.

    - Avoid hardcoding `Form::SanitizerFn<std::uint64_t>` or `std::uint64_t& out`.
    Instead, use `Form::SanitizerFn<Form::DecayedType<HexUInt>::Type>`.
    This ensures the sanitizer automatically stays correct if the rule’s output type is changed later, without requiring updates in multiple places.

**Example (Overriding default cleanup logic)**:
```cpp
// Perform parsing and checks in sanitization only
static bool SanitizeEvenInt(
    std::string_view sv,
    const void*,
    std::int64_t& out
) {
    auto [ptr, ec] = std::from_chars(
        sv.data(), sv.data() + sv.size(), out
    );

    if(ec != std::errc{})
        return false;

    return (out % 2) == 0;
}

// ...
Form::Field(
    "count",
    Form::Int{ .min = 0, .max = 100 }
)
.CustomSanitizer(SanitizeEvenInt);
// ...
```

---

## Form Usage

WFX forms can be parsed in multiple ways, depending on how much control you want.
If you are unsure which one to use, use `Parse`.

### **Parse**

**`Parse`** automatically selects the correct parsing strategy based on the request headers.

What it does:

- Reads the `Content-Type` header
- Chooses the correct parser internally
- Validates and cleans the form
- Produces a fully validated output tuple

Current support:

- `application/x-www-form-urlencoded`

Future support (no code changes required from you):

- `multipart/form-data`
- other form encodings

**Typical Usage**:
```cpp
/*
 * Parses the login form using the 'LoginForm' schema
 *
 * NOTE: 'LoginForm' is assumed to be the one defined in 'Schema' section
 *        of this documentation
 */
WFX_MIDDLEWARE("ParseForm", [](Request& req, Response& res, MiddlewareMeta _) {
    if(req.method != HttpMethod::POST)
        return MiddlewareAction::CONTINUE;

    LoginFormSchema::CleanedType output;

    if(LoginForm.Parse(req, output) != Form::FormError::NONE) {
        res.Status(HttpStatus::BAD_REQUEST)
           .SendText("Invalid form data");
        return MiddlewareAction::BREAK;
    }

    req.SetContext("login-form", std::move(output));
    return MiddlewareAction::CONTINUE;
});

/*
 * The parsed form data is stored as a tuple in the request context
 * Now we can use the stored form as:
 *
 *  // Get the form
 *  auto form = req.GetContext<LoginFormSchema::CleanedType>("login-form");
 *     
 *  // Accesses the first field in declaration order
 *  auto& username = std::get<0>(form);
 */
```

### **ParseStatic**

**`ParseStatic`** parses a raw form body directly.
It does not look at headers and does not guess the format.

What it does:

- Assumes the body is already `application/x-www-form-urlencoded` format
- Parses small, in-memory input
- Validates and cleans the form

**Typical Usage**:
```cpp
// Same semantics as the previous example, but the form is parsed directly-
// -from req.body
//
// In a real application, you should perform basic sanity checks yourself,-
// -such as validating the HTTP method (e.g. POST) and ensuring the-
// -'Content-Type' header is present and correct.

LoginFormSchema::CleanedType output;

if(LoginForm.ParseStatic(req.body, output) != Form::FormError::NONE) {
    // Handle failure
}

// Handle success
```

### **Accessing data**

On success, all above mentioned methods produce the same output type.

```cpp
auto& usertype = std::get<0>(output);
auto& passtype = std::get<1>(output);

// Access value via .value member (More information below)
std::string_view username = usertype.value;
```

- Access is positional (0-based indexing), based on field definition order
- Output is valid only if parsing succeeded

**Field value semantics (important)**

Each extracted element (e.g. `username`, `password`) is **not a raw type**.
It is a *cleaned type wrapper struct* with the following members:

```cpp
field.value
field.present
```

- `.value`  
    - Holds the parsed and validated value
    - Type is the **decayed field type** (after sanitization / normalization)
    - Safe to read **only if** parsing succeeded

- `.present`  
    - Indicates whether the field was actually provided in the input
    - Meaningful **only for optional fields** (`.required = false`)
    - Semantics:  
        - `true` -> field was present and parsed
        - `false` -> field was omitted by the user

    - For required fields, `.present` is always `true` on successful parsing

---

## Form Rendering

WFX provides field-level rendering only.

**What Is Rendered**

Each form schema pre-renders its fields at construction time.

```cpp
std::string_view html = LoginForm.Render();
```

**`Render()`** returns a view to HTML containing:

- `<label>` elements
- `<input>` elements
- One pair per field
- In the exact order the fields were defined

The output is static, pre-built, and allocation-free at render time.

**What Is Not Rendered**

WFX does not generate:

- `<form>` tags
- Submit buttons
- Custom attributes
- Layout or styling

These are always the responsibility of the user.

**Typical Usage**

**HTML Template**:
```html
<!-- ... -->
  <form class="login-form" method="POST">
    {% var login_form_fields %}

    <button type="submit" class="btn-enquire">
      Enquire Now
    </button>
  </form>
<!-- ... -->
```

**C++ Source Code**:
```cpp
// There are two supported ways to pass rendered form fields to templates:
//
// 1. Using Render()
//    - Returns a pre-rendered HTML string_view
//    - Simple and straightforward
//
// 2. Using FormToJson()
//    - Avoids an extra copy / allocation
//    - Technically more efficient
//
WFX_GET("/form", [](Request& req, Response& res) {
    res.SendTemplate("login-page.html", Json::object({
        { "login_form_fields", LoginForm.Render() }
        /* ... */
    }));
});

// Same result as above, but more optimized
WFX_GET("/form", [](Request& req, Response& res) {
    res.SendTemplate("login-page.html", Json::object({
        { "login_form_fields", Form::FormToJson(LoginForm) }
        /* ... */
    }));
});
```

**Rendered HTML**:
```html
<!-- ... -->
  <form class="login-form" method="POST">
    <label for="login__username">username</label>
    <input
        id="login__username"
        name="username"
        type="text"
        maxlength="64"
        pattern="[\x20-\x7E]*"
    />

    <label for="login__password">password</label>
    <input
        id="login__password"
        name="password"
        type="text"
        minlength="6"
        maxlength="64"
        pattern="[\x20-\x7E]*"
    />

    <button type="submit" class="btn-enquire">
      Enquire Now
    </button>
  </form>
<!-- ... -->
```

!!! important
    Design notes and guarantees

    - WFX intentionally renders **only form fields** (labels and inputs), not the `<form>` element itself.  
    This is by design, so you retain full control over CSRF tokens, honeypots, submit buttons,
    layout, and any additional markup outside the fields.

    - Every rendered input uses a stable and predictable `id` format: **`<form-schema-name>__<field-name>`**  
    This guarantees uniqueness across forms and allows reliable label association and
    client-side scripting without collisions.

!!! warning
    WFX currently does **not** validate global uniqueness of form schema names.

    This means the following is allowed but **incorrect**:

    ```cpp
    inline const auto LoginForm  = Form::FormSchema{ "login",  /* ... */ };
    inline const auto SigninForm = Form::FormSchema{ "login",  /* ... */ };
    ```

    Since rendered input IDs are generated using the format
    **`<form-schema-name>__<field-name>`**, both schemas above will produce
    **colliding `id` attributes** in the generated HTML.

    Consequences include:

    - Broken `<label for="...">` associations
    - Undefined behavior in client-side scripts
    - Hard-to-debug rendering issues

    Until explicit collision checks are added, it is **your responsibility**
    to ensure all form schema names are globally unique.