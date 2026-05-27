# mowas2-perf

### What this project does

`mowas2-perf` is a small DLL you drop into the game's folder. It fixes the "Failed to allocate memory. Requested 2147483552 bytes" crash that prevents Men of War: Assault Squad 2 from reaching a skirmish lobby. The DLL catches the bad allocation, hands the engine a small valid buffer instead, and the game continues normally. A small log file is written next to the executable so you can see what fired.

Men of War: Assault Squad 2 is a 32-bit game from 2014, which means it can only address around 4 GB of memory at any given moment, no matter how much RAM your PC has. That limit was baked in when the game was compiled, and no Windows update or more powerful computer can change it. Your PC isn't the bottleneck, the game's own 32-bit ceiling is.

This is an experimental side project. Many memory-management ideas were tried in an attempt to improve the game's performance (custom allocators, texture compression, driver memory hints, and others) and most of them either didn't help or made things worse on real workloads, so they were removed. What's left is the one mitigation that survived measurement.

## Install

Tested only on the Steam version `3.262.1` (Oct 2019 build). The hooks rely on hard-coded byte offsets inside `mowas_2.exe`. If your copy is bit-identical to that Steam build, it should work. Anything else (older retail / boxed releases, future Steam patches that shift the binary, modified or repacked copies) will likely crash at install time.

1. Go to the Releases page of this repository.
2. Download the latest `dbghelp.dll` file.
3. Drop it into your game folder, next to `mowas_2.exe`. Default Steam path : `…\steamapps\common\Men of War Assault Squad 2\`.
4. Launch the game normally through Steam.

That's it. To uninstall, just delete `dbghelp.dll` from the game folder ; the DLL never modifies any game file (it only patches `mowas_2.exe` in memory at runtime), so removing it reverts the game to vanilla behaviour immediately. Saves are untouched.

The DLL uses techniques (runtime instruction patching, memory protection changes) that look suspicious to antivirus software. Your antivirus may flag it as a generic "trojan" the first time you launch the game. This is a false positive : the source code is in this repo, you can read it or compile it yourself.

The 4GB Patch by [NTCore](https://ntcore.com/4gb-patch/) is the tool many older games need to flip the `LargeAddressAware` flag on their `.exe` so they can use 4 GB instead of 2 GB. The Oct 2019 build of MoW:AS2 that Steam ships already has that flag set, so applying the 4GB Patch on top is not necessary.

## Disclaimer

I did not personally write or review the code in this repository. My role was :

- Setting up the toolchain.
- Interacting with the AI through many iterative turns to direct the design, refactors, and bug fixes.
- Running playtest sessions to catch real-world crashes and verify the DLL doesn't introduce regressions.

The only quality guarantee is therefore : *"two LLMs cross-reviewed each other across many turns, and it ran without crashing in our playtests."*.

## Files the DLL creates

A `mowas2-perf.log` file is written next to `dbghelp.dll` in the game folder. It contains the install banner and one line each time the clamp fires. It is truncated at each launch and safe to delete any time. A copy of the real Windows `dbghelp.dll` is also placed in your Windows temp folder (`%TEMP%`, typically `C:\Users\<you>\AppData\Local\Temp\mowas2_real_dbghelp.dll`) so the proxy can forward `ImageNtHeader` calls to it.

## License

MIT, see [LICENSE](./LICENSE). No warranty, no liability.


# For developers

The sections below are for anyone who wants to build the DLL from source, modify it, or contribute. Not needed if you just want to play.

## Build from source

You need MSVC with the x86 C++ workload. **[Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/)** (free, command-line only) is what we tested with ; the full Visual Studio IDE or older 2019 Build Tools should work too. Other toolchains (MinGW, Clang-on-Windows) would need porting because the code uses MSVC-specific SEH (`__try` / `__except`).

### Clone with submodules

Third-party dependencies (MinHook) live in `thirdparty/` as pinned git submodules, not as vendored copies. You must clone with submodules :

```bat
git clone --recurse-submodules <repository>
```

Or after a regular clone :

```bat
git submodule update --init --recursive
```

If you forget this step, the build will fail with `Cannot open include file: MinHook.h` (or similar) because `thirdparty/<lib>/` is empty.

### Compile

```bat
build.bat
```

The script auto-discovers every `.cpp` under `src/`, compiles MinHook from the submodule sources, and links the result to `dist/dbghelp.dll`. Drop a new `.cpp` in any domain folder and it gets picked up on the next build.

The resulting `dist/dbghelp.dll` goes in the game folder, same as the Releases binary. Building from source and downloading a release produce a functionally identical DLL.

## Dependencies

To add a new third-party dependency, use a git submodule pinned to a specific commit or tag, never a vendored copy-paste into `thirdparty/`. The submodule hash lets any reviewer verify the source byte-for-byte against the public upstream repo. This matters for a DLL that runs inside a game process, since a vendored copy could quietly carry private or malicious edits that nobody would notice. After adding the submodule, update `build.bat` to compile and include it.

| Submodule | Path | Pinned tag | License |
| --- | --- | --- | --- |
| **[MinHook](https://github.com/TsudaKageyu/minhook)** : trampoline hook installer | `thirdparty/minhook` | `v1.3.4` | BSD-2-Clause |

## Toolchain

This project would not have existed without the following ecosystem of tools, listed for anyone wanting to reproduce the setup.

### Reverse engineering

[Ghidra](https://github.com/NationalSecurityAgency/ghidra) is the free open-source SRE framework from the NSA. We used it to disassemble `mowas_2.exe` and name the two engine allocator entry points we hook.

[GhidraMCP](https://github.com/LaurieWired/GhidraMCP) exposes Ghidra's decompiler and symbol database to an LLM, so Claude could walk the binary itself (decompile, list xrefs, rename) during the conversation instead of paste-and-pray. That's what made the reverse engineering from scratch possible.

[Steamless](https://github.com/atom0s/Steamless) by `atom0s` strips the SteamStub V3 wrapper from the game executable so Ghidra analyses the real code instead of a packed shell.

### LLMs

[Claude Code](https://www.anthropic.com/claude-code) was the primary coding agent. It did the vast majority of the line-by-line writing, refactoring, splitting, and fix application. Hooks, sub-agents, MCP servers, and slash commands kept the workflow tight.

[OpenAI Codex](https://openai.com/codex/) acted as the second-opinion reviewer. It audited each major Claude iteration and flagged what Claude had missed (races, atomicity bugs, dead code, architecture smells) ; findings fed back to Claude for fixes. Two differently-trained LLMs catch each other's blind spots.

### Build / target

- **Windows 10/11**, 32-bit DLL target.
- **Visual Studio 2022 Build Tools** : `cl.exe` / `link.exe` x86, vcvarsall, MSVC 19.x.
- **`dbghelp.dll` proxy pattern** : chosen over the more common `iphlpapi.dll` because SteamStub V3 blacklists `iphlpapi.dll` as a proxy target. `dbghelp.dll` passes the integrity check.

## Architecture

The source tree is small enough to read in one sitting. `/I"src"` is set at compile time so cross-module includes are flat (e.g. `#include "shared/logging/logger.h"` from any file).

