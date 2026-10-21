# v2.5 allocator-diagnostics experiment

The current prerelease is `2.5.0-experimental.10`. It preserves experimental.9's
allocation policy and adds exact preflight-release attribution, fresh failure
snapshots, a separate large-allocation history, and aligned free-hole counts.
See the [experimental.10 diagnostic guide](docs/EXPERIMENTAL-10-Diagnostics.md)
and the [experimental.9 review record](docs/REVIEW-2026-10-20.md).

This branch starts from the published v2.4.2 stable rollback build. It does
not add SteamMatchmaking hooks and does not weaken either fail-closed VAC
safeguard.

The current experiment changes memory resilience, observation, and the
implementation of the existing VAC join boundary:

- four staged address-space reserves (8 MiB, 8 MiB, 48 MiB, and 32 MiB),
  keeping separate contiguous large-request and failure-only blocks for
  content-heavy mods;
- staged release and re-scan before allocations of 8 MiB or larger, allowing
  the guard to release enough adjacent reserve space for the current request;
- protection of the 48 MiB block from ordinary small-allocation churn and the
  separate 32 MiB block from all preflight release, with one-stage retries and
  adaptive recovery-block re-arming after a successful retry;
- conservative primary-reserve re-arming after sustained recovery;
- 64-bit diagnostic counters;
- a per-thread caller cache and bounded lookup of existing sites when the
  fixed table is saturated, while global sample lifetime totals stay active;
- higher-level engine attribution for sampled retained allocations;
- sampled per-heap statistics and heap compatibility reporting;
- a fixed 256-entry allocation flight recorder;
- one-second address-space scans during critical pressure; and
- timestamped detailed logs with the newest ten sessions retained; and
- a continuously refreshed `DLL Crash Report.txt` containing the most useful
  session, memory, allocation, hotspot, recovery, and exception fields in no
  more than 900 ASCII bytes for direct use in one Steam comment.

Experimental.6 changes that final-reserve policy in response to a native
Windows RobZ report. The older policy released the 48 MiB block under general
emergency pressure; small allocations then consumed it before a later request
of roughly 33 MiB arrived. Experimental.6 holds the contiguous block until a
large request is known or a low-level allocation has actually failed. The
compact report also preserves the exact most recent allocation failure,
reports sub-1 MiB contiguous space in KiB, and uses `total=` instead of the
Steam-filtered `cum=` label.

Experimental.7 adds a game-only long-path compatibility layer for multiplayer
resource checking. File opens, enumeration, and attribute queries made with
paths of 248 characters or more are passed to Windows using absolute
extended-length paths. The detailed log records whether these rewrites succeed
without recording users' path text. This does not alter resource hashes or
network data, and it cannot recover a path already truncated inside the game.

Experimental.8 responds to a second native-Windows RobZ report. The
48 MiB final reserve had already been released for a large request; later,
a heap request just over 16 MiB failed with 55 MiB total VA free but only a
4 MiB largest hole. The process accounted for roughly 4,094 MiB of its x86
address space and no guard remained. Experimental.8 therefore adds a separate
32 MiB failure-only recovery reserve, releases and retries one stage at a time,
reduces large-request preflight margin from 16 MiB to 4 MiB so the recovery
stage stays protected, and adaptively re-reserves 8–32 MiB after a successful
recovery when a sufficiently large region remains. Reports now distinguish a
failed retry from a retry that was never attempted because no reserve remained.

The subsequently supplied full experimental.6 log refines that interpretation:
the final block was released about 3 minutes 19 seconds before the failure, and
a 64 MiB free region around its old address persisted for over a minute after
release. It does not prove that the largest observed 33 MiB request consumed
that block. Experimental.10 adds direct attribution and separate histories to
distinguish release, later reuse and fragmentation instead of inferring them
from widely spaced periodic snapshots.

The secure-join guard no longer overwrites the Proton-owned SteamUser018
vtable. It detours only the implementation already referenced by the
`InitiateGameConnection` entry. This preserves the same `bSecure` refusal and
fail-closed behavior without mutating the shared dispatch table implicated in
persona-name corruption and a Proton Steam IPC-thread crash.

The flight recorder, heap table, call-site table, and sample table are all
fixed-size static storage. They do not allocate memory while the game is under
pressure.

## Editor and utility compatibility

Starting with `2.5.0-experimental.5`, initialization is gated by the loading
executable. Only `mowas_2.exe` starts the memory guard, allocator hooks,
diagnostics, crash recorder, and fail-closed VAC safeguards. The map editor
(`mowas_2_ed.exe`), Workshop utility (`mowas_2_workshop.exe`), and unrelated
programs use proxy-only mode, which forwards the required `dbghelp` functions
without installing game-specific hooks or replacing the game's compact crash
report.

## Architecture compatibility

This experiment is still a **32-bit x86 DLL** for the 32-bit
`mowas_2.exe`. The phrase "64-bit diagnostic counters" describes only the
width of a few integer values used to count allocator activity. Those counters
use aligned x86 atomic operations; they do not introduce 64-bit pointers,
increase the game's address space, or turn the DLL into an x64 binary.

Consequently, the counter change does not by itself require a 64-bit operating
system. The DLL continues to run as 32-bit code on Windows and under WoW64 or
Proton WoW64. Native 32-bit Windows has not yet been tested with this
experimental build, so it should not be advertised as verified there until a
real-machine test is completed.

Digitalmindsoft's developer-marked explanation for not shipping a separate
64-bit AS2 executable was multiplayer community separation: its 32-bit and
64-bit clients would not have been compatible with one another. That concern
does not apply to these counters because this project does not create a
separate 64-bit game or protocol:

<https://steamcommunity.com/app/244450/discussions/0/1743356517529247789/#c1743356517529377814>

## VAC invariants

Every experimental DLL must preserve all of these properties:

1. Secure `SteamGameServer_Init` modes are rejected.
2. `SteamUser018::InitiateGameConnection` is rejected when `bSecure` is true,
   without modifying the SteamUser018 vtable.
3. Failure to install either guard closes the game.
4. Insecure hosting and joining continue to call the original Steam APIs.
5. SteamMatchmaking and persona/friend interfaces are not modified.

The experiment should be tested on the developer's Proton installation before
it is considered for another machine or a public release.
