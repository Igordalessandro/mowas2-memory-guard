# Evidence-first implementation roadmap

Approved direction, 2026-10-22. Work advances on evidence, not deadlines tied
to volunteers uploading logs. No visual-quality reductions, gameplay changes,
whole-engine source reconstruction, native x64 conversion or helper process.
Bounded instruction inspection and targeted function analysis are permitted.

## Milestone 1: actionable community evidence

Experimental.15 is diagnostic-only, based on .14. Keep the single x86 DLL,
full request accounting, native allocator semantics, reserve/retry policy,
editor proxy behavior and fail-closed VAC host/join refusals. The refusals do
not disable VAC or guarantee account safety. Stable Latest stays v2.4.2.

- Match compact and full reports using `sid`, full-log basename, build and
  format version. Keep `DLL Crash Report.txt` ASCII and <=900 bytes.
- Prioritize session, captured crash/failure, pinned failure-time VA, retained
  sample/coverage and correlation over configuration and cumulative traffic.
- Keep full histories and make unknowns and estimates explicit. No automatic
  inference of menu/load phases, leaks, clean exit, or crash from log ending.
- Add `tools/analyze_logs.py`: explicitly selected files, no recursive search,
  background worker, dependency installation or network access. Output JSON
  or Markdown to stdout. Accept historical and truncated formats.
- Add size-distribution and lifecycle evidence for a Fallout-style allocator
  study. Preserve all existing full profiling; additional periodic sampling
  is labeled and is not a statistically unbiased sample guarantee.

Exit: old/new fixtures pass; historical reports agree with previous manual
findings; correlation and byte-budget tests pass; Windows x86 tests pass.
Compiler success and Wine tests are not native-Windows gameplay validation.

## Milestone 2: close specific evidence gaps

Maintain the [evidence register](EVIDENCE-REGISTER.md). For each hypothesis,
record observations, alternatives, missing evidence, next experiment and
status: investigating, ready to test, parked or validated. Preserve original
reports privately outside Git; use hashes and sanitized fixtures publicly.

Preferred voluntary reproduction: same save/map/settings, fresh process,
first load, return to menu, identical reloads. Record action times when
possible; absent times remain unknown. Casual sessions remain useful but do
not establish controlled treatment effects. Diagnostic files must be useful
without requiring the reporter to reproduce a crash a second time.

Only add new observation code to answer a named unanswered question. A future
watched-allocation ledger, if justified by .14/.15 evidence, is limited to
the existing watched callers, requests >=64 KiB, 4,096 entries and <=256 KiB
additional state. No payload reads, unbounded containers or per-call disk
logs. Saturation or unobserved frees mean incomplete coverage, not a leak.

## Milestone 3: individual behavioral experiments

| Candidate | Required gate | Safe boundary |
| --- | --- | --- |
| Heap-cache optimization | Isolated native tests show useful VA/commit changes with acceptable latency | Verified heaps; decommit alone is not a VA fix; unsupported runtime stays unchanged |
| Graphics cleanup | Verified device/lifecycle boundary and evidence of delayed destruction | No periodic mid-match purge, forced COM releases or guessed lifecycle hooks |
| Targeted allocator | Waste/placement opportunity plus allocation/free/resize/size-query ownership audit | One category first; preserve flags, alignment, concurrency, realloc failures and old-pointer routing |
| Resource/cache release | Identified owner and valid release/reload conditions | Use the owner's lifecycle; sampled pointers are never authority to free |

`MOWAS2_EXPERIMENT` is read once at logger startup. Effective experiment is
currently always `off`: every nonempty value other than `off` is refused as
unvalidated, including lists and oversized values. Refusal does not alter
recovery or VAC policy. Implement a new value only after its gate passes;
allow exactly one behavioral experiment in a process and report it in both
files. Unsupported layouts refuse the experiment.

The analyzer currently reports `insufficient_evidence` with opportunity hints
and `integration_safety=not_audited`. Do not invent thresholds that convert
allocation volume or sample estimates into certification. Future assessments
may say `worth_controlled_experiment` or `unlikely_to_solve_this_crash` only
when an explicitly documented evidence rule is supported; neither means safe
integration or a universal verdict on allocator replacement.

## Validation and release gates

Before each behavioral test: write hypothesis, expected outcome, acceptance
criteria and rollback. Run portable sanitizers and native x86 Windows tests;
also test under isolated Wine. Cover failed alloc/realloc/free, ownership,
pointer reuse, concurrency, error state, saturation, unsupported APIs, editor,
Steam-text behavior and secure host/join refusals.

Seek three matched gameplay comparisons. Require repeatable completion of
the failing case or useful VA improvement exceeding baseline variability,
without corruption, visual differences, desync or sustained slowdown. Measure
diagnostic CPU/state/log growth; do not translate a component benchmark into
FPS claims. Full request-site profiling must not silently become light mode.

Keep diagnostic releases distinct from proven fixes. Publish only experimental
artifacts with source commit, SHA-256, validation coverage, limitations and
rollback. Public stable remains v2.4.2. Publication is not deployment: obtain
a separate deployment request and confirm the target game/editor is closed.
No intrusive activity on Tety's PC or replacement of a loaded DLL.

While reports are missing, work on explicit-file analysis, regression tests,
isolated mechanism benchmarks and documentation. Park proposals that require
month-scale infrastructure; never add speculative cleanup to show progress.
