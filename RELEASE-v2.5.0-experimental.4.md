# v2.5.0-experimental.4

This is a **prerelease test build**, not the stable v2.4.2 rollback build.

## Highlights

- Redistributes the existing 64 MiB guard into 8 MiB, 8 MiB, and a contiguous
  48 MiB final reserve for large content mods such as RobZ.
- Releases and rescans reserve stages before allocations of 8 MiB or larger,
  and proactively releases the final stage under emergency fragmentation.
- Stops expensive call-site probing after the fixed diagnostic table fills.
- Uses 64-bit diagnostic counters while remaining a 32-bit x86 DLL.
- Replaces the Proton-sensitive SteamUser vtable modification with a narrow
  `InitiateGameConnection` implementation detour.
- Preserves fail-closed refusal of VAC-secured hosting and joining.
- Creates `DLL Crash Report.txt`, a continuously refreshed ASCII report capped
  at 900 bytes for direct pasting into one Steam comment.

## Testing and reporting

Install `dbghelp.dll` beside `mowas_2.exe` using the normal Windows or Proton
instructions. After a crash, paste the entire `DLL Crash Report.txt` into the
Steam thread and state the active mods, last action, and whether a dialog
appeared. The longer timestamped log is still available for GitHub issues.

This build is experimental. It cannot remove the game's 32-bit address-space
limit or guarantee that every large mod will fit. Modifying a game process may
be incompatible with anti-cheat systems; the included guards refuse secure
sessions but cannot guarantee protection from account enforcement.
