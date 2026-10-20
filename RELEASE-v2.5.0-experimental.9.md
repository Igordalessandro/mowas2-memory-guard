# v2.5.0-experimental.9

Experimental.9 repairs issues found during a review of experimental.8. It
remains a **prerelease**; stable GitHub Latest stays on **v2.4.2**.

## Memory recovery and observation

- Fixes a race where rebuilding the recovery reserve could erase another
  thread's pending retry state.
- Preserves reserve space when a failed `VirtualAlloc` cannot benefit from
  releasing it, such as invalid parameters or commits inside an existing range.
  Genuine resource-exhaustion failures still enter the report even when no
  reserve retry is applicable.
- Preserves native error codes across allocator telemetry and retries.
- Fixes sampled free/realloc address-reuse tracking, restores samples when the
  native operation fails, and maintains global sampled-live totals independently
  of call-site table saturation.
- Keeps existing hot call sites active with a per-thread cache and bounded
  saturated-table lookup.
- Keeps native targets callable by threads already inside a wrapper during
  import-hook rollback.

Reserve sizes and thresholds remain experimental.8's 8/8/48/32 MiB policy.
The game and DLL remain 32-bit; this does not increase their address-space
ceiling or guarantee that a large RobZ match fits indefinitely.

## Compatibility and diagnostics

- Fixes short relative paths in long working directories and root-relative
  paths under UNC shares. All seven file wrappers preserve native error codes
  after cleanup, including successful `CreateFile` status values.
- Limits repeated long-path failure logging to avoid unnecessary disk I/O.
- Preserves the previous compact crash report as
  `DLL Crash Report.previous.txt` before starting a new session.
- Keeps compact reports within 900 ASCII bytes using complete lines and
  prioritizes session, crash, allocation failure and memory evidence.
  `omitted=optional_fields` means the full session log has additional details.
- Writes a small `DLL Emergency Crash.txt` before normal exception formatting;
  a busy logger cannot prevent chaining the game's exception handler. Its
  previous session is saved as `DLL Emergency Crash.previous.txt`.
- Loads the real system dbghelp forwarding targets early, before memory
  pressure, and verifies an existing locked temporary copy before reusing it.

Editor/Workshop proxy-only compatibility, the exact lobby sentinel workaround,
the long-path layer, and VAC-secured hosting/joining refusal are retained.
The safeguards do not disable VAC or guarantee protection from account action.

## Validation and remaining field work

The three portable regression suites passed, including AddressSanitizer and
UndefinedBehaviorSanitizer runs. All nine regression executables cross-compiled
warning-free for i686, and changed DLL sources compiled with warnings treated
as errors. The final DLL is PE32/i386 with the two expected forwarding exports.

The regression tests exercise production recovery, allocator, path, crash,
logger, proxy-comparison and VAC policy code using controlled substitutes.
They do not prove live Steam/Proton interoperability or RobZ match stability.
The [GitHub regression run](https://github.com/Igordalessandro/mowas2-memory-guard/actions/runs/39387404224)
passed on Linux and executed all nine regression executables successfully on
native Windows x86. A real RobZ session is still required for field validation.

Full findings and future investigation priorities are in
[`docs/REVIEW-2026-10-20.md`](https://github.com/Igordalessandro/mowas2-memory-guard/blob/v2.5.0-experimental.9/docs/REVIEW-2026-10-20.md).

## Installation and reporting

Close the game and editor before replacing `dbghelp.dll` beside the game
executable. This remains a single-DLL installation. On Proton, retain the
native-first `dbghelp` override (`native,builtin`) in Protontricks/Wine settings
or the existing `WINEDLLOVERRIDES` launch configuration.

After a crash, paste `DLL Crash Report.txt` into the Steam thread and include
mods, map, player/unit settings, graphics settings, duration and dialog text.
If you already restarted, use `DLL Crash Report.previous.txt`. Include the
emergency report when present, and attach the matching full timestamped log
for deeper investigation.
