# v2.5.0-experimental.13

Experimental.13 retains full per-request caller-site profiling and adds
bounded caller caching, VA-scan timing and system-memory diagnostics. It also
includes the optional Proton Steam-text workaround from the unreleased .11
candidate. This is a **prerelease**; stable GitHub Latest remains **v2.4.2**.
The intermediate .11/.12 candidates were not separate public releases.

## What's included

- **Full request-site profiling, not light profiling.** Every observed heap
  allocation/reallocation request contributes its real count/size to its
  registered caller site. The light-mode sampler and selector were removed
  before publication. `MOWAS2_PROFILE_MODE` has no effect. Existing lifetime,
  free-site and engine sampling remain unchanged, and bounded tables can
  still saturate. Global totals continue independently of site coverage.
- **Bounded caller caching.** A fixed 64-entry per-thread cache retains both
  known sites and saturated-table misses. Advisory pressure/saturation reads
  avoid locked read-modify-write operations without reducing accounting.
- **VA-scan cost evidence.** Full logs report cumulative scan calls, query
  counts, wall time and worst duration by category. A large-request preflight
  walk is skipped when none of its three reserve stages remains; actual
  failures still take fresh snapshots and follow unchanged recovery retries.
- **Physical-memory context.** Periodic full-log records add available RAM
  and process page-fault totals. Wall time includes scheduling and faults
  include soft plus hard faults; neither proves the cause of a slowdown alone.
- **Proton Steam-text compatibility candidate.** A validated-layout optional
  bridge workaround refreshes high-address native C strings into bounded,
  immutable low-address copies. It targets stale/reused persona/lobby strings
  without modifying Steam dispatch tables. Unsupported layouts/read failures
  fall back; native Windows skips it. Real UI field confirmation is still
  required, and this is not a claimed fix for checkerboard rendering.

`DLL Crash Report.txt` retains its 900-byte cap. Detailed scan/timing evidence
stays in the timestamped full session log. Reports are not uploaded automatically.

## What the reports establish—and what they don't

The new RobZ experimental.10 session records **four successful allocation
recoveries**, releasing 48, 32, 16 and 8 MiB reserve stages. A later 4,591,679-byte
heap request failed with about 39 MiB free overall, only a 1 MiB largest free
region, and no reserve remaining. This supports 32-bit VA exhaustion and
fragmentation; it does not prove a physical-RAM shortage or a specific engine
resource leak. Same-sized allocations at different callers are a lead, not
proof of a duplicate object.

Repeated-save memory growth and disabled older mod versions increasing usage
remain investigation leads. There is no automatic resource purge or confirmed
reload-leak fix in this build. The observed slow-motion cause is unconfirmed;
this release does not promise an FPS improvement or that huge mods will fit.

## Preserved behavior and safety

Still a **single PE32/x86 DLL**, not a 64-bit game conversion. Native allocator
semantics, flags and valid sizes remain unchanged. The exact corrupt lobby
sentinel remains the only allocation-size substitution. The 8/8/48/32 MiB
reserve policy, staged identical-request retries, recovery rearming, long-path
compatibility and editor/Workshop isolation are retained.

VAC-secured hosting and joining refusals remain enabled and fail closed if
their guards cannot install. They **do not disable or bypass VAC, guarantee
account safety or provide ban immunity**. Use at your own risk: the project
owner and contributors are not responsible for account bans/restrictions,
lost data, crashes or other damage. Do not use it in multiplayer if that risk
is unacceptable.

## Installation and rollback

Close the game and editor first and retain your current DLL as a rollback.

- **Windows:** download this release's `dbghelp.dll` and place it beside
  `mowas_2.exe`, replacing the previous Memory Guard DLL.
- **Linux / Steam Proton:** use the same DLL in the same game directory and
  keep the native-first `dbghelp` override. In Protontricks/Wine configuration,
  set Libraries → `dbghelp` → Native, then Builtin, or keep the existing
  `WINEDLLOVERRIDES="dbghelp=n,b" %command%` Steam launch configuration.
  Preserve any other required Proton options. No profiling flag is needed.

The startup log should identify `2.5.0-experimental.13` and
`ALLOC PROFILER installed: mode=full request-site=1/1`, plus successful VAC
guard installation. The release checksum is
`f4d2ebab75e63943a751a9a828f83c6b07c37a3e886cb03df7e08c5445482fcb`.
The earlier local light-mode .13 artifact was superseded and is not attached.

For rollback, close the game and restore your saved DLL or use stable v2.4.2.

## Useful testing reports

Provide `DLL Crash Report.txt` (or the previous-session copy after a restart),
the matching full `mowas2-memory-guard-*.log`, any dialog text, mod versions,
map/player/unit settings, graphics settings and time until failure. Review
full logs for private paths/names before sharing.

For reload growth, compare a fresh-process load, return to menu and repeated
loads of the **same save** at fixed settling times. For duplicate mod versions,
compare fresh processes with only the current version discoverable versus both
folders present but the old version disabled; preserve old data rather than
deleting it. For slow motion, distinguish choppy camera/UI from slowly moving
units and note whether the other player's game also slows.

## Validation and documentation

Seven portable regression suites passed with ASan/UBSan. All sixteen Windows
x86 regression executables were freshly compiled with strict warnings and
passed locally under isolated Wine 11.19. Native-Windows regression status is
available in the [source CI checks](https://github.com/Igordalessandro/mowas2-memory-guard/actions/workflows/regression-tests.yml).
These are controlled production-code tests, not a native-gameplay or heavy-mod
stability claim. The DLL exports only `ImageNtHeader` and `MiniDumpWriteDump`.

See the [experimental.13 guide](https://github.com/Igordalessandro/mowas2-memory-guard/blob/v2.5.0-experimental.13/docs/EXPERIMENTAL-13.md),
[memory/lag investigation](https://github.com/Igordalessandro/mowas2-memory-guard/blob/v2.5.0-experimental.13/docs/INVESTIGATION-2026-10-21-Lag-And-Mod-Crashes.md)
and [Proton text investigation](https://github.com/Igordalessandro/mowas2-memory-guard/blob/v2.5.0-experimental.13/docs/PROTON-Steam-Text-Investigation.md).
