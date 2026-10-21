# Experimental.10: allocation-failure evidence

Experimental.10 preserves experimental.9's memory policy and adds diagnostics
to answer when a reserve was released, what happened before the next failure,
and whether enough suitably aligned free address space remained.

## Why this experiment exists

The supplied [experimental.6 full session log](https://drive.google.com/file/d/1lDzxYAYukesTmvIVRBxfeyrozXOYuEcs/view)
identifies PID 105424 and build `Oct 19 2026 02:51:48`. Its SHA-256 is
`5112e234faa513cdf4674fff46b322eb9a91f47670ea6be075e417388d832dbb`.
Only selected non-identifying measurements are reproduced here; the original
full log includes local paths and should not be automatically republished.

The final 48 MiB guard was released at log time `02:34:30.412`. A 64 MiB free
region around its old address remained in subsequent snapshots through
`02:35:49.230`. At `02:37:49.800`, a 16,777,235-byte `HeapAlloc` failed with
no reserve available. The flight recorder shows a successful 10,801,501-byte
allocation on the same thread 78 tick milliseconds earlier, apparently in the
previously measured largest hole. The next periodic snapshot reports 55 MiB
free overall and a largest hole of 4 MiB.

This supports address exhaustion and fragmentation. It does not establish
that the largest observed request of about 33 MiB consumed the final reserve
immediately when it was released. That was a hypothesis from the compact
report; the full log gives a more complicated sequence. No experimental.9
RobZ result was available in this input.

## Reading the new records

### `MEM GUARD PREFLIGHT`

Emitted only after a large-request check actually releases a reserve stage.
It includes:

- `stage`: primary, critical or final;
- `bytes`: the exact engine request that triggered this check;
- `site`: executable-relative caller (`FFFFFFFF` means unknown/outside target);
- `thread` and `tick`: caller identity and monotonic Windows tick;
- `before_free`, `before_largest`, `after_free`, `after_largest`: KiB
  measurements around that release; and
- `complete`: completion flags for the before/after scans.

These are the allocation-thread records, separate from the older monitor's
`MEM GUARD RELEASED` notifications. The monitor can report later. Each actual
preflight release is attributed, including a primary reserve released again
after rearming. Ordinary successful small requests do not create these lines.

### `MEM FAILURE SNAPSHOT`

Memory hooks invoke a fresh address-space scan after observing a failure and
capturing the native error, before releasing any reserve or retrying. The
engine wrapper covers returned failures too. When actual retries occur,
another snapshot is captured after their result and before recovery rearming.

`phase=initial` is the pre-recovery state. `retry_ok` and `retry_failed` are
post-retry states. A failure with no eligible/available retry retains its
initial snapshot. Genuine resource-exhaustion errors still get evidence even
when releasing VA cannot help; ordinary invalid-parameter probes do not.

The detailed line includes API, exact bytes, caller, phase, thread, tick,
free/largest space in KiB, committed/reserved MiB, remaining guard MiB, fit
counts and scan completion. Measurements are fresh scans, not atomic
transactions: other game threads can allocate/free during a scan. `complete=0`
means the walk stopped early; do not treat that result as a complete map.

The compact report's `mem_fail` line pins the latest failure snapshot. Later
periodic `mem` updates continue in the detailed log but do not overwrite it.
It remains within the existing 900-byte ASCII limit; optional fields may be
omitted. A recovered session can therefore retain an older `retry_ok` snapshot
until the next failure or restart. Use its phase/tick and the detailed log.

### `MEM HOLES` and `fit4/8/16/32MiB`

These are overlapping counts of free regions large enough for one new
4, 8, 16 or 32 MiB reservation after rounding the start up to Windows allocation
granularity. A 32 MiB usable region contributes to all four counts. This is not
the number of independent allocations available: do not add the counts.

A raw free region can be page-aligned but not reservation-aligned. For example,
a raw region of exactly 4 MiB can fail the 4 MiB fit test if the next valid
reservation boundary falls inside it. Histogram calculations do not change
the existing raw largest-region value used by recovery policy.

Fit counts are evidence, not an allocation guarantee. Existing-heap commits,
fixed-address requests, system commit limits, and concurrent allocations can
have different requirements.

### `ALLOC LARGE HISTORY` and `ALLOC LARGE`

A separate static 32-entry ring retains requests of at least 1 MiB. The newest
16 entries are copied under a short lock and logged after releasing that lock,
on detailed summaries and allocation-failure reporting. Small pressure events
cannot evict this ring. Large traffic can still overwrite older large entries.

Records distinguish engine, heap, realloc and virtual calls. `direct` is the
caller of that recorded layer. `engine`/`engine_bytes` carry the enclosing
engine request when known. A heap request and its enclosing engine completion
are distinct records for one logical operation; do not sum them as separate
memory ownership. Native request flags and failure/recovered outcome flags are
also retained.

Entries are recorded after calls finish, including retries. The sequence is
recording/completion order, not request-start order across concurrent threads.
Use ticks, thread identity and context to reconstruct the relationship. A
fatal engine abort might never produce an engine-completion record; its
observed low-level failure and active context can still be present.

The existing 256-entry flight recorder remains separate and unchanged.

## Limits, performance and tests

Diagnostics use fixed storage and stack snapshots, preserve native error
state, and do not replace allocators or change valid request sizes. The guard
sizes, release thresholds, retry rules and rearm rules match experimental.9.
The exact lobby sentinel remains the only allocation-size substitution.

There is no disk write per recorded large request. New output occurs at actual
reserve releases, failures and the existing monitor/summary cadence. Ordinary
small allocations do not trigger extra VA scans. Failure scans inspect only
the current process's virtual address regions; no directory scanning, heap
walking, live-object freeing or automatic upload is added.

Tests exercise the production portable histogram and history helpers under
native sanitizers, including alignment edge cases, wraparound, a flood of
small events and concurrent producers/readers. Windows tests compile actual
production memory/allocator/engine/logger code with controlled delegates and
check release attribution, before/after-retry ordering, native errors, scan
completion and pinned compact evidence. Existing VAC/proxy/path tests remain
in the suite; the editor/Workshop executable gate is unchanged.

Run `bash tests/run-native.sh` and, for sanitizers,
`MOWAS2_TEST_SANITIZERS=1 bash tests/run-native.sh`. Windows uses
`tests/run-windows.ps1` with the pinned llvm-mingw toolchain. Real RobZ and
Steam/Proton sessions remain a field-validation requirement.

Publication validation: [GitHub run 39527019786](https://github.com/Igordalessandro/mowas2-memory-guard/actions/runs/39527019786)
passed all five portable suites under Linux sanitizers and all 13 Windows x86
regression executables. The published artifact is built with the pinned
`20261006` UCRT llvm-mingw compiler. Verify it using
`SHA256SUMS-v2.5.0-experimental.10.txt` from the release.
