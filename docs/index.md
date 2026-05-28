---
hide:
  - navigation
  - toc
---

<div class="wfx-hero">
<div class="wfx-badge">v0.x - active development</div>
<h1 class="wfx-title">WFX</h1>
<p class="wfx-sub">An explicit, low-level C++ web engine for people who want control and performance without behavior being hidden behind abstractions.</p>
<div class="wfx-buttons">
<a href="getting_started/installation/" class="md-button md-button--primary">Get started</a>
<a href="api_reference/overview/" class="md-button">API reference</a>
</div>
</div>

!!! warning "Active development"
    Documentation and APIs change frequently. Do not treat anything here as stable until stated otherwise.

<p class="wfx-section-label">What it includes</p>

<div class="wfx-grid">
<div class="wfx-card">
<p class="wfx-card-title">HTTP(S) engine</p>
<p class="wfx-card-body">Full server with TLS support and a deterministic request-response lifecycle.</p>
</div>
<div class="wfx-card">
<p class="wfx-card-title">Middleware</p>
<p class="wfx-card-body">Global and per-route middleware with strict ordered execution.</p>
</div>
<div class="wfx-card">
<p class="wfx-card-title">Streaming</p>
<p class="wfx-card-body">Outbound streaming responses with backend-controlled buffer sizing.</p>
</div>
<div class="wfx-card">
<p class="wfx-card-title">SSR engine</p>
<p class="wfx-card-body">Server-side rendering with a full templating system.</p>
</div>
<div class="wfx-card">
<p class="wfx-card-title">Async model</p>
<p class="wfx-card-body">Custom coroutine execution with explicit suspension and resumption.</p>
</div>
<div class="wfx-card">
<p class="wfx-card-title">Security primitives</p>
<p class="wfx-card-body">Connection timeouts, rate limiting, and request-level controls.</p>
</div>
<div class="wfx-card">
<p class="wfx-card-title">Form handling</p>
<p class="wfx-card-body">Built-in parsing, validation, sanitization, and field rendering.</p>
</div>
<div class="wfx-card">
<p class="wfx-card-title">TOML configuration</p>
<p class="wfx-card-body">File-based engine configuration with full runtime control.</p>
</div>
<div class="wfx-card wfx-card--more">
<p class="wfx-card-title">And much more</p>
<p class="wfx-card-body">Explore the full API reference for everything else.</p>
</div>
</div>

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

<p class="wfx-section-label">Who this is for</p>

<div class="wfx-fit">
<div class="wfx-fit-col">
<p class="wfx-fit-header wfx-fit-header--good">Good fit</p>
<ul class="wfx-fit-list">
<li>Comfortable with C++ and ownership semantics</li>
<li>Care about performance and memory behavior</li>
<li>Want full visibility into what the engine does</li>
<li>Prefer reading code over reading magic</li>
</ul>
</div>
<div class="wfx-fit-col">
<p class="wfx-fit-header wfx-fit-header--bad">Not for you</p>
<ul class="wfx-fit-list">
<li>Need hot reload or scripting-language iteration</li>
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