```
src/
├── app/
│   ├── dllmain.cpp           # DllMain + delayed_init worker thread
│   ├── dbghelp_proxy.cpp     # runtime ImageNtHeader forwarder
│   └── config.h              # engine offsets + clamp threshold + log file name
│
├── shared/
│   ├── core/
│   │   ├── module.h          # ModuleSpec + MOWAS_REGISTER_MODULE
│   │   └── registry.cpp      # priority-ordered install dispatch
│   └── logging/
│       ├── logger.h
│       └── logger.cpp        # single log file, thread-safe printf-style
│
└── orchestration/
    ├── hooks.h
    ├── hooks_internal.h      # shared declarations for the two hooked functions
    ├── hooks.cpp             # MinHook install of engineAlloc + engineAllocCore
    └── engine_alloc.cpp      # the clamp itself + the hooked allocator wrappers
```

### Extending the DLL

The project ships a single feature, but a tiny module registry is wired in (`shared/core/`) so anyone forking can add side modules without touching `orchestration/hooks.cpp`. The registry only supports an `install` callback — there is no background tick thread (this project doesn't need one).

To add a feature, drop a `.cpp` anywhere under `src/` (`build.bat` picks it up automatically) and end it with a registration block :

```cpp
// src/<your_module>/<your_feature>.cpp
#include "app/config.h"
#include "shared/logging/logger.h"
#include "shared/core/module.h"

#include <cstdint>

namespace {
bool your_install(std::uintptr_t game_base) {
    // ... install your hook, allocate state, log "OK"
    return true;
}
}  // anonymous namespace

MOWAS_REGISTER_MODULE(your_feature, (::core::ModuleSpec{
    "your_feature",
    100,                  // priority (lower installs earlier; 100 is the default)
    &your_install,
}));
```

`hooks::install()` calls `core::install_all()` after the mandatory MinHook hooks, which iterates the registered modules in ascending `priority` order. Each `install_fn` runs under `__try / __except`, so a misbehaving module can't crash the rest of the install path. Forks expanding the scope are welcome but won't be merged upstream — this repo stays focused on the menu crash.
