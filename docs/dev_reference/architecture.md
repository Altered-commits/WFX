# Architecture

This page describes how WFX initializes and runs at a high level. It does not cover internal implementation details.

---

## Startup flow

```mermaid
flowchart TD
    A[Start] --> B[Load config]
    B --> C[Generate SSL key]
    B --> D[Allocate shared metrics region]
    C --> E[Compile templates and user source]
    D --> E

    E --> F{Spawn N workers}

    F -->|Worker 0| W0
    F -->|Worker 1| W1
    F -->|Worker N| WN

    subgraph W0[ ]
        direction TB
        I0[Init subsystems] --> L0[Load user library] --> R0[Register routes] --> EL0([Event loop])
    end

    subgraph W1[ ]
        direction TB
        I1[Init subsystems] --> L1[Load user library] --> R1[Register routes] --> EL1([Event loop])
    end

    subgraph WN[ ]
        direction TB
        IN[Init subsystems] --> LN[Load user library] --> RN[Register routes] --> ELN([Event loop])
    end
```

---

## Request flow

```mermaid
flowchart TD
    A([Incoming connection]) --> B[Assign connection slot]
    B --> C{TLS enabled?}
    C -- Yes --> D[Perform handshake]
    C -- No --> E[Ready to receive]
    D --> E

    E --> F[Receive and parse request]
    F --> G{Parse result}

    G -- Incomplete --> H[Wait for more data]
    G -- Error --> I[Send 400, close]
    G -- Success --> J{Route match?}

    J -- No --> K[Send 404]
    J -- Yes --> L[Run global middleware]

    L --> M{Middleware result}
    M -- Break --> N[Send response]
    M -- Continue --> O[Run per-route middleware]

    O --> P{Middleware result}
    P -- Break --> N
    P -- Continue --> Q{Handler type}

    Q -- Sync --> R[Execute handler directly]
    Q -- Async --> S[Suspend coroutine, resume on completion]

    R --> N
    S --> N

    N --> T{Connection state}
    T -- Keep-alive --> U[Wait for next request]
    T -- Close --> V([Connection closed])
```

---

## Subsystem relationships

```mermaid
flowchart TD
    Master[Master Process]
    SM[Shared Metrics Region]

    Master -->|allocates before spawn| SM
    Master -->|spawns| W0
    Master -->|spawns| W1
    Master -->|spawns| WN

    subgraph W0[Worker 0]
        direction TB
        EL0[Event Loop] --> CE0[Core Engine]
        CE0 --> UL0[User Library]
        CE0 --> BP0[Buffer Pool]
        CE0 --> FC0[File Cache]
        CE0 --> LG0[Logger]
        CE0 --> CT0[Crash Tracer]
    end

    subgraph W1[Worker 1]
        direction TB
        EL1[Event Loop] --> CE1[Core Engine]
        CE1 --> UL1[User Library]
    end

    subgraph WN[Worker N]
        direction TB
        ELN[Event Loop] --> CEN[Core Engine]
        CEN --> ULN[User Library]
    end

    W0 -->|writes slot 0| SM
    W1 -->|writes slot 1| SM
    WN -->|writes slot N| SM
```

---

## Key points

- The master process never handles requests. It only initializes shared state and spawns workers.
- Each worker is fully independent. A crash in one worker does not affect others.
- User code runs inside a loaded shared library. It communicates with the engine through ABI-stable function pointer structs.
- All memory for connections and buffers comes from a per-worker pool, not the system allocator.
- Metrics are written to a shared region. Any worker can aggregate across all slots at any time.
- Per-worker subsystems (buffer pool, file cache, logger, crash tracer) are initialized after spawn and are never shared.