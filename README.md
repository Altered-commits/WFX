# WFX

> Scene opens: two developers staring at a terminal. One of them hasn't slept in 36 hours :(

**Dev 1:** Dawg... what even *is* this thing?  
**Dev 2:** Haven't you heard about it?  
**Dev 1:** No? Tf is WFX?  
**Dev 2:** Weird Framework  
**Dev 1:** ...wat?  
**Dev 2:** eXactly (.\_.)  
**Dev 1:** (.\_.)  

**Silence...**   
The fans sound like a jet engine. The build somehow finishes.
Somewhere, a socket coughs itself awake.
No logs, no confetti, just a binary sitting there opening its eyes for the first time.

## Architecture

Everything runs on one event loop [Epoll / IoUring / IOCP]. Flow is quite simple: request -> event-loop (read from socket) -> core-engine (runs callbacks and creates response) -> event-loop (write to socket)  

## Folder Structure

**cli/** - command-line tools: build, new, dev, doctor  
**config/** - TOML loader  
**engine/** - runtime core, template execution  
**http/** - routing, parser, serializer, response machinery, etc.  
**os_specific/** - Linux and Windows platform code  
**shared/** - internal shared logic  
**include/** - public headers for user extensions  

## Shared Libraries (`lib/`)

Each shared module is self-contained and linked into the core runtime.

- **utils/** - logging, crypto, memory, file I/O, etc.  

## Build

Some dependencies which need to be resolved before building engine:

- **Linux:** `sudo apt install -y cmake build-essential ninja-build`
- **C++ Standard:** C++17

Follow each command step by step to build the engine binary:

```bash
 - git clone https://github.com/Altered-commits/WFX.git
 - cd wfx
 - cmake -S . -B build -G Ninja
 - cmake --build build
```

Now that the engine binary has been created, follow these steps to use `wfx`:

#### OS-Specific Commands

#### Linux / macOS
```bash
 - mv wfx ..
 - cd ..
```

Now you can start using `wfx` :)