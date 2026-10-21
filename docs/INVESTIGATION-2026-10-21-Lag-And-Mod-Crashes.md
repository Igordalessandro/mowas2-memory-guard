# Slow motion, repeated loads and heavy-mod exhaustion — 2026-10-21

Status: local `experimental/lag-investigation` work; candidate
`2.5.0-experimental.12-dev`. Not a public release or a confirmed gameplay fix.
Neither installed game was changed during this investigation. Public stable
2.4.2 and public experimental.10 remain unchanged. The candidate inherits the
[experimental.11 Proton text workaround](PROTON-Steam-Text-Investigation.md).

## Evidence and limits

Two full experimental.10 logs were read, not just their compact summaries:

| Evidence | Session identifier | SHA-256 |
| --- | --- | --- |
| New RobZ report linked by the user | 2026-10-21 00:20:37, p67676 | `1911185804272897ce4fb75c0607b532f1fa94dc4958afc72bfcc8cb8ffb36ea` |
| Tety's last reported slow match | 2026-10-20 21:29:06, p20996 | `e090f4a35dcb49163f249e1aa2d7685c949d6ee0957bc2adcf1e90ffa7d4e085` |

The original logs are archived outside Git in the sibling private
`mowas2-memory-guard-local-diagnostics` directory. Do not publish them wholesale:
they can contain personal names, paths and machine details. Times below are
logger timestamps, not independently verified UTC times or match durations.

The repeated-save and duplicate-mod reports are narrative evidence only: no
matching log, save, mod set or controlled reproduction was supplied. Their
reported MB readings do not identify private bytes versus working set.

## 1. Tety's slow motion

The user clarified that Tety's game ran in slow motion, without a multiplayer
"waiting for player" pause; the local player's game was not the slow one.
That is not enough to distinguish simulation pacing, low FPS, CPU load,
memory pressure, graphics/driver issues or DLL overhead. Lack of a waiting
message does not prove networking is uninvolved.

Her native-Windows .10 log shows approximately 75,238 heap allocations and
75,230 frees per second between 23:26:10 and 23:26:40. The diagnostic site table
was saturated, with approximately 59.7 million recorded dropped lookups. The
old one-entry per-thread cache can repeatedly miss for alternating callers,
even though saturated lookup is already bounded to 16 probes. Per-request
site counters also perform synchronized 64-bit updates on x86. These are
plausible, removable costs, **not a proven cause of the slow motion**.

Late-match private bytes were about 3314–3320 MiB; the working set was only
1167–1183 MiB. This gap does not prove hard-fault thrashing: some committed
memory can be untouched or nonresident. There were no recorded allocation
failures. Free VA was about 135 MiB, with a 76 MiB largest free region and
88 MiB of reserves still protected. The new no-reserves preflight shortcut
therefore would not have applied to that late-match state.

The session log is about 0.78 MB over almost two hours, not evidence of runaway
disk logging. Old timestamps cannot measure VA-scan CPU cost: scans and log
messages have different cadences. The .11 native-Windows startup log confirms
the optional Steam-text compatibility hook was skipped there; that hook is
not active in Tety's Windows process.

## 2. New RobZ crash: recoveries succeeded, then space ran out

Reported configuration: RobZ 1.28.6, more CP and cosmetic mods, 300 CP / 50k MP,
medium-to-high graphics. The user reports 7–8 minutes of gameplay. The full
process log spans longer; do not label its entire duration as gameplay.

| Log time | Failed native request | Reserve released | Retry |
| --- | ---: | ---: | --- |
| 00:41:03 | 1,440,019 bytes | final 48 MiB | succeeded |
| 00:43:35 | 699,235 bytes | recovery 32 MiB | succeeded |
| 00:44:33 | 699,235 bytes | rearmed recovery 16 MiB | succeeded |
| 00:46:02 | 951,289 bytes | rearmed recovery 8 MiB | succeeded |
| 00:47:46 | 4,591,679 bytes | none remaining | failed |

