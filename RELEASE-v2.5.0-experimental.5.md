# v2.5.0-experimental.5

This is a **prerelease test build**, not the stable v2.4.2 rollback build.

## Editor compatibility repair

- Fixes `mowas_2_ed.exe` being closed by the game-only fail-closed VAC guard.
- Runs memory hooks, reserves, diagnostics, the crash recorder, and VAC guards
  only inside the supported `mowas_2.exe` process.
- Keeps `mowas_2_ed.exe`, `mowas_2_workshop.exe`, and unrelated executables in
  transparent proxy-only mode.
- Prevents editor and utility launches from overwriting the game's
  `DLL Crash Report.txt`.
- Preserves all memory recovery, large-mod diagnostics, 900-byte Steam-ready
  crash reporting, and fail-closed VAC behavior from experimental.4 when the
  actual game is running.

## Testing and reporting

Close the game and editor, then replace the existing `dbghelp.dll` beside
`mowas_2.exe`. Test both the editor and the normal game. After a game crash,
paste the entire `DLL Crash Report.txt` into the Steam thread and state the
active mods, last action, and whether a dialog appeared.

This build remains experimental. It cannot remove the game's 32-bit
address-space limit or guarantee that every large mod will fit. Modifying a
game process may be incompatible with anti-cheat systems; the included guards
refuse secure sessions but cannot guarantee protection from account
enforcement.
