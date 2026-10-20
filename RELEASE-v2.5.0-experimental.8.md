# v2.5.0-experimental.8

This is a **prerelease test build**, not the stable v2.4.2 rollback build.

## Second-chance reserve for heavy mods

A new native-Windows RobZ 1.28.6 report showed a different failure after the
experimental.6 policy had extended the session:

- the earlier maximum engine request was approximately 33 MiB;
- the 48 MiB final reserve had been released for that large request;
- a later heap backing request of 16,777,235 bytes failed;
- 55 MiB of virtual address space remained in total, but its largest contiguous
  region was only 4 MiB;
- all guard stages had already been released; and
- committed, reserved, and free regions accounted for roughly 4,094 MiB of the
  32-bit process address space.

This build changes recovery policy accordingly:

- adds a separate contiguous 32 MiB recovery reserve, increasing total
  uncommitted guard space from 64 MiB to 96 MiB;
- keeps that 32 MiB stage out of size-aware large-request preflight so an
  earlier approximately 33 MiB request cannot consume every protected block;
- reduces preflight headroom from 16 MiB to 4 MiB so the 48 MiB stage can serve
  the observed request without unnecessarily exposing the recovery stage;
- releases at most one reserve stage per attempt and retries the identical
  allocation after each release;
- if a recovery-stage retry succeeds, tries to reserve another 8–32 MiB from
  the largest remaining free region while leaving at least 8 MiB unreserved;
- prevents re-reservation until the releasing thread has confirmed a successful
  retry; and
- reports released MiB, retry count, and `not_attempted` when no reserve was
  available instead of calling that case a failed retry.

Experimental.8 retains the experimental.7 multiplayer long-path layer, the
experimental.5 editor compatibility repair, the exact lobby sentinel fix,
bounded allocator diagnostics, and fail-closed refusal of VAC-secured hosting
and joining.

## Validation before publication

- All changed translation units compiled for i686 with `-Wall -Wextra
  -Wconversion` without warnings.
- The complete DLL built successfully with the pinned llvm-mingw toolchain.
- The artifact is PE32/i386 and exports `ImageNtHeader` and
  `MiniDumpWriteDump`.
- Binary inspection confirms experimental.8, the adaptive recovery messages,
  the experimental.7 long-path module, and both VAC guard implementations.
- This reserve policy has not yet completed a real RobZ field session; that is
  the purpose of this prerelease.

## Expectations and limitations

This is intended to preserve a second contiguous allocation opportunity after
a large mod request consumes the first reserve. It cannot increase the x86
address-space ceiling, reclaim objects still owned by the engine or mod, or
guarantee that a 4x4 RobZ match at high unit limits and graphics settings will
fit indefinitely. Holding an additional uncommitted 32 MiB can also expose
previously hidden allocation-order behavior, which is why this remains an
experimental prerelease.

Close the game and editor before replacing `dbghelp.dll`. After testing, paste
the entire `DLL Crash Report.txt`. For a remaining crash, also attach the newest
`mowas2-memory-guard-*.log` and state the mods, map, player/unit settings,
graphics settings, duration, last action, and dialog text.

Modifying a game process may be incompatible with anti-cheat systems. The
included guards refuse secure sessions but cannot guarantee protection from
account enforcement.
