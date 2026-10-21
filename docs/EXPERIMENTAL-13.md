# Experimental.13 — full profiling and expanded diagnostics

Corrected at the user's request on 2026-10-21. Branch:
`experimental/v2.5.0-experimental.13`. Building and publishing did not change
either installed game; the later requested deployment is recorded below.
Publication was subsequently requested; see the
[GitHub prerelease](https://github.com/Igordalessandro/mowas2-memory-guard/releases/tag/v2.5.0-experimental.13)
for live publication status and attached assets. Stable 2.4.2 is unchanged.

## Contents

The first local .13 candidate inherited light profiling from .12. The user
subsequently clarified that this was a mistake. The corrected .13 removes
the request-site sampler, weighted estimates, mode selector and light/full
benchmark, while retaining the other .12 additions and recovery safeguards.

- Every observed heap allocation/reallocation request contributes its actual
  count and byte size to its registered caller site, not a scaled estimate.
  Rankings use `ALLOC TOP REQUEST` and compact `hot req`, not `EST` / `req_est`.
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

## Profiling behavior

Full per-request site accounting is always enabled. The startup log identifies
`ALLOC PROFILER installed: mode=full request-site=1/1`; periodic profiles
identify `mode=full request_site_rate=1/1`. No new launch flag is required.

`MOWAS2_PROFILE_MODE` is no longer read. Even a previously configured `light`
value cannot enable light profiling. Existing Proton settings, the dbghelp
override and `%command%` must be preserved; no installed launch options were
changed during this correction.

"Full" restores per-request site accounting, not unlimited tracking of every
pointer. The pre-existing lifetime/free-site sampling and engine sampling are
unchanged. The fixed site table can still saturate, making per-site coverage
incomplete; global totals continue independently. Caching and advisory atomic
read optimizations do not sample away accounting.

## Build and validation record

Corrected artifact: `dist/v2.5.0-experimental.13/dbghelp.dll`, PE32/x86,
exporting only `ImageNtHeader` and `MiniDumpWriteDump`. SHA-256:

`f4d2ebab75e63943a751a9a828f83c6b07c37a3e886cb03df7e08c5445482fcb`

- All seven portable regression suites passed with ASan/UBSan.
- All sixteen freshly compiled Windows x86 regression executables passed
  under isolated Wine 11.19 (Staging), with strict compilation warnings.
- Regressions check 131,072 consecutive requests are counted exactly, request
  bytes accumulate beyond 32 bits, retries do not double-count requests, and
  request-site output no longer carries scaled-estimate labels.
- Binary inspection confirms the full-mode startup marker and the absence
  of the removed mode-setting name, `mode=light` and `req_est`.
- `git diff --check` passed. Recovery, VAC, compatibility and orchestration
  source files are unchanged by this correction. The isolated temporary Wine
  prefix was cleaned up after its own server exited.

The earlier light-mode artifact (SHA-256
`63c1604da8064ef3017705024fe273c5dc51f69c1a344864f64dddfc2bd071bd`)
was preserved at `dist/v2.5.0-experimental.13-light-superseded/dbghelp.dll`.
It is superseded, not the version to deploy. No public release ever used it.

The above Windows-test executions were local Wine runs; native-Windows
regression status is recorded in the source CI checks linked from the release.
Real gameplay/slow-motion validation remains outstanding. Publication does
not deploy the DLL to either PC. No live game session was interrupted.

### Verified publication

Published on 2026-10-21 as `v2.5.0-experimental.13`, a non-draft prerelease at
source commit `2bc2975bb555e572343407d4adf773649a3e5540`. The
[GitHub regression run](https://github.com/Igordalessandro/mowas2-memory-guard/actions/runs/39560205475)
passed seven Linux sanitizer suites and all sixteen regression executables on
native Windows x86. Both draft assets were downloaded and byte-compared with
the local DLL/checksum before publication. Published asset digests match the
corrected full-profiling artifact. The public tag resolves to the tested source
commit, stable GitHub Latest remains v2.4.2, and neither installed game had
been updated at publication. This is a documentation-only follow-up to the tag.

### Subsequent deployment to both PCs

At the user's request on 2026-10-21, the corrected full-profiling .13 DLL was
installed on the local Proton PC and Tety's native-Windows PC, with both games
and editors closed. Both installed hashes matched
`f4d2ebab75e63943a751a9a828f83c6b07c37a3e886cb03df7e08c5445482fcb`.
Verified .11-dev rollback copies were retained on each PC, matching
`09ed0886d5898fe90c9eb09a45ced06aab8e9de0b59e16bc0b3858100996bd45`.
The local native/builtin dbghelp override was preserved. Tety's deployment was
silent; neither game was launched. Installation verification is not gameplay
or UI confirmation. No game executable, mod, launch option or VAC policy was
changed.