At the final failure the fresh complete VA scan measured 40,336 KiB free in
total, but only 1024 KiB in its largest free region, with no 4/8/16/32 MiB hole.
Committed VA was 3776 MiB and reserved VA 279 MiB. The native request was about
4.38 MiB; the engine request was 4,591,660 bytes (19 bytes smaller).

This is strong evidence of near-exhaustion and fragmentation of the 32-bit
address space, not a diagnosis that the PC needs more physical RAM. The
reserve/retry system demonstrably recovered four failures, including rearming
smaller recovery blocks. It did not eliminate the underlying resource demand.
The six minutes between first recovery and final failure are not a controlled
measurement of extra gameplay gained.

The separate large-allocation history also records a successful request of
the same size on the same thread 63 tick-ms before the failure. Its engine
caller was `+0032762D`; the failing caller was `+001C43A6`, through native
`+0093176C`. This is a useful investigation pair, but does not prove these
were two copies of one object. Do not change allocation sizes based on that
assumption. Retained sampling highlights `+003749C9` (about 593 MiB estimated
from only 37 live samples); that is a lead, not an exact leak size.

Targeted inspection of the installed on-disk executable encounters SteamStub
encrypted code at the relevant engine caller. Consequently, no texture,
save-loader or other semantic function identity has been established. No
unpacking, game launch or speculative resource-freeing patch was attempted.

## 3. Repeated save loads

Reported sequence: 2551 → 2879 → 2932 → 3172 MB, then failure. That is a net
621 MB rise before the fifth load, with uneven increments of 328, 53 and
240 MB. It suggests something survives reloads or heap usage does not return
to baseline. It does **not** distinguish a live-object leak, intentional
resource cache, heap high-water retention or fragmentation.

Next controlled test, using the same save and graphics/mod configuration:

1. Start a fresh process; record after first load, after returning to menu,
   and after each identical reload, using fixed settling times.
2. Save the full session log as well as the compact report. Record which
   memory counter supplied any external MB measurements.
3. Compare retained caller samples, private bytes, commit/reserve and largest
   free hole. A rising working set alone is not evidence of a leak. Sampling
   is incomplete and cannot prove absence of a leak either.
4. Compare a fresh-process load at the same point. This separates reload
   accumulation from a save that already approaches the process limit.

Do not free sampled pointers: they are observations, not ownership records.
Restarting between loads is a workaround to test, not an engine repair.

## 4. Disabled older mod version still changes memory

Presence in a searched mod directory may affect metadata/catalog discovery,
dependencies, resource resolution or cache behavior. It does not prove all
assets from a disabled mod are loaded. Names, versions and enabled flags alone
are insufficient to safely identify folders the DLL should hide.

Ask the reporter for two fresh-process runs of the same save/settings: only
the current version discoverable, versus both folders present with the older
one disabled. Preserve the old copy outside searched mod directories rather
than deleting it. A bounded native file-access capture should distinguish
opening only metadata/previews from reading old resource archives or assets.
No automatic file tracing, mod moves, filtering or deletions were added here;
those could change dependencies and multiplayer checksums.

## Local candidate changes

- Default `light` request-site profiling samples approximately 1/1024 calls
  independently per thread. Scaled site counts/bytes are explicitly labelled
  estimates (`ALLOC TOP REQUEST EST`, compact `req_est`). **Global allocation,
  free and requested-byte totals remain exact.** Existing lifetime and engine
  sampling, failure snapshots and large-allocation history remain enabled.
- A fixed 64-entry per-thread caller cache replaces the one-entry cache and
  also remembers saturated misses. It compares complete keys and allocates
  no dynamic containers. On x86 it adds about 768 bytes per participating
  thread, not an unbounded history.
- Advisory pressure/table-saturation reads use lock-free relaxed atomic loads
  instead of locked read-modify-write operations. Recovery state locks are
  unchanged.
- Large-request preflight skips its full VA walk when no primary, critical
  or final stage remains. It does not consume the protected recovery reserve.
  An actual failure still takes a fresh scan and follows identical retries.
- Full logs include cumulative VA-scan calls, query counts, wall milliseconds
  and worst duration by monitor/preflight/failure/other category. Timing is
  coarse and includes scheduling; it is not CPU profiling or frame timing.
