# Engine Commands

The `wfx` command-line interface (CLI) provides several commands to interact with WFX, create projects, manage builds, and run the development server. Below is a detailed explanation of each command and its options.

---

## `wfx new`

Create a new WFX project.

**Usage:**

```bash
wfx new <project_name>
```

`<project_name>`: Required. Name of the project to create.

If no project name is provided, WFX will display an error.

---

## `wfx doctor`


Verify system requirements for the current workspace.

!!! warning "Deprecated"
    WFX now relies entirely on **CMake's build system** instead of a custom toolchain, making this check unnecessary. As a result, `wfx doctor` no longer performs environment, compiler, or dependency validation.

    The command may be repurposed or reintroduced in the future if additional validation or tooling becomes necessary.

**Usage (no-op):**

```bash
wfx doctor
```

---

## `wfx build`

Pre-build various parts of the project, such as templates or source code.

**Usage:**

```bash
wfx build <project_name> <target> [options]
```

#### Compulsory Arguments

| Argument          | Description                                                         |
|-------------------|---------------------------------------------------------------------|
| `<project_name>`  | Name of the project folder to build                                 |
| `<target>`        | Which part to build: `templates` or `source`                        |

#### Optional Flags

| Option  | Description                                             | Default | Requires value? |
|---------|----------------------------------------------------------|---------|------------------|
| `--env` | Which environment's config to build against (see below) | `local` | Yes              |

**Example:**

```bash
wfx build my-project source
wfx build my-project source --env stage
```

---

## `wfx run`

Start the WFX server.

**Usage:**

```bash
wfx run <project_name> [options]
```

#### Optional Flags

| Option               | Description                           | Default   | Requires value? |
|----------------------|----------------------------------------|-----------|-----------------|
|--host	               | Host to bind	                       | 127.0.0.1 | Yes             |
|--port	               | Port to bind	                       | 8080      | Yes             |
|--env                 | Environment to run (see below)        | local     | Yes             |
|--pin-to-cpu          | Pin workers to CPU cores              |     –     | No              |
|--use-https	       | Enable HTTPS connection	           |     –     | No              |
|--https-port-override | Override default HTTPS port           |     –     | No              |
|--detach              | Run server as a background daemon     |     –     | No              |

#### Additional Information
- **Default** specifies the value used by WFX when the option is not explicitly provided.
- **Requires value?** indicates whether an option must be followed by a value (for example, `--port 3000`) or can be used as a standalone flag (for example, `--debug`).
- `--use-https` by default uses port 443.
- `--https-port-override` overrides the HTTPS port using the value provided via `--port`.

**Example:**

```bash
wfx run my-project --host 0.0.0.0 --port 3000 --use-https --https-port-override
wfx run my-project --env stage
```
The first line starts the server on all interfaces, port 3000, HTTPS enabled. The second runs `my-project` against its `stage` environment.

---

## Environments (`--env`)

`wfx run` and `wfx build` both load their configuration from `config/wfx.<env>.toml`, where `<env>`
comes from `--env` (`local` when omitted). Each environment is a complete, standalone file with no
base file and no inheritance between environments, so `config/wfx.stage.toml` needs every section a
runnable config requires on its own.

```text
<your_project_name>/
└─ config/
   ├─ wfx.local.toml   # wfx run my-project
   ├─ wfx.stage.toml   # wfx run my-project --env stage
   └─ wfx.prod.toml    # wfx run my-project --env prod
```

`<env>` can be any name you like, `local`/`stage`/`prod` are just conventions, whatever matches an
existing `config/wfx.<env>.toml` works.

!!! note
    `--env` and the `[ENV]` section inside a `wfx.toml` file answer different questions. `--env`
    picks *which whole file* gets loaded. `[ENV] env_path`, once that file is loaded, points at a
    `.env` file to load secrets from. See [Environment Variables](../api_reference/env.md) for that
    part.

---

## `wfx control`

Manage running WFX servers.

**Usage:**

```bash
wfx control <subcommand> [project_name]
```

##### Subcommands

| Subcommand | Description                                      |
|------------|--------------------------------------------------|
| `list`     | List all running WFX servers with their status   |
| `folder`   | Print the WFX root and daemon directories        |
| `stop`     | Stop a running server by project name            |

**Examples:**

```bash
wfx control list
wfx control stop my-project
wfx control folder
```

`stop` sends `SIGTERM` to the master and blocks until it exits: normally up
to that project's configured `worker_shutdown_timeout` (see
[`[Linux]`](wfx_toml.md)), plus a couple of seconds of slack for the master's
own exit bookkeeping. If the master still hasn't exited by then, `stop`
force-kills it directly. Under normal conditions this shouldn't happen: the
master waits on all of its own workers within that same `worker_shutdown_timeout`
window and force-kills any straggler itself first, so it should always finish
well within the budget `stop` gives it.