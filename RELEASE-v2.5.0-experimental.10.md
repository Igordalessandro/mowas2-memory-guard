# v2.5.0-experimental.10

Experimental.10 adds focused diagnostics for heavy-mod allocation failures.
Its memory recovery policy remains experimental.9's; stable GitHub Latest
stays on **v2.4.2**. This remains a **prerelease**.

## New evidence for the next memory fix

- **Reserve-release attribution:** large-request preflight logs the exact
  request size, game-relative caller, thread, tick and reserve stage, with
  before/after free-space measurements.
- **Fresh failure snapshots:** allocation failure paths scan address space
  before releasing reserves and again after actual retries. These measurements
  distinguish `initial`, `retry_ok` and `retry_failed` states.
- **Persistent compact evidence:** the 900-byte `DLL Crash Report.txt` pins
  its `mem_fail` snapshot so later periodic updates cannot replace it. The
  detailed log keeps the complete evidence and the compact report prioritizes
  the essentials when optional fields do not fit.
- **Separate large-allocation history:** a fixed 32-entry history keeps large
  requests apart from small-allocation pressure traffic. Summaries dump the
  newest 16 entries with request source, native and engine context, result,
  flags, heap, thread and tick. Recording does not write to disk per request.
- **Useful free-hole counts:** snapshots count regions that can fit
  4/8/16/32 MiB reservations after allocation-granularity alignment. Total free
  space alone cannot answer whether a particular request can fit.

These additions follow the full experimental.6/PID 105424 report. It shows the
last reserve released about 3 minutes 19 seconds before a 16 MiB heap request
failed. A 64 MiB free region persisted after release, so the earlier assumption
that one 33 MiB request immediately consumed the reserve was not established.
The new records are designed to expose that missing sequence directly.

## What this build preserves

The 8/8/48/32 MiB reserves, release thresholds, retry semantics, recovery
rearming, exact lobby sentinel repair, long-path support, editor/Workshop
isolation, and VAC-secured hosting/joining refusal remain in place. The game
and DLL remain 32-bit, and this does not raise the address-space ceiling or
guarantee RobZ match stability. The VAC safeguards do not disable VAC or
guarantee protection from account action.

## Installation and useful reports

Close the game and editor, then replace `dbghelp.dll` beside `mowas_2.exe`.
This remains a single-DLL installation. Proton users must retain the
native-first `dbghelp` override (`native,builtin`) in Protontricks/Wine settings
or their existing `WINEDLLOVERRIDES` launch configuration.

For a remaining crash, provide the complete `DLL Crash Report.txt` plus the
matching `mowas2-memory-guard-*.log`, mods, map/unit/player settings, graphics
settings, duration and dialog text. If the game was restarted, use
`DLL Crash Report.previous.txt` for the previous session. Include the emergency
report if present.

The full log matters for this experiment: a Steam comment can carry the compact
failure summary, while the attribution and allocation history remain in the
timestamped log. No reports are uploaded automatically.

See [the diagnostic guide](https://github.com/Igordalessandro/mowas2-memory-guard/blob/v2.5.0-experimental.10/docs/EXPERIMENTAL-10-Diagnostics.md)
for field interpretation and validation details.

## Validation

Five portable regression suites passed normally and under AddressSanitizer and
UndefinedBehaviorSanitizer. All 13 Windows regression executables cross-compiled
with warnings treated as errors, and the four changed DLL translation units
compiled with `-Wall -Wextra -Wconversion -Werror`. The complete release DLL
built as PE32/i386 and exports `ImageNtHeader` and `MiniDumpWriteDump`.

The [GitHub regression run](https://github.com/Igordalessandro/mowas2-memory-guard/actions/runs/39527019786)
passed all five portable suites under Linux sanitizers and executed all 13
regression executables successfully on native Windows x86.

The new tests cover alignment-aware fit counts, scan completion, preflight
release attribution including a rearmed primary, failure-snapshot ordering,
pinned compact memory evidence, large-history wrap/flood/concurrency, and
native error preservation. They use production code with controlled delegates;
real RobZ and Steam/Proton sessions remain the field-validation step.
