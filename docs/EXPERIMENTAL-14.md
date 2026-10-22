# Experimental.14 — targeted retention and caller evidence

This diagnostic experiment follows the RobZ .13 report investigated on
2026-10-22. It preserves .13's **full per-request profiling**, native allocator
semantics, reserve/retry/rearm policy, compatibility layers and fail-closed
VAC-secured host/join refusals. There is no light-mode option, automatic
resource purge, mod rewrite or increase to the game's address-space ceiling.

## Why another diagnostic build

The reported .13 log is `mowas2-memory-guard-2026-10-21-221514-388-p41204.log`,
SHA-256 `59e78f054e34bdd5b521fb1702c80f26db433e4670a512e351b351db4ec68719`.
The raw report is retained privately outside Git, not republished with its
personal paths. DLL initialization succeeded on native Windows. Private bytes
rose from 1170 to 3417 MiB over about 32 seconds. Four allocation recoveries
released 48/32/24/8 MiB within roughly 1.4 seconds before the terminal observed
699,211-byte heap failure. Its fresh scan found 35.3 MiB free overall, only
480 KiB in the largest free region, and no reserve remaining. The final system
snapshot still had 14,079 MiB of physical RAM available.

The repeated retained caller `+003749C9` accounted for 1594 KiB in 45 live
samples, scaled to an uncertain 1594 MiB estimate. Other observed request sizes
match common compressed texture mipmap sizes: 1,398,128 bytes is a complete
1024-square BC3 payload; 2,796,216 bytes is a complete 2048-square BC1 payload.
A 1024-square BC1 mip chain plus a legacy DDS header is 699,192 bytes; the
observed engine wrapper often adds 19 bytes for the native heap request.
The calculation follows Microsoft's
[DDS layout](https://learn.microsoft.com/en-us/windows/win32/direct3ddds/dds-file-layout-for-textures).
These are size fingerprints, **not identified texture contents or proof of a
leak**. The final engine input previously had to be inferred because the
large-allocation history excluded requests below 1 MiB.

## New full-log records

### `ALLOC RETAINED AGE` and `ALLOC WATCH AGE`

Every existing 30-second allocator summary, and every failure-triggered
summary, takes a bounded snapshot of the existing lifetime-sample metadata.
It does not dereference allocation pointers or read payloads. It reports the
global sample cohort and five explicitly watched executable-relative callers:

`003749C9`, `0032762D`, `0004105D`, `002A89C1`, `00374A97`.

These addresses are observation filters, not new hook sites or established
semantic function names. Their global site-table entries are registered before
hook publication so later registration saturation does not exclude them.

- `snap` joins records belonging to one metadata snapshot; `tick` is the
  modular Windows tick and `span_ms` is time since the previous age snapshot.
- `sample_blocks` and `sample_B` are the actual retained sample count/bytes,
  **not full-game totals or already multiplied estimates**.
- `delta_B` is signed sampled-byte change since the previous snapshot. The
  first snapshot is `baseline=1`, with zero span and zero deltas.
- `oldest_ms` is the oldest currently observed sample, not the average age of
  allocations already freed.
- `age_B_lt1s/lt10s/lt60s/ge60s` gives four disjoint byte buckets: ages
  0–999, 1000–9999, 10,000–59,999 and at least 60,000 milliseconds. They sum to
  that watched site's `sample_B`.
- `metadata_wall_ms` includes waiting for the short sample lock and metadata
  scanning, but not subsequent logging. It is coarse wall time, not CPU time.

The cohort remains the existing pointer-selected 1/1024 lifetime sample. It
can be noisy, incomplete or affected by unobserved frees and saturation; a
change is not an exact leak measurement. Detached in-flight realloc/free
records may temporarily be absent. Snapshots are not atomic with all process
memory activity. Sequence numbers help interpret interleaved log lines.
Tick ages wrap after about 49 days; ordinary tick-boundary crossing is handled
modularly, not an unlimited lifetime guarantee.

### `ALLOC WATCH TRACE`

At most one successful native request of at least 64 KiB per watched caller,
plus the first eight observed failure callbacks, captures a bounded stack.
There are thirteen immutable slots for the whole process session and at most
twelve game-image frame RVAs per slot. Existing full request-site accounting
is not sampled down. Failure traces and successful traces occupy separate
slots, so successful traffic cannot evict failure evidence.

Records carry API/kind, native and active engine request sizes, direct and
engine caller RVAs, thread, tick, request flags, filtered frame RVAs and
`capture_wall_ms`. `engine=FFFFFFFF` / `engine_bytes=0` means no active engine
context was observed. For an engine-only failure trace, `native_bytes=0` means
not applicable. Missing unwind frames are explicitly `unavailable`, not guessed.

Each trace is logged once on a summary, after release-publication. No logger
call or OS memory read occurs while the sample lock is held. Once the eight
failure-stack slots are consumed, ordinary failure snapshots and histories
continue normally; the cap limits stack capture, not recovery or reporting.

### `ALLOC WATCH CODE`

The first logged trace from each watched caller may also read **64 bytes of
the loaded game executable image**, starting 16 bytes before its return RVA.
This provides a small runtime instruction window where the on-disk executable
may still be SteamStub-encrypted. It is bounded to five windows / 320 bytes
of machine code for the session, not a module dump or reverse-engineered
function identity. Bounds are checked before a read; unreadable windows are
reported and left unchanged. Windows are read at logging time, not at the
original allocation tick; readable bytes are not guaranteed to be plaintext
instructions if the executable re-encrypts or otherwise changes its image.

The reader uses this process's own handle. It never reads another process,
new malloc buffers, resource contents, textures or arbitrary user memory, and
does not scan files or directories. RVAs/windows are diagnostic observations;
no new patches are applied at these addresses. Game-relative instruction
snippets are included in the full log, so review the log before sharing it.

### Small failures retained in `ALLOC LARGE`

The existing fixed 32-entry history now admits actual failures below 1 MiB,
including those recovered by a retry. `includes_small_failures=yes` announces
the exception to its normal size filter. Ordinary successful small requests
and small pressure events still cannot flood it. At most the newest sixteen
entries are printed per detailed summary, as before.

This retains exact native/active-engine sizes even if the engine aborts before
an engine-completion callback. A native record and enclosing engine record can
describe one logical request; do not sum them as separate resource ownership.
Frames and code do not establish whether a retained buffer is redundant or
safe to free.

## Budget, compatibility and reporting

New static diagnostic state is compile-time capped below 4 KiB, with fixed
stack snapshots and no dynamic containers. Additional metadata passes are
over the existing 16,384 sample entries, not over game objects, payloads or the
filesystem. Stack capture and bounded code reads have costs, measured in the
new records; this is not a zero-overhead or FPS-improvement claim.

The DLL remains one PE32/x86 file. `DLL Crash Report.txt` keeps its hard
900-byte ceiling; the new evidence is in the timestamped full log. Nothing
uploads automatically. VAC refusals are unchanged, do not disable VAC and
cannot guarantee account safety. Native Windows still skips the inherited
Proton-only Steam-text workaround. Editor/Workshop proxy-only behavior remains.

For a useful field test, start a fresh process and record the times of the
first load, return to menu and identical reloads. Keep map/save, mods and
graphics settings constant, then make separate one-variable comparisons
(for example texture quality, or .13 versus .14). The DLL does not infer menu
or load phases automatically. Supply the compact report **and matching full
log**, any dialog, mod versions and those action times. Review full logs for
private information. Do not install into a running game/editor.

## Validation and artifact

Artifact: `dist/v2.5.0-experimental.14/dbghelp.dll`, PE32/x86, exporting only
`ImageNtHeader` and `MiniDumpWriteDump`. SHA-256:

`73efe4215d9f99ed27ae80d1d1e3fe645ba8a7dc5c86d17c3e3868c0fa7152c0`

- Eight portable suites passed under ASan/UBSan.
- Seventeen Windows x86 regression executables were freshly compiled with
  strict warnings and executed successfully under isolated Wine 11.19.
- Tests cover exact small-failure context, no history flooding by small
  successful requests, 64-bit sums, age boundaries/tick wrap, declining cohorts,
  saturation survival, trace caps and one-time publication, error preservation,
  absent engine context, code-window bounds/read failures and an actual bounded
  read of the test executable's own code. No game is needed for these tests.
- New static state is compile-time capped below 4 KiB. The compact-report
  900-byte tests and existing VAC/compatibility regressions remain active.
- Native Windows CI is a publication gate, not a claim of native-gameplay
  validation. Existing game installations are untouched by these tests.

Publishing a prerelease does not install it on either PC; game deployment
requires a separate request. Stable GitHub Latest remains v2.4.2.

### Verified publication

Published on 2026-10-22 as the non-draft
[v2.5.0-experimental.14 prerelease](https://github.com/Igordalessandro/mowas2-memory-guard/releases/tag/v2.5.0-experimental.14),
tagged at source commit `c3a8c672a02a12c1a77e28033f65493b663dfa85`.
[GitHub regression run 39809188548](https://github.com/Igordalessandro/mowas2-memory-guard/actions/runs/39809188548)
passed all eight Linux sanitizer suites and all seventeen regression
executables on native Windows x86. Both uploaded assets were downloaded and
byte-compared with the local DLL/checksum before publication. The public DLL
digest matches the hash above, the tag resolves to the tested source commit,
and stable Latest remains v2.4.2. Neither installed game was updated as part of
this release. This publication record is a documentation-only follow-up.
