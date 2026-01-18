# Installation

!!! important
    WFX currently supports **Linux only**.

    - Linux: ✅ Supported
    - WSL: ✅ Supported
    - Windows (native): ❌ Not supported
    - macOS: ❌ Not supported

---

## Overview

This section will guide you through installing all necessary dependencies, setting up WFX, and verifying your environment.

### Prerequisites

- **C++20**
- **CMake 3.20+**
- **Ninja build system** (optional, recommended)
- **Git**

<p>The following commands install required tools on common Linux distributions.
Other distributions may require equivalent packages.</p>

<strong>Ubuntu / Debian</strong>
<pre class="code-format">
- sudo apt update
- sudo apt install -y build-essential cmake git

# Optional (recommended)
- sudo apt install -y ninja-build
</pre>

<strong>Fedora</strong>
<pre class="code-format">
- sudo dnf install -y gcc-c++ cmake git

# Optional (recommended)
- sudo dnf install -y ninja-build
</pre>

<strong>Arch Linux</strong>
<pre class="code-format">
- sudo pacman -S --needed base-devel cmake git

# Optional (recommended)
- sudo pacman -S ninja
</pre>

### Steps

<ol>
  <li>
    <strong>Create an empty directory</strong> (name it whatever you want, 'mydir' is just a placeholder)
    <pre class="code-format">
- mkdir mydir
- cd mydir</pre>
  </li>

  <li>
    <strong>Clone the WFX repository</strong>
    <pre class="code-format">
- git clone https://github.com/Altered-commits/WFX.git
- cd WFX
- git checkout dev</pre>
  </li>

  <li>
    <strong>Configure and build WFX</strong>

    <p>
      Choose one of the following build methods:
    </p>

    <p>
      <strong>Option A (recommended): Build with Ninja</strong><br>
      Provides faster build times and better parallelism
    </p>
    <pre class="code-format">
- cmake -S . -B build -G Ninja
- cmake --build build</pre>

    <p>
      <strong>Option B: Build with the default CMake generator</strong><br>
      Use this option if Ninja is not installed
    </p>
    <pre class="code-format">
- cmake -S . -B build
- cmake --build build</pre>
  </li>

  <li>
    <strong>Move the built executable to your main directory</strong>
    <pre class="code-format">
- mv wfx ..
- cd ..</pre>
  </li>

  <li>
    <strong>Verify installation</strong>
    <pre class="code-format">
- ./wfx</pre>
    You should see <strong>WFX</strong> being printed.
  </li>
</ol>

### Documentation (Optional)

This section explains how to build and preview the WFX documentation locally.

Documentation is built using **MkDocs Material**.  
Do **not** install plain `mkdocs`; it is missing required features and extensions used by this project.

!!! note
    Documentation can be built on **Linux, macOS, and Windows**, even though WFX itself currently supports Linux only.

<strong>Requirements</strong>

- **Python 3.8+**
- **pip**

Verify Python is available:

<pre class="code-format">
- python3 --version
</pre>

<strong>Virtual environment setup</strong>

Creating a virtual environment is <strong>strongly recommended on all platforms</strong> to ensure reproducible documentation builds and to avoid dependency conflicts.

<strong>Linux / macOS</strong>
<pre class="code-format">
- python3 -m venv .venv
- source .venv/bin/activate
</pre>

<strong>Windows (PowerShell)</strong>
<pre class="code-format">
- python -m venv .venv
- .venv\Scripts\Activate.ps1
</pre>

<strong>Install dependencies</strong>

Install <strong>MkDocs Material</strong> (not plain MkDocs):

<pre class="code-format">
- pip install mkdocs-material
</pre>

This package includes all required themes, extensions, and plugins used by WFX documentation.

<strong>Serve the documentation locally</strong>

From the repository root:

<pre class="code-format">
- mkdocs serve
</pre>

Then open your browser at:

<pre class="code-format">
- http://127.0.0.1:8000
</pre>

---

## Notes

- Windows and macOS will be added in the future.
- The most recent and up-to-date code is available on the `dev` branch. The `main` branch will host stable releases in the future. Until then, it is recommended to use the `dev` branch.
- WFX is currently tested only on Ubuntu and Debian-based Linux distributions. Other Linux distributions may work, but are untested.
- Kernel TLS (used for HTTPS acceleration) requires a sufficiently recent Linux kernel. Older kernels may compile but will not support certain runtime features.

---

Continue to **[Your First WFX Program](first_program.md)**