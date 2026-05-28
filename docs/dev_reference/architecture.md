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
    K --> N[Load wfx.toml and .env\nrequire owner uid and perms 600]

    N --> O[Install SIGINT and SIGTERM handlers]
    O --> P[Install SIGCHLD handler\nwakes master from nanosleep]
    P --> Q[GenerateSSLKey]
    Q --> R[MetricTracer Create\nallocate shared mmap for N workers]

    R --> S[HandleBuildDirectory]
    S --> T[PreCompileTemplates]
    T --> U{hasDynamic?}
    U -- No --> V[Compile source only]
    U -- Yes --> W[Compile source and templates]
    V --> X[LoadDynamicTemplatesFromLib]
    W --> X

    X --> Y{Daemon mode flag set?}
    Y -- Yes --> Z[fork]
    Z --> ZA[Parent logs daemon pid\nexits immediately]
    Z --> ZB[Child calls setsid\nbecomes session leader]
    ZB --> ZC[Redirect stdin stdout stderr\nto dev/null]
    ZC --> ZD
    Y -- No --> ZD[Write PID file via DaemonRegistry]

    ZD --> ZH[Configure logger settings]
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
        S7 --> S8[Construct CoreEngine\nload user library\nregister routes\nload middleware]
        S8 --> S9[Install SIGTERM SIGINT SIG_IGN\nSIGPIPE SIG_IGN SIGHUP SIG_IGN]
        S9 --> S10[PinWorkerToCPU if flag set]
        S10 --> S11([engine.Listen\nenter event loop])
        S2 -- No master --> S12[setpgid on child\nstore pid in workerPids slot\nupdate slot self.pid and startedAt]
    end

    ZK --> SW
    ZK --> ML

    subgraph ML[Master wait loop]
        direction TB
        M1[nanosleep masterPollInterval\nwakes early on SIGCHLD]
        M1 --> M2[ReapDeadWorkers\nwaitpid WNOHANG loop\nincrement crashes\ncompute backoff\nmark SLOT_PENDING or SLOT_DEAD]
        M2 --> M3[RevivePendingWorkers\ncheck nextRetryAt per slot\nSpawnWorker if window expired\nincrement restarts and backoffAttempts]
        M3 --> M4[PollWorkerMetrics\nopen /proc/pid/status per live slot\nparse VmRSS and VmSize\nwrite rssBytes and vmBytes into slot]
        M4 --> M1
    end

    ML --> SD{shouldStop set\nby SIGINT or SIGTERM}
    SD --> SE[Re-enable stdout\nSetMinLevel INFO]
    SE --> SF[For each live slot\nsend SIGTERM]
    SF --> SG[Poll waitpid WNOHANG every 100ms\nup to workerShutdownTimeout]
    SG --> SH{Exited in time?}
    SH -- No --> SI[Send SIGKILL\nwaitpid to reap zombie]
    SH -- Yes --> SJ
    SI --> SJ[MetricTracer Destroy\nmunmap shared region]
    SJ --> SK[DaemonRegistry Delete PID file]
    SK --> SL([Exit 0])
