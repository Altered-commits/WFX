# Architecture

This page describes how WFX initializes and runs at a high level. It does not cover internal implementation details.

---

## Startup flow

```mermaid
flowchart TD
    A([wfx run project]) --> B[SetMinLevel INFO]
    B --> C[CheckAlreadyRunning]

    C --> D{PID file result}
    D -- Not found --> G
    D -- IO error --> E([Fatal: cannot read PID file])
    D -- Corrupted --> F[Delete corrupted PID file]
    F --> G
    D -- OK and alive --> H([Fatal: already running])
    D -- OK but dead --> I[Delete stale PID file]
    I --> G

    G[Validate project directory exists] --> K[Create default_logs and crash_logs dir if missing]
    K --> N[Load the selected config/wfx toml and .env\nrequire owner uid and perms 600]

    N --> O[Install SIGINT and SIGTERM handlers\nroutes to HandleMasterSignal]
    O --> P[Install SIGCHLD handler\nwakes master from nanosleep, does nothing else]
    P --> Q[GenerateSSLKey]
    Q --> R[MetricTracer Create\nallocate shared mmap for N workers]

    R --> PA{Prebuilt boot requested?}
    PA -- Yes --> PB{build/user_entry.so exists?}
    PB -- No --> PC([Fatal: prebuilt library missing])
    PB -- Yes --> PD[LoadTemplatesFromCache\nrestore map from the shipped cache]
    PD --> X

    PA -- No --> S[HandleBuildDirectory]
    S --> T[PreCompileTemplates]
    T --> U{Compile succeeded\nAND has a dynamic template?}
    U -- No: either failed\nor purely static --> V[Compile source only\ntarget: user_entry]
    U -- Yes --> W[Compile source and templates\nboth cmake targets]
    V --> X[LoadDynamicTemplatesFromLib]
    W --> X

    X --> Y{Daemon mode flag set?}
    Y -- Yes --> Z[fork]
    Z --> ZA[Parent logs daemon pid\nexits immediately]
    Z --> ZB[Child calls setsid\nbecomes session leader]
    ZB --> ZC[Redirect stdin stdout stderr\nto dev/null]
    ZC --> ZD
    Y -- No --> ZD[Write PID file via DaemonRegistry]

    ZD --> ZH[Apply configured logger settings\nmin level, stdout, colors, timestamps]
    ZH --> ZI[Open master.log if file logging enabled]

    ZI --> ZK[workerPids resize to N slots\nSpawnWorker 0 to N-1]

    subgraph SW[SpawnWorker per slot]
        direction TB
        S1[fork]
        S1 --> S2{pid == 0?\nchild}
        S2 -- Yes --> S3[setpgid\nfirst worker becomes group leader\nrest join group]
        S3 --> S4[MetricTracer InitWorker slot]
        S4 --> S5[CrashTracer SetWorkerName\nCrashTracer Install]
        S5 --> S6[Open worker-N.log if file logging enabled]
        S6 --> S7[Init BufferPool and FileCache]
        S7 --> S9[Install SIGTERM handler\nSIGINT SIGPIPE SIGHUP ignored]
        S9 --> S10[PinWorkerToCPU if flag set]
        S10 --> S8[Construct CoreEngine:\ncreate connection backend init API tables\ndlopen user .so invoke RegisterMasterAPI\nload middleware]
        S8 --> S11([engine.Listen\nenter event loop, blocks here])
        S2 -- No: master --> S12[setpgid on child\nstore pid in workerPids slot\nupdate slot self.pid and startedAt]
    end

    ZK --> SW
    ZK --> ML

    subgraph ML[Master wait loop]
        direction TB
        M1[nanosleep masterPollInterval\nwakes early on any signal, incl. SIGCHLD]
        M1 --> M2[ReapDeadWorkers\nwaitpid WNOHANG loop\nincrement crashes\ncompute backoff\nmark SLOT_PENDING or SLOT_DEAD]
        M2 --> M3[RevivePendingWorkers\ncheck nextRetryAt per slot\nSpawnWorker if window expired\nincrement restarts and backoffAttempts]
        M3 --> M4[PollWorkerMetrics\nopen /proc/pid/status per live slot\nparse VmRSS and VmSize\nwrite rssBytes and vmBytes into slot]
        M4 --> M1
    end

    ML --> SD{shouldStop set?}
    SD -- Yes --> SF[Log: waiting for workers to shutdown]

    SF --> SG[All workers together: poll waitpid WNOHANG every 100ms\nup to workerShutdownTimeout, shared across every worker]
    SG --> SH{All exited in time?}
    SH -- No --> SI[SIGKILL whatever's left\nwaitpid to reap each zombie]
    SH -- Yes --> SJ
    SI --> SJ[MetricTracer Destroy\nmunmap shared region]
    SJ --> SK[DaemonRegistry Delete PID file]
    SK --> SL([Exit 0])
```

