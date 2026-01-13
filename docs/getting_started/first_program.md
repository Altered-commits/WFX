# Your First WFX Program

This guide will help you create and run your first WFX project, verifying that your installation works correctly.

---

## Steps

Follow the steps below in order, without skipping any.

### 1. Create a new project

Run the following command in your terminal (replace `<project_name>` with your own project name, e.g., `myproj`):

```bash
./wfx new <project_name>
```

This will generate a full project folder structure for you.

### 2. Run the WFX server

To start the server, run this final command:

```bash
./wfx run <project_name>
```

By default:

- The server runs on 127.0.0.1:8080
- Three routes are preconfigured in `main.cpp` inside of `src/` directory:
    - /     -> serves the `index.html` from `templates/` directory
    - /text -> returns plain text
    - /json -> returns JSON object

Open your browser and visit these routes to verify the content. If you see the expected output, WFX is working correctly.

---

## Congratulations!

WFX is now running on your system.

For beginners, it is recommended to continue to the **[Overview](../core_concepts/project_structure.md)** section to learn about the project structure and available engine commands.