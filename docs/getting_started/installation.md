# Installation

!!! note "Note"
    WFX currently supports **Linux only**.

    - Linux: ✅ Supported
    - WSL: ✅ Supported
    - Windows (native): ❌ Not supported
    - macOS: ❌ Not supported

---

## Overview

This section will guide you through installing all necessary dependencies, setting up WFX, and verifying your environment.

### Prerequisites

- **C++17** (g++, clang++)
- **CMake 3.20+**
- **Ninja build system**
- **Git**
- **Python 3.x** (for MkDocs and documentation, optional)

<p>The following commands install required tools on common Linux distributions.
Other distributions may require equivalent packages.</p>

<strong>Ubuntu / Debian</strong>
<pre class="code-format">
- sudo apt update
- sudo apt install -y build-essential cmake ninja-build git
</pre>

<strong>Fedora</strong>
<pre class="code-format">
- sudo dnf install -y gcc-c++ cmake ninja-build git
</pre>

<strong>Arch Linux</strong>
<pre class="code-format">
- sudo pacman -S --needed base-devel cmake ninja git
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
    <pre class="code-format">
- cmake -S . -B build -G Ninja
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

---

## Notes

- Windows and macOS will be added in the future.
- The most recent and up-to-date code is available on the `dev` branch. The `main` branch will host stable releases in the future. Until then, it is recommended to use the `dev` branch.
- WFX is currently tested only on Ubuntu and Debian-based Linux distributions. Other Linux distributions may work, but are untested.
- Kernel TLS (used for HTTPS acceleration) requires a sufficiently recent Linux kernel. Older kernels may compile but will not support certain runtime features.

---

Continue to **[Your First WFX Program](first_program.md)**