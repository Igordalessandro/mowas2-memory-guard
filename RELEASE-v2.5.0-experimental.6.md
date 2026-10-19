# v2.5.0-experimental.6

This is a **prerelease test build**, not the stable v2.4.2 rollback build.

## RobZ fragmentation mitigation

A native-Windows RobZ 1.28.6 report showed all 64 MiB of guard space already
released, only 37 MiB total virtual address space free, a largest free region
smaller than 1 MiB, and a maximum observed request of roughly 33 MiB. The
process had reached the 4 GB address-space ceiling despite ample physical RAM.

This build therefore:

- protects the final contiguous 48 MiB reserve from ordinary small-allocation
  churn;
- releases that block only when a size-aware large request needs it or an
  allocation has failed and will be retried;
- preserves the exact latest allocation failure in a dedicated compact-report
  field so later reserve events cannot overwrite it;
- reports a largest free region below 1 MiB in KiB rather than as `0MiB`; and
- renames the compact `cum=` field to `total=` because Steam's text filter
  obscured the old label.

The DLL remains capped to a 900-byte `DLL Crash Report.txt`, retains the
experimental.5 editor compatibility repair, and preserves fail-closed refusal
of VAC-secured hosting and joining in `mowas_2.exe`.

## Expectations

This policy may preserve a usable block for the observed 33 MiB request and
extend heavy-mod sessions. It cannot expand the 32-bit 4 GB address space, and
it cannot guarantee that a 4x4 RobZ battle with large unit limits will fit.

Close the game and editor before replacing `dbghelp.dll`. After a crash, paste
the entire `DLL Crash Report.txt` and state the active mods, map, player/unit
settings, last action, and whether a dialog appeared.

This build remains experimental. Modifying a game process may be incompatible
with anti-cheat systems; the included guards refuse secure sessions but cannot
guarantee protection from account enforcement.
