# Men of War: Assault Squad 2 Memory Guard

> [!IMPORTANT]
> The `experimental/allocator-diagnostics-v2.5` branch is not the public stable
> release. Its DLL identifies itself as `2.5.0-experimental.4`. Use the v2.4.2
> GitHub release for the stable rollback build.

An experimental 32-bit proxy DLL for the Steam edition of *Men of War: Assault
Squad 2*. It mitigates the known 2 GB lobby-allocation crash, keeps emergency
virtual address space available for allocation recovery, preserves crash dumps,
and refuses VAC-secured hosting and joining while the custom DLL is loaded.

> [!CAUTION]
> **Use this project at your own risk.** Modifying a game process may be
> incompatible with anti-cheat systems. The VAC guards are designed to refuse
> secure sessions, not to bypass or disable VAC, and they cannot guarantee that
> Valve or a game server will consider the DLL safe. The project owner and
> contributors are not responsible for VAC bans, other account restrictions,
> lost data, crashes, or any other damage. If account risk is unacceptable, do
> not use this DLL in multiplayer.
>
> note from the developer: 
>
> if you are reading this before reading the rest of the post:
>
> keep in mind that this DLL has a safe-guard system to avoid creating or joining VAC servers
> it has extensively been tested on both linux and windows and not a single problem so far.
> extensively tested does not mean its 100% safe, just that nobody was unlucky so far.
> i made this dll to play assault squad 2 with my girlfriend, we like base defense missions.
> i hope everyone can enjoy this and have a good time like we do.

## Installation

Memory Guard has been tested only with the Steam `3.262.1` October 2019 build.
Its allocator hooks target that executable. Do not use it with a different,
modified, or repacked `mowas_2.exe`.

### Windows

