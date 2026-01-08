# Templating

WFX includes a simple but high-performance template engine, hereafter referred to as **WTX**.

It is designed for **server-side HTML rendering** with a strong focus on:

- clarity,
- predictable behavior,
- and extreme performance in production.

---

## Overview

WTX operates in two modes:

### Production mode
- Templates are compiled:
    1. HTML => bytecode
    2. bytecode => generated C++
    3. generated C++ => shared library (DLL)
- The resulting libraries are **auto-linked into the engine**
- Templates are cached; files are **not reopened repeatedly**
- Rendering is effectively native code execution

### Debug mode
- Templates are still compiled to **bytecode**
- Bytecode is **interpreted**, not converted to C++ / DLL
- This enables fast iteration without recompilation

!!! note
    The debug-mode bytecode interpreter is not implemented yet.
    The core compilation pipeline is complete; the interpreter is a straightforward addition and will be built later.

The template language itself is identical in both modes.

---

## Capabilities and Limits

**Supported constructs**:

- Variables (`var`)
- Logical expressions
- Conditionals (`if`, `elif`, `else`)
- Loops (`for`)
- Template inheritance (`extends`)
- Includes (`include`)
- Partials (`partial`)
- Named blocks (`block` / `endblock`)

**Unsupported constructs**:

- Functions
- User-defined macros
- Variable mutation
- Arbitrary code execution
- Side effects

This feature set is **Django-like**, with minor additions (`partial`, `include`) for optimization.

---

## SendTemplate

`SendTemplate(...)` is the **core function** for sending any HTML template from the `templates/` folder.  
All templates (static or dynamic) are sent via this function.

### Path resolution
- Paths are **relative to the `templates/` folder**.
- Example:
    ```cpp
    // templates/index.html exists
    res.SendTemplate("index.html");
    ```

### Static vs dynamic templates

Whether a template is treated as **static** or **dynamic** depends on the constructs it contains:

- **Static templates**:

    - Contain only `include`, `partial`, `extends`, `block`, and `endblock`
    - Fully compiled to pre-rendered HTML
    - Sent using **zero-copy file send** (no JSON context needed)

- **Dynamic templates**:

    - Contain `if`, `elif`, `else`, `endif`, `for`, `endfor`, or `var`
    - Compiled into DLLs
    - Require the JSON parameter of `SendTemplate` for runtime data binding

## Constructs

This section explains the template language constructs used by WTX.
Each construct is shown with a **simple template example** and the **corresponding C++ code**
that calls `SendTemplate`.

### Variables

Variables must be **explicitly declared** before they can be used.

**Template example**:
```html
<!-- welcome.html -->
<h1>Welcome</h1>

<p>
  Name: {% var name %}
</p>

<p>
  Logged in: {% var logged_in %}
</p>
```

**C++ usage**:
```cpp
res.SendTemplate("welcome.html", Json::object({
    {"name", "Alice"},
    {"logged_in", true}
}));
```

**Key points**:

- `{% var name %}` declares the variable name
- Values are supplied through the JSON context
- Variable output is HTML-escaped by default

!!! note
    If a variable is declared but not present in the JSON context,
    it is substituted with an empty value (renders as blank).

### Conditionals

Conditionals control whether a block of HTML is rendered.

**Template example**:
```html
<!-- status.html -->
{% if logged_in && user.is_verified %}
  <p>Welcome back, {% var user.name %}.</p>
{% elif logged_in %}
  <p>Your account is not verified.</p>
{% else %}
  <p>Please sign in.</p>
{% endif %}
```

**C++ usage**:
```cpp
res.SendTemplate("status.html", Json::object({
    {
        "logged_in", true
    },
    {
        "user", Json::object({
            {"name", "Alice"},
            {"is_verified", false}
        })
    }
}));
```

**Key points**:

- Conditions are evaluated at render time
- No side effects or variable mutation is allowed

### Operators

#### Logical and Comparision

WTX supports logical and comparison operators.
Their behavior matches C++.

| Operator | Meaning               |
|----------|-----------------------|
| `&&`	   | Logical AND           |
| `||`	   | Logical OR            |
| `!`	     | Logical NOT           |
| `==`	   | Equal                 |
| `!=`	   | Not equal             |
| `>`	     | Greater than          |
| `>=`	   | Greater than or equal |
| `<`	     | Less than             |
| `<=`	   | Less than or equal    |

**Template example**:
```html
{% if logged_in && age >= 18 %}
  <p>Access granted.</p>
{% endif %}
```

