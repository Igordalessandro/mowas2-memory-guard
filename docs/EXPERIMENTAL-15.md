# Experimental.15 — evidence-first reporting and allocator preparation

Diagnostic candidate based on .14. Not a Fallout-style allocator replacement,
memory cleanup, graphics downgrade or increased address-space limit. Native
allocation/retry/reserve behavior, full profiling, VAC guards and editor policy
are unchanged. No game installation is changed by building or publishing it.

## Reports

`DLL Crash Report.txt` remains <=900 ASCII bytes. It now identifies `sid`,
`fmt=2`, effective `exp=off` and the matching full-log basename. Largest free
regions use KiB, including values below one MiB. Failure-time memory stays
pinned against later monitor updates. Sampled retained caller and coverage
precede optional configuration/cumulative traffic; optional omissions remain
marked. Requests and crash status retain their existing explicit semantics.

Full logs add matching session/environment/feature/coverage records. An
unavailable process query or total-RAM query is explicitly labeled; periodic
legacy full lines still contain zero placeholders and must be interpreted
with their availability record. No action phase, mod identity, true live total,
allocator slack or leak is automatically inferred.

`MOWAS2_EXPERIMENT` is a startup-only safety selector. Missing/empty/`off`
keeps it off; all other values (including multiple names or oversized values)
are refused as unvalidated. Effective experiment remains off in both reports.
No cleanup or allocator mode has passed its gate yet.

## Allocator viability evidence

- Twelve request-size buckets from below 16 B to at least 16 MiB. A separate
  per-thread 1/1024 sample counts allocation/reallocation **attempts**
  and requested bytes, excluding reserve retries. Full existing accounting
  stays 1/1; these additional samples do not enable light mode. Each block of
  1,024 attempts selects one varying pseudo-random offset, avoiding a fixed
  sampling phase against repeating workloads. Partial blocks can lack a
  sample; the generator is not cryptographic or a statistical confidence bound.
- Existing pointer-selected 1/1024 lifetime samples additionally retain the
  allocating thread and allocation flags. Retirement histograms count observed
  successful frees and successful reallocations; `reallocated` distinguishes
  resize retirements. Failed free/realloc restores observation metadata and
  does not count as retirement. Metadata loss is not a free.
- `same_thread`, `cross_thread`, modular `retired_ms`, and `flags_or` describe
  retired samples only. No payload reads, HeapSize calls or global API detours.
  Other-module frees and size queries are not audited. These measurements
  cannot authorize routing a pointer to another allocator.
- Request and retirement cohorts differ. Do not subtract their byte totals,
  infer live bytes, or assume unbiased estimates. Observed
  lifetime/thread estimates may have pointer-selection bias and table drops.
- Additional declared sample fields and shape counters total under 256 KiB;
  a compile-time budget assertion enforces it. The conditional 4,096-entry
  watched ledger is **not added** without a specific unanswered .14/.15
  question. Existing sampled metadata already supplies bounded lifetimes.

`ALLOC SHAPE`, `ALLOC LIFECYCLE` and `ALLOCATOR ASSESSMENT` use existing
summary/failure callbacks: no per-request disk writes or dynamic containers.
New routine output is a fixed fourteen lines per summary (~30 seconds normally),
in addition to existing diagnostics. Failure callbacks can trigger summaries;
no zero-overhead or total file-size guarantee is claimed.

Periodic memory snapshots additionally record process CPU time deltas.
`core_pct` uses one fully occupied CPU core as 100%, so multithreaded usage
can exceed 100%. It is not per-core load, GPU time, frame time or proof of
the DLL's cost. First/failed measurements say `unknown`, not zero. Process
memory/page-fault availability is explicit; fault totals still include soft
and hard faults and cannot establish paging by themselves.

## Offline analysis

Run a short-lived command, outside the game:

```sh
python3 tools/analyze_logs.py "selected-full.log"
python3 tools/analyze_logs.py --format json "baseline.log" "candidate.log"
```

Python 3 standard library only, no installed packages, service or upload.
Windows uses the same command where Python is installed (`py -3` also works).
Only explicitly selected regular files are read. Each read is a bounded
snapshot (default 32 MiB; `--max-input-mib` accepts 1–256); histories retain the
latest 256 memory/timeline and 64 failure records with omission warnings.
Output goes to stdout; nothing is saved automatically. Sanitized fixtures
cover legacy, .14-age/.15 evidence, compact, partial and unknown input.

JSON includes provenance SHA-256, timeline, sampled retention changes per
caller, memory trends, size/lifecycle observations, scan costs, hook coverage,
explicit conclusions and unknowns. Markdown is a human triage view. All
current assessments conservatively remain `insufficient_evidence`, with
opportunity hints and `integration_safety=not_audited`. The tool has no
controlled allocator-comparison or ownership evidence that could certify a
replacement. Matching metadata is not a matched gameplay experiment.

Share the compact file for Steam triage and the matching full log for detail;
review full logs for private information before uploading. Mods, last action
and any dialog remain useful voluntary context. Old reports remain supported
but cannot supply measurements that their DLL never recorded.

See the [roadmap](EVIDENCE-FIRST-ROADMAP.md) and
[evidence register](EVIDENCE-REGISTER.md) for advancement gates. No raw
community report is committed. Stable public Latest remains v2.4.2.

## Local validation and artifact

- Nine portable suites passed under ASan/UBSan; ten offline analyzer tests
  passed, including historical/truncated reports, correlation conflict,
  unavailable queries and bounded histories.
- Eighteen strict-warning Windows x86 regression executables ran successfully
  under isolated Wine 11.19. Tests include actual file report matching,
  unsupported experiment refusal, unchanged recovery/error behavior, sampler
  boundaries, retired-thread evidence and CPU baseline/unavailable/unit cases.
- A maximum-counter formatting test measured 3,283 bytes of new allocator
  evidence per summary (timestamp allowance included), below its 4 KiB test
  budget. This excludes inherited diagnostics and repeated failure callbacks.
- A one-million-request component benchmark had ~13–15 ms baseline and
  ~13–14 ms with shape observation in one isolated Wine run. Noise overlaps;
  this is not an overhead bound, a native gameplay benchmark or an FPS claim.
- Artifact: `dist/v2.5.0-experimental.15/dbghelp.dll`, PE32/i386, 392,192 bytes,
  exporting only `ImageNtHeader` and `MiniDumpWriteDump`. SHA-256:
  `80a945a320b4612718ad5cc9c61f96033fc0442f981e9b5900784b1a9af678e6`.

Native Windows CI is still the publication gate at this local checkpoint.
Neither PC has been updated or launched for this work.