1. Close the game.
2. Download `dbghelp.dll` from the latest
   [release](https://github.com/Igordalessandro/mowas2-memory-guard/releases).
3. Open the game directory. Steam's default is
   `C:\Program Files (x86)\Steam\steamapps\common\Men of War Assault Squad 2`.
4. Put `dbghelp.dll` beside `mowas_2.exe`.
5. Start the game through Steam.
6. Confirm that a timestamped `mowas2-memory-guard-*.log` appears in the game directory and
   reports that the allocator and VAC guards installed successfully.

### Linux / Steam Proton

1. Launch the game through Proton once, then close it.
2. Download `dbghelp.dll` from the latest release.
3. Put it beside `mowas_2.exe`, normally under
   `steamapps/common/Men of War Assault Squad 2/`.
4. In Steam, open **Properties > General > Launch Options** and enter:

   ```sh
   PROTON_USE_WOW64=1 WINEDLLOVERRIDES="dbghelp=n,b" %command%
   ```

5. Launch the game and confirm that a timestamped `mowas2-memory-guard-*.log` appears in the
   game directory.

`PROTON_USE_WOW64=1` selects Proton's newer WoW64 execution path when the
installed Proton version supports it. It does **not** turn this 32-bit game into
a 64-bit process or expand its 32-bit pointer space. `WINEDLLOVERRIDES` tells
[Proton](https://github.com/ValveSoftware/Proton) and Wine to try the native
`dbghelp.dll` from the game directory before its built-in implementation. If
you prefer Protontricks,
run [`protontricks --gui`](https://github.com/Matoking/protontricks), choose
*Men of War: Assault Squad 2* (App ID `244450`),
select the default Wine prefix, open `winecfg`, and add `dbghelp` under
**Libraries** as **Native, then Builtin**. You must still copy `dbghelp.dll` into
the game directory. Use either the Steam launch option or the Protontricks
override; both configure the same DLL preference.

### Uninstall

Close the game and remove `dbghelp.dll` from its directory. On Linux, also
remove the Steam launch option or the Protontricks `dbghelp` override. The DLL
does not modify saves or permanently patch `mowas_2.exe`.

## What it does

### Memory and crash mitigation

- Hooks both known engine allocation entry points.
- Replaces only the observed corrupt `2,147,483,552`-byte lobby request with a
  4 MiB request. It does not broadly shrink every large allocation.
- Holds 64 MiB of uncommitted emergency virtual address space in three stages:
  an 8 MiB primary reserve, an 8 MiB critical reserve, and a contiguous 48 MiB
  final reserve suitable for the large requests observed while loading heavy
  mods such as RobZ.
- Releases the primary reserve when total free address space, the largest
  contiguous block, or an upcoming large request indicates exhaustion is near.
  The critical reserve has a lower threshold. Large requests release and
  re-scan stages until enough contiguous headroom exists; the final reserve is
  also released proactively under emergency fragmentation or after an observed
  failure. The primary reserve can be re-armed after address space remains
  healthy for a full minute.
- Hooks the executable's `HeapAlloc`, `HeapReAlloc`, `HeapFree`,
  `VirtualAlloc`, and `VirtualFree` imports without replacing their allocator
  semantics. On an allocation failure it releases remaining reserves and
  retries the same request once.
- Records 64-bit allocation counts, the largest request, committed/reserved memory,
  total free address space, and the largest free region every 30 seconds.
  "64-bit" here describes the diagnostic integer width only: the DLL remains
  PE32/x86, uses 32-bit pointers, and does not require an x64 process. Native
  32-bit Windows has not yet been verified with the experimental build.
- Samples allocation lifetimes and engine request call sites using fixed-size,
  preallocated tables. Every two minutes the log lists the largest request,
  retained-allocation, free, and engine-request sites as executable-relative
  addresses suitable for later reverse engineering.
- Carries the higher-level engine caller into retained-allocation samples,
  records sampled heap identity and heap compatibility mode, and retains a
  fixed 256-entry allocation flight recorder for large and low-memory requests.
- Validates both hook-site signatures and rolls back partial hook installation
  instead of patching an unknown executable.
- Forwards `ImageNtHeader` and `MiniDumpWriteDump` to the real system
  `dbghelp.dll`, preserving the game's crash-dump path.

This is not a 64-bit conversion or garbage collector. It cannot reclaim live
objects owned by the engine, eliminate all fragmentation, or guarantee that an
out-of-memory crash will never occur. The supported executable is already
Large Address Aware, so applying a separate 4 GB patch does not raise its
address-space limit further.

### VAC safety guards

The game leaves **VAC Secured** checked by default when creating an Internet
game. Memory Guard intentionally leaves the visible checkbox unchanged, but it
refuses `SteamGameServer_Init` if the game requests secure server mode. The user
must uncheck **VAC Secured** and try hosting again.

Before a secure game-server session can authenticate, the DLL rejects any
SteamUser018 `InitiateGameConnection` request whose `bSecure` flag is true.
The guard detours that method's implementation without modifying Proton's
SteamUser vtable, and does not patch or query SteamMatchmaking. Earlier
experimental builds modified Steam interface dispatch tables, but that was
removed because it could destabilize friend and persona names under Proton's
WoW64 bridge.

These are fail-closed refusal mechanisms. They do not disable VAC, forge VAC
state, make a secure server insecure, or provide immunity from enforcement. If
the host or join safeguards cannot be installed, the DLL closes the game rather
than continuing without them.

For implementation details, limitations, logs, and troubleshooting, see the
[project wiki](https://github.com/Igordalessandro/mowas2-memory-guard/wiki).

## Compatibility and security notice

The DLL patches code inside the game process and may trigger generic antivirus
or heuristic detections. Build it from the published source if you do not trust
the release binary. This project is an experimental, AI-assisted derivative of
[mowas2-perf](https://github.com/Lukas-Ldc/mowas2-perf) and is not affiliated
with Valve, 1C Company, Digitalmindsoft, or Best Way.

## Credits and project history

This project began with
[Lukas-Ldc/mowas2-perf](https://github.com/Lukas-Ldc/mowas2-perf), whose author
investigated the multiplayer/skirmish lobby crash that reports
`Failed to allocate memory. Requested 2147483552 bytes`. The original project
identified the game's bad lobby allocation and demonstrated that intercepting
the engine allocator could let the lobby continue.

Memory Guard v2.4 retains that central idea while narrowing the replacement to
the exact observed request, adding staged virtual-address-space recovery and
allocator diagnostics, preserving crash dumps, and adding the fail-closed VAC
host and join guards. Thank you to Lukas-Ldc for publishing the original work
and source that made this continuation possible.

## Files the DLL creates

Timestamped `mowas2-memory-guard-*.log` files are written next to `dbghelp.dll`
in the game folder. They contain hook validation, periodic address-space and
allocator telemetry, sampled call-site rankings, heap statistics, flight
recorder entries, emergency-reserve events, and lobby-workaround events. The
newest ten sessions are retained. A copy of the real Windows `dbghelp.dll` is
also placed in `%TEMP%` so the proxy can forward both `ImageNtHeader` and
`MiniDumpWriteDump`.

The same folder contains **`DLL Crash Report.txt`**. It is overwritten for
each run and continuously refreshed with the version, platform, guard status,
latest memory snapshot, allocator totals, hottest sampled call sites, last
recovery event, and any captured unhandled exception. It contains ASCII only
and has a hard 900-byte ceiling so the entire file fits in Steam's roughly
1,000-character comment limit. A report remains useful after a silent crash,
but it can also describe a healthy or still-running session.

When reporting a problem, paste the entire contents of `DLL Crash Report.txt`
and separately state the active mods, last in-game action, and whether the game
showed a dialog or silently returned to the desktop. The compact report avoids
command lines, usernames, full installation paths, server IP addresses, and
Steam IDs. If maintainers request deeper evidence, attach the matching full
timestamped log to a GitHub issue instead of trying to paste it into Steam.

## License

MIT, see [LICENSE](./LICENSE). No warranty, no liability.


# For developers

The sections below are for anyone who wants to build the DLL from source, modify it, or contribute. Not needed if you just want to play.

## Build from source

The original `build.bat` uses MSVC with the x86 C++ workload. This working copy also provides `build-linux.sh`, tested with the i686 target from llvm-mingw.

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

Or on Linux:

```sh
LLVM_MINGW_ROOT=/opt/llvm-mingw ./build-linux.sh
```

The script auto-discovers every `.cpp` under `src/`, compiles MinHook from the submodule sources, and links the result to `dist/dbghelp.dll`. Drop a new `.cpp` in any domain folder and it gets picked up on the next build.

The resulting `dist/dbghelp.dll` goes in the game folder. The v2 artifact built from this working copy is intentionally different from the upstream v1 release.

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
- **Architecture:** PE32/x86. It must be loaded by the game's 32-bit process;
  its 64-bit diagnostic counters do not make it a 64-bit DLL or expand the
  game's virtual address space.
- **Visual Studio 2022 Build Tools** : `cl.exe` / `link.exe` x86, vcvarsall, MSVC 19.x.
- **`dbghelp.dll` proxy pattern** : chosen over the more common `iphlpapi.dll` because SteamStub V3 blacklists `iphlpapi.dll` as a proxy target. `dbghelp.dll` passes the integrity check.

## Architecture

The source tree is small enough to read in one sitting. `/I"src"` is set at compile time so cross-module includes are flat (e.g. `#include "shared/logging/logger.h"` from any file).

```
src/
├── app/
│   ├── dllmain.cpp           # DllMain + delayed_init worker thread
│   ├── dbghelp_proxy.cpp     # runtime dbghelp API forwarder
│   └── config.h              # engine offsets + guard thresholds + log file name
│
├── shared/
│   ├── core/
│   │   ├── module.h          # ModuleSpec + MOWAS_REGISTER_MODULE
│   │   └── registry.cpp      # priority-ordered install dispatch
│   └── logging/
│       ├── logger.h
│       └── logger.cpp        # single log file, thread-safe printf-style
│
├── diagnostics/
│   ├── allocator_profiler.h
│   └── allocator_profiler.cpp # game IAT hooks + bounded call-site sampling
│
└── orchestration/
    ├── hooks.h
    ├── hooks_internal.h      # shared declarations for the two hooked functions
    ├── hooks.cpp             # MinHook install of engineAlloc + engineAllocCore
    ├── engine_alloc.cpp      # exact lobby workaround + allocator wrappers
    ├── memory_guard.h
    └── memory_guard.cpp      # VA telemetry + emergency reserve
│
└── safety/
    ├── vac_join_guard.h
    ├── vac_join_guard.cpp    # blocks secure lobby joins and authentication
    ├── vac_host_guard.h
    └── vac_host_guard.cpp    # blocks secured host initialization, fail-closed
```

### Extending the DLL

A tiny module registry is wired in (`shared/core/`) so additional side modules can be added without touching `orchestration/hooks.cpp`. The v2 memory monitor uses its own background thread rather than the registry.

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

`hooks::install()` calls `core::install_all()` after the mandatory MinHook hooks, which iterates the registered modules in ascending `priority` order. MSVC builds isolate installers with `__try / __except`; llvm-mingw builds call them directly because its x86 SEH cannot compile that STL-containing frame.
