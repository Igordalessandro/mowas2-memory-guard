# v2.5 allocator-diagnostics experiment

This branch starts from the published v2.4.2 stable rollback build. It does
not add SteamMatchmaking hooks and does not weaken either fail-closed VAC
safeguard.

The first experiment changes only memory resilience and observation:

- three staged address-space reserves (32 MiB, 16 MiB, and 16 MiB);
- conservative primary-reserve re-arming after sustained recovery;
- 64-bit diagnostic counters;
- higher-level engine attribution for sampled retained allocations;
- sampled per-heap statistics and heap compatibility reporting;
- a fixed 256-entry allocation flight recorder;
- one-second address-space scans during critical pressure; and
- timestamped logs with the newest ten sessions retained.

The flight recorder, heap table, call-site table, and sample table are all
fixed-size static storage. They do not allocate memory while the game is under
pressure.

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
2. `SteamUser018::InitiateGameConnection` is rejected when `bSecure` is true.
3. Failure to install either guard closes the game.
4. Insecure hosting and joining continue to call the original Steam APIs.
5. SteamMatchmaking and persona/friend interfaces are not modified.

The experiment should be tested on the developer's Proton installation before
it is considered for another machine or a public release.
