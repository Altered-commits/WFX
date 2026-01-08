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

Everything runs on a single event loop (epoll / io_uring / IOCP).  
Request flow:

- socket read -> event loop
- event loop -> core engine (execute callbacks, build response)
- core engine -> event loop
- event loop -> socket write

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

## Getting Started

> [!NOTE]
> WFX requires a native build and is not distributed as a prebuilt binary.

All documentation is hosted and kept up to date here:

**https://altered-commits.github.io/WFX/**

**Key entry points**:

- **Getting Started / Build**  
  https://altered-commits.github.io/WFX/getting_started/installation/

- **Project Structure**  
  https://altered-commits.github.io/WFX/core_concepts/project_structure/