**C++ usage**:
```cpp
res.SendTemplate("access.html", Json::object({
    {"logged_in", true},
    {"age", 21}
}));
```

#### Nested access

WTX supports nested access into **JSON objects** using the dot (`.`) operator.

**Template example**:
```html
{% if user.profile.is_active %}
  <p>Profile is active.</p>
{% endif %}

<p>
  Email: {% var user.profile.email %}
</p>
```

**C++ usage**:
```cpp
res.SendTemplate("profile.html", Json::object({
    {
        "user", Json::object({
            {
                "profile", Json::object({
                    {"is_active", true},
                    {"email", "alice@example.com"}
                })
            }
        })
    }
}));
```

**Key points**:

- Every intermediate segment must resolve to a JSON object; the final segment may be any JSON value.
- Nested access can be chained (`a.b.c`)
- Missing keys result in an empty value
- Access is read-only; no mutation occurs

### Loops

Loops allow repeated rendering over arrays provided in the JSON context.

**Template example**:
```html
<!-- users.html -->
<h2>Users</h2>

<ul>
{% for user in users %}
  <li>
    Name: {% var user.name %},
    Age: {% var user.age %}
  </li>
{% endfor %}
</ul>
```

**C++ usage**:
```cpp
res.SendTemplate("users.html", Json::object({
    {
        "users", Json::array({
            Json::object({
                {"name", "Alice"},
                {"age", 21}
            }),
            Json::object({
                {"name", "Bob"},
                {"age", 30}
            })
        })
    }
}));
```

**Key points**:

- `{% for item in collection %}` iterates over a JSON array
- The loop variable (`item`) is scoped to the loop body only
- Each element must be a JSON object to support dotted access
- Nested loops are allowed
- If the collection variable does not exist or is not a JSON array,
  the loop is not evaluated and the body renders nothing
- No loop control (`break`, `continue`) is supported
- The collection is **read-only**; mutation is not possible

### Includes

`include` inserts another template **verbatim** at **compile time**.

**Template example**:
```html
<!-- base.html -->
<html>
  <body>
    {% include 'header.html' %}
    <main>
      <p>Main content</p>
    </main>
    {% include 'footer.html' %}
  </body>
</html>
```

**Key points**:

- Included templates cannot receive a separate context
- Missing include files are a compile-time error
- Includes are best used for static, reusable fragments

### Partials

`partial` is **not** a dynamic include.  
It is a **compile-time optimization marker** for templates that are **never rendered directly**.

A template marked as `partial` is intended to be **used only as a base** (via `include` or `extends`) and **never sent on its own**.

**Template example**:
```html
<!-- user_card.html -->
{% partial %}
<div class="card">
  <p>Name: {% var user.name %}</p>
</div>
```
```html
<!-- page.html -->
<h1>Dashboard</h1>

{% include 'user_card.html' %}
```

**C++ usage**:
```cpp
res.SendTemplate("page.html", Json::object({
    {
        "user", Json::object({
            {"name", "Alice"}
        })
    }
}));
```

**Key points**:

- `{% partial %}` **must be the very first bytes in the file**
- The directive must be written **exactly as-is**; no whitespace, no BOM, no comments before it
- Partial templates are **never evaluated or rendered as entry-point templates**.
  This saves some processing time but is purely an optimization
- Using `partial` is **optional**

### Inheritance

`extends` enables template inheritance using a base layout at **compile time**.

**Template example**:

```html
<!-- base.html -->
<html>
  <body>
    {% block content %}{% endblock %}
  </body>
</html>
```
```html
<!-- home.html -->
{% extends 'base.html' %}

{% block content %}
  <h1>Home</h1>
  <p>Welcome.</p>
{% endblock %}
```

**Key points**:

- `extends` must appear **at the top of the template**
- Only one base template is allowed
- The base template defines the overall structure
- Child templates only fill named blocks

### Blocks

Blocks (`block` / `endblock`) define named replacement regions in a base template at **compile time**.

**Template example**:
```html
<!-- layout.html -->
<html>
  <head>
    {% block head %}{% endblock %}
  </head>
  <body>
    {% block body %}{% endblock %}
  </body>
</html>
```
```html
<!-- page.html -->
{% extends 'layout.html' %}

{% block head %}
  <title>Page</title>
{% endblock %}

{% block body %}
  <p>Page content</p>
{% endblock %}
```

**Key points**:

- Blocks are identified by name
- A child block fully replaces the parent block
- Nested blocks are **NOT** allowed
- Blocks cannot be conditionally defined
- Undefined blocks render the parent's default content
- Block names must be unique within a template