```

---

## Request flow

```mermaid
flowchart TD
    A([onReceive fires]) --> B[HandleRequest]

    B --> C{Parse result}

    C -- Incomplete headers --> D[Refresh header timeout\nResume receive]
    C -- Incomplete body --> E[Refresh body timeout\nResume receive]
    C -- Expect 100 --> F[Send 100 Continue]
    C -- Expect 417 --> G[Send 417]
    C -- Parse error --> H[Send 400]
    C -- Unsupported --> I[Send 501]

    C -- Success --> J[Increment request counter]

    J --> K{/public/ path?}
    K -- Yes --> L[Resolve and queue file\nSkip routing and middleware]
    K -- No --> M[Match route in trie]

    M -- No match --> N[Set 404 response]
    L --> O[FinishRequest]
    N --> O

    M -- Match --> P[HandleSuccess]

    P --> Q[ExecuteMiddleware\nglobal stack first\nthen per-route stack]

    Q --> R{Middleware result}

    R -- Sync break --> O
    R -- All passed --> S{Handler type}

    R -- Async suspended --> T[Store stack index\nSuspend execution]
    T --> U([OnCoroutineComplete fires])
    U --> V{Middleware action}
    V -- Continue or Skip --> Q
    V -- Break --> O

    S -- Sync --> W[Execute handler directly]
    W --> O

    S -- Async --> X[Inject connection context\nCall async handler\nClear context pointer]
    X --> Y{Completed synchronously?}
    Y -- Yes --> O
    Y -- No --> Z[Suspend execution]
    Z --> U

    U --> AA{Coroutine result}
    AA -- IO failure --> AB[Set 500 response]
    AB --> O
    AA -- Route completed --> O

    O[FinishRequest\nRefresh idle timeout] --> AC[HandleResponse\nUpdate response metrics]

    AC --> AD{Response type}

    AD -- File --> AE[Hand off to backend\nfile send primitive]
    AD -- Stream --> AF[Hand off to backend\nstreaming generator]
    AD -- Buffer --> AG[Hand off to backend\nwrite serialized buffer]

    AE --> AH{Connection state}
    AF --> AH
    AG --> AH

    AH -- Keep-alive --> AI[Clear context\nResume receive]
    AH -- Close --> AJ([Backend closes and releases connection])
```

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

    L --> M[Ensure output subdirectory exists]
    M --> N[Open input file]
    N --> O{Starts with partial tag?}
    O -- Yes --> P[Skip file]
    P --> F
    O -- No --> Q[Open output file\ncall CompileTemplate]

    Q --> R{Compile result}
    R -- Failure --> S[Increment error count]
    S --> F
    R -- Static --> T[Add to templates map as STATIC]
    T --> U[Update cache entry]
    U --> F
    R -- Dynamic --> V[Generate C identifier from path\ncompute .cpp output path]
    V --> W[Call GenerateCxxFromTemplate]
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

### Compilation loop

```mermaid
flowchart TD
    A([CompileTemplate called]) --> B[Init CompilationContext\npush main file onto frame stack]

    B --> C{Frame stack empty?}
    C -- Yes --> D([Return STATIC or DYNAMIC\nbased on foundDynamicTag])

    C -- No --> E[Read next chunk from top frame]
    E --> F{Read result}
    F -- Error --> G([Return FAILURE])

    F -- EOF --> H[Flush carry if any\nFlush write buffer to disk]
    H --> I{extends tag pending?}
    I -- Yes --> J[Push parent file onto stack\nclear currentExtendsName]
    J --> C
    I -- No --> K[Pop frame from stack]
    K --> C

    F -- Data --> L{Pending carry?}

    L -- Yes --> M{Carry is single brace\nnext char is not percent?}
    M -- Yes --> N[Write brace as literal\nclear carry]
    N --> O[Default chunk loop]
    M -- No --> P{Carry ends with percent\nnext char is closing brace?}
    P -- Yes --> Q[Append closing brace\ncomplete tag from carry]
    Q --> ProcessTag
    P -- No --> R[Find tag end in current chunk\nappend to carry]
    R --> ProcessTag

    L -- No --> O

    O --> S{Tag start found?}
    S -- No --> T[Write remaining as literal\ncheck for trailing brace\nset carry if needed]
    T --> C

    S -- Yes --> U[Write literal before tag]
    U --> V{Tag end in same chunk?}
    V -- No --> W[Assign remainder to carry]
    W --> C
    V -- Yes --> X[Extract tag view]
    X --> ProcessTag

    ProcessTag --> Y{Tag result}
    Y -- FAILURE --> G
    Y -- SUCCESS --> Z[Clear carry\nadvance read offset]
    Z --> O
    Y -- CONTROL_TO_ANOTHER_FILE --> AA[Push included or extends file\nbreak to outer loop]
    AA --> C
    Y -- PASSTHROUGH_DYNAMIC --> AB[Set foundDynamicTag true\nwrite tag as-is to output]
    AB --> O
