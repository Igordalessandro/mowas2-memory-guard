# Evidence register — 2026-10-22

Statuses: **investigating**, **ready to test**, **parked**, **validated**.
Validated observation is not a validated fix. No behavioral candidate below
has passed its integration gate. No raw community logs belong in Git.

| Hypothesis | Status | Observations / alternatives | Missing evidence and next decision |
| --- | --- | --- | --- |
| RobZ failures involve VA exhaustion and fragmentation | validated (observed sessions only) | .13: four explicit recoveries, then 699,211-byte unrecovered failure, complete scan free=36,196 KiB / largest=480 KiB; physical snapshot still 14,079 MiB available | Does not distinguish resource demand, retention or heap waste. Collect matched .14/.15 full logs; do not assume every crash has this cause |
| Retained path +003749C9 owns redundant texture buffers | investigating | .13: 1,594 KiB in 45 samples scaled to ~1,594 MiB; other sizes match DDS layouts. Could be legitimate resources or biased/incomplete observations | Need resource identity and owner lifetime. Use existing watch ages/stacks and bounded instruction/API inspection; no payload-based guess or forced free |
| Repeated save loads accumulate unused resources | investigating | Narrative 2551→2879→2932→3172 MB then crash; metric and phases unknown. Cache, heap high-water and fragmentation remain alternatives | Same-save fresh/menu/reload logs with voluntary action times. No leak claim until ownership/coverage are established |
| Allocator replacement could reduce waste or contention | investigating | Current game IAT observation/retries are not replacement. High traffic alone is inconclusive; .10/.13 lack allocation-size/thread distributions | .15 adds labeled size/retirement/thread samples. Then audit free/realloc/HeapSize/alignment/flags/other modules and compare isolated allocator workloads; no integration until both opportunity and compatibility gates pass |
| Disabled duplicate mods increase resource demand | investigating | Narrative only; discovery/dependency/catalog changes can explain it without old assets being loaded | Matched fresh-process runs with reporter-controlled folder setup; narrow file trace only if needed. Never automatically hide/delete/move mods |
| DLL overhead caused Tety's slow motion | investigating | .10: no observed allocation failures; saturated site accounting and high alloc/free traffic; working-set gap is not hard-paging proof | Controlled CPU/frame observations and diagnostic cost comparison. Preserve full profiling; do not claim network/FPS cause from memory logs |
| Proton bridge string caching explains wrong lobby strings | investigating | Reproducible pointer-address cache reuse mechanism; bounded workaround inherited from .11. Does not explain every checkerboard artifact | Live UI confirmation and workaround/fallback evidence; keep separate from memory-failure diagnosis |
| Native heap optimization helps this game's address space | parked | API may decommit without releasing VA; no native-game benefit established | Isolated native-Windows VA/latency experiment before any game hook |
| Graphics trim/flush helps reloads | parked | Documented APIs exist; no verified game lifecycle boundary or ownership cause | Resource identity and safe idle boundary first; no periodic purge |
| Full allocator replacement / x64 helper / quality reductions | parked | No broad ownership audit; broad replacement may worsen fragmentation; helper is a larger architecture; quality reduction excluded | Only targeted replacement may advance on measured evidence. No helper, full-engine conversion or quality loss in this roadmap |

## Historical report re-analysis

The explicit-file analyzer reproduced these previously recorded findings:

- RobZ .13, SHA-256 `59e78f054e34bdd5b521fb1702c80f26db433e4670a512e351b351db4ec68719`:
  4 recovered + 1 unrecovered low-level records; private peak 3,417 MiB.
- RobZ .10, SHA-256 `1911185804272897ce4fb75c0607b532f1fa94dc4958afc72bfcc8cb8ffb36ea`:
  4 recovered + 1 unrecovered low-level records; private peak 3,396 MiB.
- Tety .10, SHA-256 `e090f4a35dcb49163f249e1aa2d7685c949d6ee0957bc2adcf1e90ffa7d4e085`:
  no low-level failure records; private peak 3,320 MiB.

All three currently remain `insufficient_evidence` for allocator replacement.
No captured exception in these inputs means termination remains unknown, not
that the game did not crash. Session length is not match duration. Sampled
native/engine records may describe the same logical request: never sum them
as independent resource ownership.
