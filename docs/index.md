---
hide:
  - navigation
  - toc
---

<div class="wfx-hero">
<div class="wfx-badge">v0.x - active development, Linux only</div>
<h1 class="wfx-title">WFX</h1>
<p class="wfx-sub">An explicit, low-level C++ web engine for people who want control and performance without behavior hidden behind abstractions.</p>
<div class="wfx-buttons">
<a href="getting_started/installation/" class="md-button md-button--primary">Get started</a>
<a href="api_reference/overview/" class="md-button">API reference</a>
</div>
</div>

!!! warning "Active development"
    Documentation and APIs change frequently. Do not treat anything here as stable until stated otherwise. See [what's missing](#whats-missing) below before you commit to anything.

<p class="wfx-section-label">What it includes</p>

<div class="wfx-grid">
<div class="wfx-card">
<p class="wfx-card-title">HTTP(S) engine</p>
<p class="wfx-card-body">HTTP/1.1 server with TLS and a deterministic request-response lifecycle.</p>
</div>
<div class="wfx-card">
<p class="wfx-card-title">Multi-process workers</p>
<p class="wfx-card-body">A master process plus independent worker processes. A crash in one worker doesn't take the others down.</p>
</div>
<div class="wfx-card">
<p class="wfx-card-title">Outbound Endpoint client</p>
<p class="wfx-card-body">Pooled outbound connections with DNS refresh, retry, coalescing, multiplexing, connection pinning, and chunked streaming. Ships with HTTP and SMTP clients; other protocols are a serialize/parse pair away.</p>
</div>
<div class="wfx-card">
<p class="wfx-card-title">Middleware</p>
<p class="wfx-card-body">Global and per-route middleware, including async middleware, with strict ordered execution.</p>
</div>
<div class="wfx-card">
<p class="wfx-card-title">Async model</p>
<p class="wfx-card-body">C++20 coroutines with explicit suspension and resumption. Async, but still deterministic.</p>
</div>
<div class="wfx-card">
<p class="wfx-card-title">SSR templates (WTX)</p>
<p class="wfx-card-body">Templates compile to generated C++ and link into the engine: variables, conditionals, loops, includes, inheritance.</p>
</div>
<div class="wfx-card">
<p class="wfx-card-title">Streaming responses</p>
<p class="wfx-card-body">Outbound streaming with backend-controlled buffer sizing.</p>
</div>
<div class="wfx-card">
<p class="wfx-card-title">Security controls</p>
<p class="wfx-card-body">Header/body/idle timeouts and connection/request rate limiting.</p>
</div>
<div class="wfx-card">
<p class="wfx-card-title">Form handling</p>
<p class="wfx-card-body">Built-in parsing, validation, sanitization, and field rendering.</p>
</div>
<div class="wfx-card">
<p class="wfx-card-title">JSON</p>
<p class="wfx-card-body">A streaming writer for output and a DOM-style parser for input.</p>
</div>
<div class="wfx-card">
<p class="wfx-card-title">Metrics and logging</p>
<p class="wfx-card-body">Structured logging plus live network, process, and worker-health metrics per worker.</p>
</div>
<div class="wfx-card">
<p class="wfx-card-title">TOML configuration</p>
<p class="wfx-card-body">File-based engine configuration for timeouts, TLS, workers, and more.</p>
</div>
</div>

<p class="wfx-sub" style="max-width: 560px; margin-top: 1.25rem !important;">...and plenty more. See the <a href="api_reference/overview/">API reference</a> for the full surface.</p>

<p class="wfx-section-label">How it is designed</p>

<div class="wfx-principles">
<div class="wfx-principle">
<p class="wfx-principle-title">Engine-as-source, not a black box</p>
<p class="wfx-principle-body">WFX headers and engine code are part of your build. There is no opaque runtime sitting between your code and what actually runs.</p>
</div>
<div class="wfx-principle">
<p class="wfx-principle-title">Minimal magic by default</p>
<p class="wfx-principle-body">Behavior is explicit. Helper macros and coroutine utilities exist, but nothing happens unless you reach for it.</p>
</div>
<div class="wfx-principle">
<p class="wfx-principle-title">Deterministic execution</p>
<p class="wfx-principle-body">Request handling, middleware order, and ownership semantics are predictable. You can reason about what runs, when, and in what order.</p>
</div>
<div class="wfx-principle">
<p class="wfx-principle-title">Clear separation of responsibilities</p>
<p class="wfx-principle-body">The engine, framework features, and your code are distinct layers even though they compile together.</p>
</div>
</div>

<p class="wfx-section-label" id="whats-missing">What's missing</p>

<div class="wfx-fit-col" style="margin-bottom: 2.5rem;">
<ul class="wfx-fit-list">
<li><strong>Linux only.</strong> No Windows or macOS support yet.</li>
<li><strong>HTTP/1.1 only.</strong> The server and the outbound client both speak it, and nothing newer.</li>
<li><strong>An engine, not a batteries-included framework.</strong> The lower-level pieces are here and the primitives to build on them are deliberate; the conveniences layered on top of them are largely not, so expect to write some of that yourself.</li>
<li><strong>No stability guarantees.</strong> Both the API and ABI can still change before a first stable release.</li>
</ul>
</div>

<p class="wfx-section-label">Who this is for</p>

<div class="wfx-fit">
<div class="wfx-fit-col">
<p class="wfx-fit-header wfx-fit-header--good">Good fit</p>
<ul class="wfx-fit-list">
<li>Comfortable with C++ and ownership semantics</li>
<li>Care about performance and memory behavior</li>
<li>Want full visibility into what the engine does</li>
<li>Prefer reading code over reading magic</li>
<li>Fine building on Linux, and fine rebuilding when WFX changes underneath you</li>
</ul>
</div>
<div class="wfx-fit-col">
<p class="wfx-fit-header wfx-fit-header--bad">Not for you</p>
<ul class="wfx-fit-list">
<li>Need hot reload or scripting-language iteration today</li>
<li>Need Windows or macOS support today</li>
<li>Want to serve static files without server logic</li>
<li>Uncomfortable with pointers or ownership</li>
<li>Hate C++</li>
</ul>
</div>
</div>

<p class="wfx-section-label">A note from the developer</p>

<div class="wfx-devnote">
<p>If you are considering using WFX, thank you genuinely.</p>
<p>That said, i would not recommend it for production software right now. There will be breaking changes, there will be bugs, and things will move fast. If you want to contribute or experiment, go ahead. Just do not build anything you cannot afford to break yet.</p>
<p>Other than that, best of luck. You will need it :)</p>
</div>