`shouldStop` only ever flips to true because `HandleMasterSignal` ran. That
handler fires asynchronously the instant `SIGINT`/`SIGTERM` arrives, and does
two things at once: it sets `shouldStop`, and it immediately broadcasts
`SIGTERM` to the entire worker process group. That broadcast, not any step in
the diagram above, is what actually asks workers to stop, the wait loop below
it only polls for them to have exited.

The wait loop polls every worker in the same `workerShutdownTimeout` window
at once, not one worker's full timeout after another. A worker that ignores
`SIGTERM`, or one that was spawned by a revival right before the signal
arrived and so never received it, still gets caught by this same shared
deadline, SIGKILLed, and reaped before the master proceeds. No worker
outlives its master. This also keeps the master's own worst-case shutdown
time bounded by `workerShutdownTimeout` regardless of worker count, which
matters because `wfx control stop` gives up and force-kills the master
itself if it waits past that same budget, see [Engine Commands](../core_concepts/engine_commands.md#wfx-control).

---

## Request flow

### Parsing and routing

```mermaid
flowchart TD
    A([onReceive fires]) --> B[HandleRequest\nallocate the per-connection response object\non first use, reused across requests after]

    B --> C{HttpParser::Parse result}

    C -- Incomplete headers --> D[Mark connection alive\nRefresh header timeout\nResume receive]
    C -- Incomplete body --> E[Mark connection alive\nRefresh body timeout\nResume receive]
    C -- Expect 100 --> F[Mark connection alive\nRefresh body timeout\nWrite 100 Continue]
    C -- Expect 417 --> G[Mark connection close\nWrite 417 Expectation Failed]
    C -- Parse error --> H[Mark connection close\nWrite 400 Bad Request]
    C -- Streaming body or unsupported --> I[Mark connection close\nWrite 501 Not Implemented]

    C -- Success --> J[Increment request counter\nreset per-request async tracking]
    J --> J1[Parse Connection header]
    J1 --> J2{Header combination valid?}
    J2 -- No --> J3[Mark connection close\nWrite 400 Bad Request]

    J2 -- Yes --> J4[Decide keep-alive vs close:\nHTTP/1.0 defaults to close\nHTTP/1.1 defaults to keep-alive\nan explicit header overrides either]
    J4 --> J5{Write buffer ready?}
    J5 -- Init failed --> J6[Mark connection close\nWrite 500 Internal Server Error]
    J5 -- OK --> J7[Reset response object\nwire in write buffer HTTP version close flag]

    J7 --> K{Path starts with /public/?}
    K -- Yes --> L[Resolve file under the public dir\nqueue it as the response\nskip routing and middleware]
    K -- No --> M[Match route in trie]

    M -- No match --> N[Set 404 response]

    L --> Finish[[FinishRequest, see: Sending the response]]
    N --> Finish
    J3 --> Finish
    J6 --> Finish

    M -- Match --> P[[HandleSuccess, see: Executing the route]]
```

A header combination like `close` and `keep-alive` together is invalid and
takes the same 400 path as any other malformed header.

### Executing the route

```mermaid
flowchart TD
    P([HandleSuccess]) --> P1{Resuming after this request's\nasync route handler already finished?}
    P1 -- Yes --> Finish[[FinishRequest, see: Sending the response]]
    P1 -- No: still at middleware stage --> Q[ExecuteMiddleware\nglobal stack first\nthen per-route stack]

    Q --> R{Middleware result}
    R -- Broke the chain: already sent a response --> Finish
    R -- Failed synchronously --> R1[Mark connection close\nSet 500 response]
    R1 --> Finish
    R -- Suspended: async middleware --> R2[[FinishRequest, return now\nresumes later via OnCoroutineComplete]]
    R -- All passed --> S[Mark stage as past-middleware]

    S --> S1{Route handler kind}
    S1 -- Sync --> W[Execute handler directly]
    W --> Finish

    S1 -- Async --> X[Set connection context for the ABI boundary\ninvoke the async handler, clear the context pointer]
    X --> X1[[FinishRequest, return either way\nhandler may already have completed\nsynchronously by this point, or not]]

    U([OnCoroutineComplete fires]) --> U1{Async result status}
    U1 -- IO failure --> U2[Mark connection close\nSet 500 response]
    U2 --> Direct[[HandleResponse directly, see: Sending the response]]
    U1 -- Completed AND still at middleware stage --> U3[Store the resulting middleware action]
    U3 --> P
    U1 -- Completed AND past middleware stage --> Direct
```

`OnCoroutineComplete`'s two exits are worth noticing: one goes back into
`HandleSuccess` (an async middleware resuming), the other skips straight to
`HandleResponse`, not through `FinishRequest` again, since `FinishRequest`
already ran once, synchronously, right when the handler was first dispatched.

### Sending the response

```mermaid
flowchart TD
    FR[FinishRequest:\nreset parse state to idle\nrefresh idle timeout] --> AC[HandleResponse]

    AC --> AC1{Response already committed?}
    AC1 -- No --> AC2[Commit response]
    AC1 -- Yes --> AC3
    AC2 --> AC3[Update response-code metrics]

    AC3 --> AD{Response type}

    AD -- File --> AE[Hand off to backend\nfile send primitive]
    AD -- Stream --> AF[Hand off to backend\nstreaming generator]
    AD -- Already-serialized buffer --> AG[Hand off to backend\nwrite the buffer as-is]

    AE --> AH{Connection state}
    AF --> AH
    AG --> AH

    AH -- Keep-alive --> AI[Clear per-request context\nResume receive]
    AH -- Close --> AJ([Backend closes and releases connection])
```

Every path in the two diagrams above that reaches `FinishRequest` lands at
the top of this one. The one exception is `OnCoroutineComplete` calling
`HandleResponse` directly, which just enters this diagram one node lower,
skipping `FinishRequest` for the reason noted above.

---

## Template Flow

### Template compilation pipeline

```mermaid
flowchart TD
    A([PreCompileTemplates called in master]) --> B[Load template cache from disk]
    B --> C{Cache result}
    C -- Corrupted or missing --> D[Clear cache\nmark for resave]
    C -- Loaded --> E[Ensure output dirs exist]
    D --> E

    E --> F[Walk template directory recursively]
    F --> G{File is .html or .htm?}
    G -- No --> F
    G -- Yes --> H[Compute relative path and output path]

    H --> I[Get disk file stats]
    I --> J{Cache entry matches\nmodified time?}
    J -- Yes --> K[Load from cache\nadd to templates map]
    K --> F
    J -- No --> L[Mark for cache update]

    L --> M{Output subdirectory\nready?}
    M -- Failed to create --> M1[Log error, skip this file\nnot counted as a compile error]
    M1 --> F
    M -- Yes --> N{Open input file}
    N -- Failed --> N1[Increment error count]
    N1 --> F
    N -- OK but empty --> N2[Skip file silently]
    N2 --> F
    N -- OK --> O{Starts with partial tag?}
    O -- Yes --> P[Skip file]
    P --> F
    O -- No --> Q{Open output file}
    Q -- Failed --> S
    Q -- OK --> Q1[Invoke CompileTemplate]

    Q1 --> R{Compile result}
    R -- Failure --> S[Increment error count]
    S --> F
    R -- Static --> T[Add to templates map as STATIC]
    T --> U[Update cache entry]
    U --> F
    R -- Dynamic --> V[Generate C identifier from path\ncompute .cpp output path]
    V --> W[Invoke GenerateCxxFromTemplate]
    W --> X{Success?}
    X -- No --> S
    X -- Yes --> Y[Add to templates map as DYNAMIC]
    Y --> U

    F --> Z{Any errors?}
    Z -- Yes --> ZA([Return failure])
    Z -- No --> ZB{Cache needs resave?}
    ZB -- Yes --> ZC[Save cache to disk]
    ZC --> ZD([Return success with hasDynamic flag])
    ZB -- No --> ZD
```

### Restoring templates without compiling

`LoadTemplatesFromCache` is the `--use-prebuilt` counterpart to the pipeline above. The templates map
is what `LoadDynamicTemplatesFromLib` and `GetTemplate` resolve against, so it still has to be
populated even when nothing is being built.

```mermaid
flowchart TD
    A([LoadTemplatesFromCache called in master]) --> B[Load template cache from disk]
    B --> C{Cache loaded?}
    C -- No --> D([Warn, register no templates, return])
    C -- Yes --> E[Iterate cache entries]

    E --> F{Entry payload valid\nAND path under template dir?}
    F -- No --> G[Warn, skip entry]
    G --> E
    F -- Yes --> H[Pop type and size\nderive relative and output path]
    H --> I[Add to templates map]
    I --> E
```

The cache file is the only input, so the template directory is left untouched and can be absent
entirely. Timestamps play no part on this path.

### Compilation loop

#### Reading each frame

```mermaid
flowchart TD
    A([CompileTemplate called]) --> B[Init CompilationContext\npush main file onto frame stack]

    B --> C{Frame stack empty?}
    C -- Yes --> D([Return STATIC or DYNAMIC\nbased on foundDynamicTag])

    C -- No --> C1{Buffer left over from before\nan include or extends jump?}
    C1 -- Yes --> G1[Resume from frame.readOffset\nin the already-loaded buffer]
    C1 -- No --> E[Read next chunk from top frame]

    E --> F{Read result}
    F -- Error --> GFAIL([Return FAILURE])

    F -- EOF --> H[Flush carry if any\nFlush write buffer to disk\nPop current frame]
    H --> I{Popped frame had\nan extends tag pending?}
    I -- Yes --> J[Push parent file onto stack\nclear currentExtendsName]
    J --> C
    I -- No --> C

    F -- Data --> G2[On this frame's first chunk only:\nskip a leading partial-tag marker]

    G1 --> Chunk[[Process this chunk, see below]]
    G2 --> Chunk
```

#### Processing one chunk

```mermaid
flowchart TD
    L{Pending carry?}

    L -- Yes --> M{Carry is single brace\nnext char is not percent?}
    M -- Yes --> N[Write brace as literal\nclear carry]
    N --> O[Main chunk loop]
    M -- No --> P{Carry ends with percent\nnext char is closing brace?}
    P -- Yes --> Q[Append closing brace\ncomplete tag from carry]
    Q --> ProcessTag
    P -- No --> R[Find tag terminator in current chunk\nappend to carry]
    R --> ProcessTag

    L -- No --> O

    O --> S{Tag start found?}
    S -- No --> T[Write remaining as literal\ncheck for trailing brace\nset carry if needed]
    T --> Back[[Back to reading each frame, see above]]

    S -- Yes --> U[Write literal before tag]
    U --> V{Tag terminator in same chunk?}
    V -- No --> W[Assign remainder to carry]
    W --> Back
    V -- Yes --> X[Extract tag view]
    X --> ProcessTag

    ProcessTag --> Y{Tag result}
    Y -- FAILURE --> Fail([Return FAILURE])
    Y -- SUCCESS --> Z[Clear carry\nadvance read offset]
    Z --> O
    Y -- CONTROL_TO_ANOTHER_FILE --> AA[Push included or extends file\nleave read offset as-is for later]
    AA --> Back
    Y -- PASSTHROUGH_DYNAMIC --> AB[Set foundDynamicTag true\nwrite tag as-is to output]
    AB --> O
```

`Pending Carry` is reached from either path in the diagram above: resuming a buffer left
over from an include or extends jump, or a freshly read chunk (after skipping
a leading partial-tag marker on a frame's very first chunk).

### Transpilation loop

This stage works on a single flat, already-fully-resolved HTML file. Unlike the
compilation loop above, there is no frame stack here, `{% include %}`,
`{% extends %}`, and `{% block %}` are already fully resolved by the earlier
static compile pass, this stage only ever sees `var`, `if`, `elif`, `else`,
`endif`, `for`, and `endfor`.

`GenerateCxxFromTemplate` is a thin wrapper around two functions: `GenerateIRFromTemplate` builds an intermediate representation from the template, then `GenerateCxxFromIR` turns that IR into a generated `.cpp` file.

```mermaid
flowchart TD
    A([GenerateCxxFromTemplate]) --> B[Open file, init context]
    B --> C[GenerateIRFromTemplate]
    C --> D{Succeeded?}
    D -- No --> DFAIL([Return false])
    D -- Yes --> E[GenerateCxxFromIR]
    E --> EEND([Return its result])
```

#### Building the IR: GenerateIRFromTemplate

Reads the file in chunks, same carry-over idea as the compilation loop for a
tag split across a chunk boundary, but with no `MAX_TAG_LENGTH` cap here (that
only exists in the earlier static pass), and no file stack, since there is
nothing left to include or extend. Tag dispatch itself is broken out below.

```mermaid
flowchart TD
    F1[Read next chunk] --> F2{Read result}
    F2 -- Error --> GFAIL1([Fail: read error])
    F2 -- EOF --> F3{Carry left over?}
    F3 -- Yes --> GFAIL2([Fail: incomplete tag at EOF])
    F3 -- No --> F4[Exit the read loop]

    F2 -- Data --> G{Carry pending?}
    G -- Yes --> H{Carry is lone brace,\nnext char not percent?}
    H -- Yes --> I[Literal brace, clear carry]
    I --> O
    H -- No --> J{Carry ends with percent,\nnext char is brace?}
    J -- Yes --> K[Tag complete from carry]
    K --> Dispatch[[ProcessTagIR, see table below]]
    J -- No --> L[Look for tag terminator here,\nnot found is a hard failure]
    L --> Dispatch

    G -- No --> O[Main read loop]
    O --> M{Tag start found?}
    M -- No --> N[Accumulate as pending literal]
    N --> F1
    M -- Yes --> P[FinalizeLiteral:\nflush pending literal into one IR entry]
    P --> Q{Tag terminator in same chunk?}
    Q -- No --> R[Remainder becomes carry]
    R --> F1
    Q -- Yes --> S[Extract tag view]
    S --> Dispatch

    Dispatch --> U{Tag result}
    U -- FAILURE --> GFAIL3([Fail])
    U -- SUCCESS --> V[Clear carry, advance offset]
    V --> O

    F4 --> W[FinalizeLiteral once more\nfor a trailing literal]
    W --> X{Patch stack empty?}
    X -- No --> XFAIL([Fail: unmatched if or for])
    X -- Yes --> Y{Separate scan:\nevery op patched?}
    Y -- No --> YFAIL([Fail: internal error,\nunpatched jump target])
    Y -- Yes --> DOK([IR complete])
```

#### Tag dispatch: ProcessTagIR

| Tag | What it does |
|-----|--------------|
| `var` | `ParseExpr`, pool the RPN bytecode, emit `VAR` (payload is the expr index, never needs patching) |
| `if` | `ParseExpr`, push a new patch frame holding just this `IF`'s own index, emit `IF` |
| `elif` | Requires a non-empty stack and frame. Emit `JUMP`, patch only the `IF`/`ELIF` entries currently in the frame to jump here (queued `JUMP`s are left alone, they wait for `endif`). `ParseExpr`, emit `ELIF`, push its index too |
| `else` | Same guard and patch rule as `elif`. Emit `JUMP`, then a bare `ELSE` marker with no payload |
| `endif` | Requires a non-empty stack. Patch `IF`/`ELIF` entries' condition target and `JUMP` entries' plain target, both to this end state. Pop the frame, emit `ENDIF` |
| `for` | Hand-parsed as `identifier in <expr>`. Push a new patch frame holding only this `FOR`'s own index, emit `FOR` |
| `endfor` | Requires a non-empty stack. A `FOR`'s frame only ever holds its own single index. Patch that jump target to this end state, pop the frame, emit `ENDFOR` carrying a copy of the now-patched value |
| anything else | `include`/`extends`/`block` never reach this stage at all, so this is always an unknown tag, fails |

#### Generating the output: GenerateCxxFromIR

The generated `GetState(state, ctx)` method is a `while(true)` wrapped around
a `switch(state)`. Only `LITERAL`, `VAR`, and the trailing default case
actually `return`, every other op sets `state` and `continue`s (or, for
`ELSE`/`ENDIF`, is a genuinely empty case relying on plain C++ fallthrough),
so several IR states can be walked through inside one call without ever
returning to the caller.

```mermaid
flowchart TD
    AA[Open output file, write header,\ngenerator type, GetStateCount] --> AB[Begin GetState:\nwhile true around switch on state]
    AB --> AC{Per IR op}

    AC -- LITERAL --> AD[return FileChunk]
    AC -- VAR --> AE[return VariableChunk\nfrom GenerateCxxFromRPN]
    AC -- IF / ELIF --> AF[False: jump to false-target, continue\nTrue: fall through]
    AC -- ELSE / ENDIF --> AG[Empty case, plain fallthrough]
    AC -- JUMP --> AH[Unconditional: jump to target, continue]
    AC -- FOR --> AI[Empty/invalid: jump past matching ENDFOR\nOtherwise: set loop var to first item,\nstash resume state, fall through]
    AC -- ENDFOR --> AJ[Re-evaluate the iterable fresh each time\nMore items: advance, jump to right after FOR\nDone: erase loop var, fall through]
    AC -- unrecognized, defensive --> AK[Log error, return false]

    AD --> AL[Next op] --> AC
    AE --> AL
    AF --> AL
    AG --> AL
    AH --> AL
    AI --> AL
    AJ --> AL

    AC --> AM[Final case: return, close out\nswitch, while, method, and type]
    AM --> AN[Write extern C factory function]
    AN --> AO([Flush, return success])
```

The footer and factory-function writes are the only two writes in this
function that aren't error-checked, everything before them returns `false` on
a write failure.

---

## Key points

- The master process never handles requests. It only initializes shared state and spawns workers.
- Each worker is fully independent. A crash in one worker does not affect others.
- User code runs inside a loaded shared library. It communicates with the engine through ABI-stable function pointer structs.
- All memory for connections and buffers comes from a per-worker pool, not the system allocator.
- Metrics are written to a shared region. Any worker can aggregate across all slots at any time.
- Per-worker subsystems (buffer pool, file cache, crash tracer) are initialized after spawn and are never shared.