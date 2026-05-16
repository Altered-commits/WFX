# Overview

This page documents the public WFX API.

It defines how users are expected to interact with the engine at the source level, what guarantees are provided, and what guarantees are explicitly not provided. Anything not documented here is not part of the public API and may change, break, or be removed without notice.

Before using any API, it is important to understand the fundamental design decisions and constraints of WFX.

---

## Stability Status (Important)

WFX is under active development.

### API Stability

- **Not guaranteed at this stage**
- Public APIs may change as the engine evolves
- Breaking changes are expected until the first official stable release on the `main` branch

### ABI Stability

- **Partially implemented**
- A C-compatible ABI layer using POD types and stable layouts has been introduced
- Initial cross-toolchain testing indicates consistent binary compatibility
- **However:**
    - This has **not been exhaustively validated**
    - Edge cases (e.g. struct padding, alignment differences, compiler-specific behavior) may still exist
    - No formal ABI compatibility guarantees are made yet

!!! note
    The ABI layer is designed with long-term stability in mind, but should be treated as **experimental** until broader validation is complete.

---

## Execution Model

WFX follows a synchronous execution model.

This refers to the order in which engine logic executes and how control flows through the system. Even when using asynchronous APIs, execution order is deterministic and controlled by the engine. Asynchronous operations do not imply arbitrary or parallel execution of user code.

Users should assume that callbacks and async-related APIs still execute within a well-defined engine-controlled flow.

---

## Error Handling and Misuse

Not all APIs are guarded by runtime checks.

Some APIs perform internal validation and will fail loudly if misused. Depending on the API, misuse may cause the engine to terminate via a fatal error or result in the operation failing silently with an error logged.

Other APIs assume correct usage and provide little to no protection against invalid input or incorrect call order.

These runtime checks are meant to catch developer mistakes, not to sanitize or validate external input. Data coming from outside the engine (network requests, files, etc.) is always properly error-checked and handled separately.

The expectation is that users do not attempt random or undefined usage patterns. If an API is documented with constraints, those constraints are mandatory.

---

## Scope of the API Reference

This API reference documents only the public surface of WFX.

Internal systems, internal headers, undocumented behavior, and implementation details are intentionally excluded. Relying on internal behavior or undocumented side effects is unsupported and unsafe.

If a feature or behavior is not documented here, it should be treated as non-existent from a public API perspective.

---

## Updates and Versioning

The primary focus is establishing a stable and consistent API surface.

Right now WFX is under active development. Bug fixes, internal changes, and performance improvements may happen at any time. These changes may break source compatibility with previous builds, and binary compatibility is not considered.

In future updates, tracking versions and updates will be easier, as automatic update checks and notifications will be added. For now, users are expected to manually track releases and recompile their code against the current version of WFX.

---

## Getting Started with the API Reference

WFX API reference is organized into sections: Request & Response, Routing & Grouping, Middleware, and Constructors.

- If you are **new to WFX**, it is recommended to follow the sections **in order**, from Request & Response first, then Routing, and so on. This will give you a proper understanding of how the engine works and how components interact.  
- If you are an **experienced user** or looking for a specific API, you can jump directly to the section of interest. All sections are self-contained, so you can refer to them independently.  