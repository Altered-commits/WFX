# Engine Commands

The `wfx` command-line interface (CLI) provides several commands to interact with WFX, create projects, manage builds, and run the development server. Below is a detailed explanation of each command and its options.

---

## `wfx new`

Create a new WFX project.

**Usage:**

```bash
./wfx new <project_name>
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
./wfx doctor
```

---

## `wfx build`

Pre-build various parts of the project, such as templates or source code.

**Usage:**

```bash
./wfx build <target> [options]
```

##### Compulsory Arguments

| Argument       | Description                                                          |
|----------------|----------------------------------------------------------------------|
| &lt;target&gt; | Specify which part of the project to build: `templates` or `source`  |

##### Optional Flags

| Flag       | Description                                                                         |
|------------|-------------------------------------------------------------------------------------|
| --debug    | Currently a **no-op**. In the future, it will be used to enable runtime debug mode  |

**Example:**

```bash
./wfx build source
```

---

## `wfx run`

Start the WFX server.

**Usage:**

```bash
./wfx dev [options]
```

##### Optional Flags

| Option               | Description                 | Default   | Requires value? |
|----------------------|-----------------------------|-----------|-----------------|
|--host	               | Host to bind	             | 127.0.0.1 | Yes             |
|--port	               | Port to bind	             | 8080      | Yes             |
|--pin-to-cpu          | Pin workers to CPU cores    |     -     | no              |
|--use-https	       | Enable HTTPS connection	 |     –     | No              |
|--https-port-override | Override default HTTPS port |     –     | No              |
|--debug	           | Currently a **no-op**  	 |     –     | No              |

##### Additional Information
- **Default** specifies the value used by WFX when the option is not explicitly provided.
- **Requires value?** indicates whether an option must be followed by a value (for example, `--port 3000`) or can be used as a standalone flag (for example, `--debug`).
- `--no-cache` cannot be combined with `--no-source-cache` or `--no-template-cache`.
- `--use-https` by default uses port 443.
- `--https-port-override` overrides the HTTPS port using the value provided via `--port`.

**Example:**

```bash
./wfx run --host 0.0.0.0 --port 3000 --use-https --https-port-override
```
This starts the server on all interfaces, port 3000 and HTTPS enabled.