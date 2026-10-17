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

## VAC invariants

Every experimental DLL must preserve all of these properties:

1. Secure `SteamGameServer_Init` modes are rejected.
2. `SteamUser018::InitiateGameConnection` is rejected when `bSecure` is true.
3. Failure to install either guard closes the game.
4. Insecure hosting and joining continue to call the original Steam APIs.
5. SteamMatchmaking and persona/friend interfaces are not modified.

The experiment should be tested on the developer's Proton installation before
it is considered for another machine or a public release.
