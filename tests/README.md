# WFX audits

Adversarial end-to-end suites. Each one boots a real WFX server against a real app in
`<suite>/app/`, drives it over raw sockets, and asserts on what actually came back.

These are not unit tests. They exist to find framing bugs, desync, memory corruption and
connection-pool contamination, so they send malformed bytes on purpose and read WFX's own logs and
sanitizer output to tell "the test failed" apart from "the worker died".

## Running

```bash
./run_audits.sh                                  # all suites, in order
./run_audits.sh --audit endpoint                 # one suite
./run_audits.sh --audit endpoint --phase push    # one phase, needs --audit
./run_audits.sh --audit endpoint --list-phases   # what phases exist
./run_audits.sh --audit tls -- --wfx-logs all    # anything after -- goes to the suite
```

`run_audits.sh` does **not** build WFX. Build first:

```bash
./scripts/install.sh --local-debug   # ASan + UBSan, what you want while fixing a bug
./scripts/install.sh --local         # optimised
```

Run the debug build unless you have a reason not to. Sanitizer reports are wired into the run
(see *Diagnosing a failure*), and without them a memory bug shows up as a bare `exit code 1`.

## Exit codes

Identical for every suite, so CI can key on them.

| Code | Meaning |
|------|---------|
| `0` | everything passed |
| `1` | a non-security check failed, or the worker was dead at the end |
| `2` | a **security** finding: a vector whose failure is a vulnerability |
| `3` | WFX never answered `/health`, so nothing was exercised |

## Layout

```
tests/
  common/            setup and plumbing, no test logic lives here
    term.py           colors, headers, CI annotations
    net.py            raw HTTP over TCP/TLS, deliberately not a real client
    logs.py           tailing WFX logs, sanitizer wiring
    server.py         the WFX process under test
    report.py         results, summary table, exit codes
    suite.py          config, lifecycle, run loop
  run_audits.sh       single entry point
  <name>_audit/
    <name>_audit.py   the suite
    upstream.py       mock backend, if it needs one
    app/              the WFX app under test (C++)
    README.md         what this suite covers and why
```

## Writing a suite

A suite is a `common.Suite` subclass. It declares its phases and, if it needs one, a mock. The
`common` owns argument parsing, boot, log following, progress output, worker-death detection,
teardown and the exit code, so a suite file contains only vectors and assertions.

```python
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import common
from common import net

def phase_framing(ctx):
    p = ctx.phase("framing")

    raw = net.send(ctx.cfg.host, ctx.cfg.port, net.request("GET", "/thing"))
    p.check("responds 200", net.status(raw) == 200, "got %r" % net.status(raw))
    p.secure("no body on 204", net.body(raw) == b"", "leaked %r" % net.body(raw))

class MyAudit(common.Suite):
    name = "my_audit"
    description = "what this proves"
    phases = {"framing": phase_framing}

if __name__ == "__main__":
    common.run(MyAudit)
```

### The rules

**Every phase is `def phase_<name>(ctx)`.** One signature, no exceptions. Anything a phase needs
hangs off `ctx`: `ctx.cfg`, `ctx.server`, and whatever `setup()` registered in `ctx.resources`
(reachable as `ctx.mock`). A phase that raises is caught, recorded and the run continues.

**Classes for things that hold state, functions for everything else.** `Server`, `Mock` and
`LogFollower` are classes because they own a process or a thread. Phases, vectors and helpers are
plain functions.

**Name a check by the behaviour it proves.** `"reserve: two pins never share a connection"`, not
`"test_reserve_2"`. The name is what a failure shows at 2am.

**A phase driving hundreds of vectors ticks, it does not narrate.** One mark per vector, so a
wedged run is visibly wedged:

```python
with term.progress("security", "trav-url", len(vectors)) as pr:
    for vec in vectors:
        pr.bad() if leaked else pr.ok()
```

**Put the observed value in `detail`.** It is only printed on failure, so it costs nothing and
saves a re-run: `"got %r want %r" % (got, want)`.

**`p.secure(...)` is for vulnerabilities, not bugs.** It outranks every other result and turns the
exit code into `2`. A wrong status code is `p.check`; a smuggled request body is `p.secure`.

**Collecting failures instead of asserting per vector** is fine when a phase drives thousands of
hostile requests and the useful output is what went wrong, not a pass count. Accumulate
`(label, detail)` pairs and flush them at the end, naming the labels that are vulnerabilities:

```python
findings.append(("PATH_TRAVERSAL_URL", "%s returned the file" % vector))
...
ctx.phase("security").record(findings, security=("PATH_TRAVERSAL_URL", "RESPONSE_SPLIT"))
```

### Hooks

| Hook | Use it for |
|------|-----------|
| `add_arguments(parser)` | suite-specific flags |
| `configure(cfg)` | derived config, before anything starts |
| `build_server(cfg)` | non-default flags, TLS probe, working directory |
| `setup(ctx)` | start mocks, register in `ctx.resources` |
| `before_phases(ctx)` | boot-time evidence, or preconditions worth failing fast on |
| `teardown(ctx)` | stop them, always runs |

And three switches:

| Attribute | Effect |
|-----------|--------|
| `stop_on_security` | end the run at the first security finding, when later phases would only be measuring an already-compromised server |
| `phase_timeout` | seconds per phase before recording `TIMEOUT` and moving on, so one wedged phase can't stall the run |
| `confirm_exit` | verify the master process actually died during teardown, for suites that kill workers |
| `heartbeat` | seconds between "still running" lines in CI, where progress marks are suppressed and a silent job gets killed |

A TLS-serving suite, for example:

```python
def build_server(self, cfg):
    return common.Server(cfg, flags=("--use-https", "--https-port-override"),
                          probe=common.tls_probe, label="/health (HTTPS)")
```

## Diagnosing a failure

**A wall of `None` results** usually means the worker died mid-phase, not that the assertions
broke. The runner says so explicitly and waits for the master to revive it.

**`[wfx-crash:*]` lines in red** are crash dumps or sanitizer reports from *this* run;
`crash_logs` is wiped at startup so nothing stale replays. ASan and UBSan are pointed there
because `wfx run --detach` sends stderr to `/dev/null`, which would otherwise discard them.

**Turn up the logs** with `--wfx-logs all` to see everything WFX printed, or `--wfx-logs crash` to
see only dumps. `base_audit` defaults to `crash` because it provokes contract violations by the
thousand, so its `[ERR]` lines are the tests working and printing them buries the results.

**Run one phase** with `--phase <name>` while iterating. Full runs take minutes.

## Adding to `common`

If two suites need the same thing, it belongs in `common/`. Four near-identical `Server` classes
are how a change to the launch environment ended up needing to be written four times, and missed
twice.