```

### Transpilation loop

```mermaid
flowchart TD
    A([GenerateCxxFromTemplate called]) --> B[Open static html file\ninit TranspilationContext]
    B --> C[Call GenerateIRFromTemplate]

    C --> D[Read next chunk from file]
    D --> E{Carry pending?}
    E -- Yes --> F{Carry is single brace\nnext char is not percent?}
    F -- Yes --> G[Write brace as literal\nclear carry\ngo to default loop]
    G --> O
    F -- No --> H{Carry ends with percent\nnext char is closing brace?}
    H -- Yes --> I[Complete tag from carry]
    I --> TagProc
    H -- No --> J[Find tag end in chunk\nappend to carry]
    J --> TagProc

    E -- No --> O[Default read loop]
    O --> K{Tag start found?}
    K -- No --> L[Accumulate bytes as literal range\ntrack offset and length\ncheck for trailing brace]
    L --> D
    K -- Yes --> M[Finalize pending literal op into IR]
    M --> N{Tag end in same chunk?}
    N -- No --> P[Assign remainder to carry]
    P --> D
    N -- Yes --> Q[Extract tag view]
    Q --> TagProc

    subgraph TagProc[ProcessTagIR]
        direction TB
        T1{Tag type}
        T1 -- var --> T2[ParseExpr\npool RPN bytecode\nemit VAR op]
        T1 -- if --> T3[ParseExpr\npush patch frame\nemit IF op]
        T1 -- elif --> T4[Emit JUMP op\npatch previous to here\nParseExpr\nemit ELIF op]
        T1 -- else --> T5[Emit JUMP op\npatch previous to here\nemit ELSE marker]
        T1 -- endif --> T6[Patch all pending jumps\npop patch frame\nemit ENDIF marker]
        T1 -- for --> T7[ParseExpr for iterable\nget loop var id\npush patch frame\nemit FOR op]
        T1 -- endfor --> T8[Patch FOR jump state\npop patch frame\nemit ENDFOR op]
    end

    TagProc --> R[Clear carry\nadvance read offset\ncontinue loop]
    R --> O

    D --> S{EOF?}
    S -- No --> D
    S -- Yes --> U[Finalize trailing literal if any]
    U --> V{Unmatched if or for blocks?}
    V -- Yes --> W([Return failure])
    V -- No --> X[IR complete]

    X --> AA[Open output .cpp file\nwrite header and using declarations]
    AA --> AB[Write generator class\nGetStateCount returns IR size]
    AB --> AC[Emit switch case per IR op]

    AC --> AD{Op type}
    AD -- LITERAL --> AE[Emit return with FileChunk\noffset and length]
    AD -- VAR --> AF[Emit return with VariableChunk\nfrom GenerateCxxFromRPN]
    AD -- IF or ELIF --> AG[Emit conditional check\nif false jump to patched state\nfallthrough otherwise]
    AD -- JUMP --> AH[Emit unconditional state jump]
    AD -- ELSE or ENDIF --> AI[Emit empty case\nwith fallthrough]
    AD -- FOR --> AJ[Emit array init\nset loop var and index in ctx\nfallthrough]
    AD -- ENDFOR --> AK[Emit index check\nloop back if more items\nelse erase loop vars\nfallthrough]

    AE --> AL[Next op]
    AF --> AL
    AG --> AL
    AH --> AL
    AI --> AL
    AJ --> AL
    AK --> AL
    AL --> AC

    AC --> AM[Emit default case\nwrite class closing brace\nwrite extern C factory function]
    AM --> AN[Flush write buffer to disk]
    AN --> AO([Return success])
```

---

## Key points

- The master process never handles requests. It only initializes shared state and spawns workers.
- Each worker is fully independent. A crash in one worker does not affect others.
- User code runs inside a loaded shared library. It communicates with the engine through ABI-stable function pointer structs.
- All memory for connections and buffers comes from a per-worker pool, not the system allocator.
- Metrics are written to a shared region. Any worker can aggregate across all slots at any time.
- Per-worker subsystems (buffer pool, file cache, crash tracer) are initialized after spawn and are never shared.