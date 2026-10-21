# Experimental.13 — lighter profiling, retained diagnostics

Prepared at the user's request on 2026-10-21. Branch:
`experimental/v2.5.0-experimental.13`. This is a local build, not yet installed
on either PC or published on GitHub. Stable 2.4.2 is unchanged.

## Contents

This version carries forward **all** additions from the unreleased
experimental.12-dev candidate. That candidate already implemented light
profiling; .13 gives the requested combination its own version and artifact,
without inventing a further allocator change or reducing recovery safeguards.

- Light request-site profiling is the default: approximately 1 in 1024
  requests contributes scaled caller-site estimates, clearly labelled
  `ALLOC TOP REQUEST EST` / `req_est`.
- Global allocation/free counts and requested-byte totals remain exact.
  Lifetime samples, retained-allocation samples, engine caller samples,
  failure snapshots and both allocation histories remain available.
- Fixed 64-entry per-thread caller caches retain both hits and saturated
  misses; advisory pressure/saturation flags use inexpensive atomic reads.
- Large-request preflight avoids a pointless full VA walk when all three
  preflight reserve stages are gone. Fresh failure scans and staged retries
  still run; the separate recovery reserve stays protected from preflight.
- Full session logs measure VA-scan calls, wall time, worst duration and query
  counts by category. They also record available physical RAM and process
  page-fault totals (soft plus hard faults, not proof of active paging).
- `DLL Crash Report.txt` keeps its existing 900-byte cap. Scan timing and
  extra system-memory information are in the timestamped full log instead.
- The experimental.11 Proton Steam-text workaround is retained; it remains
  inactive on native Windows. See its separate
  [implementation and limitations](PROTON-Steam-Text-Investigation.md).

Original allocator semantics, flags, valid request sizes, reserve sizes and
thresholds, recovery/rearm policy, long-path handling, editor/Workshop proxy
compatibility and secure-host/join refusals are unchanged from .12. The exact
known corrupt lobby request remains the only size substitution. VAC safeguards
refuse secure sessions; they do not disable VAC or guarantee account safety.

This is still **one x86 DLL**, not a 64-bit engine. It adds no automatic
game-resource purge, garbage collector or guarantee against memory exhaustion.
A recent good match does not establish that the DLL caused earlier slow motion
or that lowering diagnostic overhead fixes it. The
[investigation record](INVESTIGATION-2026-10-21-Lag-And-Mod-Crashes.md) separates
evidence from hypotheses about reload accumulation and duplicate mod versions.

## Selecting profiling mode

No new launch flag is required for light profiling. On startup, the full log
should contain `ALLOC PROFILER installed: mode=light request-site=1/1024`.

For diagnostic comparison, `MOWAS2_PROFILE_MODE=full` restores per-request
caller-site accounting. `MOWAS2_PROFILE_MODE=light` explicitly selects the
default. The setting is read once before hook installation; a restart is
required. Invalid or overlong values fall back to full with a warning.

On Proton, prepend the setting to existing Steam launch options and preserve
the existing Proton settings, `WINEDLLOVERRIDES` and `%command%`. On Windows,
set it in the environment inherited by the game; a running Steam client may
need restarting to inherit a changed variable. Do not replace unrelated launch
options, disable safety guards or install into a running game.

## Build and validation record

Artifact: `dist/v2.5.0-experimental.13/dbghelp.dll`, PE32/x86. It exports only
`ImageNtHeader` and `MiniDumpWriteDump` and embeds version
`2.5.0-experimental.13`. SHA-256:

`63c1604da8064ef3017705024fe273c5dc51f69c1a344864f64dddfc2bd071bd`

- All seven portable regression suites passed with ASan/UBSan.
- All sixteen Windows x86 regression executables were freshly compiled with
  strict warnings and passed under isolated Wine 11.19 (Staging). The tests
  do not start Steam or the game, or touch either installed game prefix.
- Source/test comparison with the .12 checkpoint confirms the version string
  is the only production-code difference: all .12 functionality is retained.
- `git diff --check` passed. The .12 artifact was preserved, and the local
  installed DLL still matches the previously deployed .11-dev artifact.

Native-Windows runtime and real gameplay/slow-motion validation remain
outstanding. Neither deployment nor GitHub publication is part of preparing
this local artifact. No live game session was interrupted.