- Periodic system-available physical RAM and the process page-fault total are
  logged. Page-fault totals include soft and hard faults, and are not an
  active-paging diagnosis on their own. These additions stay out of the
  Steam-sized compact report, whose 900-byte cap is unchanged.

No native allocator, allocation flag, requested size, reserve threshold,
resource ownership, VAC refusal, path compatibility or proxy policy changed.
The new version remains a single PE32/x86 DLL, not a 64-bit engine conversion.

Mode selection is read once before publishing allocator hooks. Default is
`light`; `MOWAS2_PROFILE_MODE=full` restores per-request site accounting for
comparisons. Invalid/overlong settings fall back to full with a warning, not
silently disabled diagnostics. On Proton, prepend `MOWAS2_PROFILE_MODE=full`
to existing launch options, preserving the override and `%command%`. On
Windows, set that environment variable in the launch environment and fully
restart the game/launcher as needed. It cannot change a running session.

## Validation and next gate

- All seven portable suites passed with ASan/UBSan.
- All sixteen Windows x86 regression executables compiled with strict
  warnings and ran successfully in an isolated Wine 11.19 prefix. This is
  **not native-Windows gameplay validation**. Tests do not start Steam, the
  game or the editor and do not use either installed game prefix.
- New tests cover cache collisions and negative caching, deterministic
  request sampling, 64-bit weighted byte totals, light-mode exact global
  totals/identical native retries/error state, exhausted-stage preflight
  bypass, protected recovery and timing/counter wrap behavior.
- A synthetic one-million-call request-site benchmark measured 16,288 µs in
  the new full mode versus 4985 µs in light mode under Wine. This measures
  only one diagnostic component: **not full hook overhead, old .10 versus
  .12, native Windows, multiplayer or FPS**. The absolute difference is only
  about 11 ms per million calls here; it cannot explain severe slow motion
  by itself. Do not advertise a threefold gameplay speedup.
- Built `dist/v2.5.0-experimental.12-dev/dbghelp.dll`, PE32/x86, exporting only
  `ImageNtHeader` and `MiniDumpWriteDump`. SHA-256:
  `e8f6988a056889d213717aab7886265980a3020176a1649c83e129e322e697a5`.

Neither PC was updated and nothing was pushed or released. Before deployment,
confirm the user wants the candidate, close the relevant game/editor, retain
and verify the installed .11 rollback copy, then verify the deployed hash.
Compare the same map/save and settings at fixed points, preferably repeated
fresh-process runs. Record whether camera/UI motion is choppy as well as
whether simulated units run slowly, plus per-core CPU/frame-time observations.
Compare .11 with .12-light; use .12-full to isolate request-site sampling.
Keep secure-host/join refusals enabled in every multiplayer comparison.

## Why no automatic RAM purge was added

The supported LAA x86 process already has up to 4 GiB of user VA on x64
Windows; adding physical RAM cannot enlarge its pointers. See Microsoft's
[memory limits](https://learn.microsoft.com/en-us/windows/win32/memory/memory-limits-for-windows-releases).
Windows' [low-fragmentation heap](https://learn.microsoft.com/en-us/windows/desktop/Memory/low-fragmentation-heap)
already operates for small allocations (roughly up to 16 KB); the report's
tracked heap reports compatibility mode 2. That does not solve a 4.38 MiB
allocation. [HeapCompact](https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heapcompact)
is not a mechanism for relocating live game objects, and ordinary frees
already coalesce adjacent free blocks.
[EmptyWorkingSet](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-emptyworkingset)
removes resident pages, not live resources or their address reservations.

One bounded future experiment is Windows 8.1+
[`HeapOptimizeResources`](https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heapsetinformation)
to trim already-free LFH caches. Test on an isolated native-Windows heap first,
measure reserved VA as well as commitment, and handle unsupported runtimes.
Decommit is not proof of released VA, this cannot free live engine resources,
and it might add stalls. It was deliberately **not enabled in this candidate**.
