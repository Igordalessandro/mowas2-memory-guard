# v2.5.0-experimental.7

This is a **prerelease test build**, not the stable v2.4.2 rollback build.

## Multiplayer long-path compatibility

This build adds a game-only compatibility layer for a reported multiplayer
resource-check failure when the path to game or mod content approaches or
exceeds the legacy Windows path limit. Single-player could still load the same
content, while creating a network game reported a resource mismatch.

The DLL now intercepts the file-open, file-enumeration, and file-attribute
calls imported by `mowas_2.exe`. Paths of 248 characters or more are converted
to absolute extended-length Windows paths before the original system function
is called. It does not change file contents, resource hashes, mod identifiers,
network packets, Steam matchmaking, or persona/friend interfaces.

The detailed session log records `LONG PATH` entries with the API, original
character count, rewrite status, result, and Windows error number. It does not
record the path itself. The compact `DLL Crash Report.txt` records the first
successful interception while remaining within its 900-byte limit.

This addresses the Win32 path boundary only. If the engine truncates a path in
its own fixed-size buffer before calling Windows, the compatibility layer
cannot reconstruct the missing text. In that case the report should reproduce
without any `LONG PATH ... rewritten=yes` entry, giving us a precise next lead.

Experimental.7 retains all experimental.6 RobZ memory-fragmentation changes,
the experimental.5 editor compatibility repair, and the fail-closed refusal of
VAC-secured hosting and joining in `mowas_2.exe`.

## Test request

Close the game and editor before replacing `dbghelp.dll`. Reproduce the issue
with the same long content path, then report whether the network game was
created. If it still fails, attach `DLL Crash Report.txt` and the newest
`mowas2-memory-guard-*.log` file.

This build remains experimental. Modifying a game process may be incompatible
with anti-cheat systems; the included guards refuse secure sessions but cannot
guarantee protection from account enforcement.
