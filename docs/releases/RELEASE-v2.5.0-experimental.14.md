# v2.5.0-experimental.14

Targeted diagnostics for memory retained during loading and across reloads.
**Full request-site profiling stays enabled; there is no light-mode option.**
This is a prerelease, not a confirmed RobZ/reload-leak fix. Stable GitHub Latest
remains **v2.4.2**.

## What's new

- **Ages of surviving allocations:** full logs report actual sample counts,
  bytes, signed changes, oldest ages and age buckets for five recurring
  callers, at the existing 30-second cadence and on allocation failures.
  This distinguishes newly allocated samples from those surviving 10/60
  seconds; it does not declare old allocations to be leaks or safe to free.
- **Bounded caller traces:** up to five first successful watched heap requests
  and eight failure callbacks record native/engine sizes and up to twelve
  game-relative stack frames. Traces are emitted once, not per allocation.
- **Small runtime code windows:** at most five 64-byte fragments around the
  watched callers may be read from this process's loaded executable image.
  They help future investigation when on-disk code is encrypted. This is not
  a module dump, allocation-content scan or new patch at those addresses.
  Unsupported/unreadable data is left unchanged; readable code is not a
  guarantee of function identification or plaintext instructions.
- **Small failures no longer disappear from the size-filtered history:**
  actual failures below 1 MiB, including recovered ones, retain exact native
  and active-engine request context in the existing fixed history. Ordinary
  small successful requests remain filtered out.
- **Watch sites registered before saturation:** the five observed callers keep
  their site-table entries even if general registration later fills up.

The new state is compile-time capped below 4 KiB, with fixed stack snapshots.
New age/trace wall-time fields help measure diagnostic cost. This is not a
zero-overhead or gameplay-performance guarantee. All additional details go
into the full log; **`DLL Crash Report.txt` remains capped at 900 bytes**.

## What this does not change

Still a single **PE32/x86 DLL**, with .13's full request-site accounting,
original allocator semantics, exact lobby-sentinel repair, 8/8/48/32 MiB staged
reserves, retry/rearm rules, long-path compatibility, editor/Workshop isolation
and Proton text workaround. Existing lifetime/engine sampling is unchanged.
There is no automatic resource purge, garbage collector, texture downscaling,
mod rewrite or expansion of the game's 32-bit address space.

The new RobZ .13 report showed four recovered failures followed by exhaustion
with a 480 KiB largest free region, despite about 14 GiB of physical RAM still
available. Repeated callers and texture-sized requests are leads, not proof
of resource contents, duplicate copies or a leak. This release collects the
missing ownership/lifetime clues; it does not claim to have repaired the cause.

VAC-secured hosting/joining refusals remain enabled and fail closed when their
guards cannot install. They do not disable/bypass VAC or guarantee account
safety. **Use at your own risk:** the project owner and contributors are not
responsible for bans, account restrictions, data loss, crashes or other damage.
Do not use the DLL in multiplayer if this risk is unacceptable.

## Installation

Close the game and editor first; keep your current DLL for rollback.

- **Windows:** put the attached `dbghelp.dll` beside `mowas_2.exe`.
- **Linux / Steam Proton:** use the same DLL and retain the native-first
  `dbghelp` override. In Protontricks/Wine configuration, set Libraries →
  `dbghelp` → Native, then Builtin, or keep the existing
  `WINEDLLOVERRIDES="dbghelp=n,b" %command%` launch configuration. Preserve
  other required Proton settings. No profiling flag is needed.

Confirm version `2.5.0-experimental.14`, `mode=full request-site=1/1`,
`ALLOC WATCH installed` and successful VAC-guard installation in the new log.
The attached checksum should match:

`73efe4215d9f99ed27ae80d1d1e3fe645ba8a7dc5c86d17c3e3868c0fa7152c0`

## Reports that will help

Provide the compact crash report **and the matching full timestamped log**.
The Steam-sized report cannot hold the new evidence. Review the full log for
private paths/names before sharing it; it also contains the bounded game-code
fragments described above. Nothing is uploaded automatically.

Record the times of first map/save load, return to menu and identical reloads,
plus mods, map, settings and dialog text. The DLL does not infer those phases.
For comparisons, use fresh processes and change only one variable at a time,
such as texture quality or .13 versus .14. Retain the safeguards.

Eight portable regression suites passed with ASan/UBSan; seventeen Windows
x86 regression executables passed locally under isolated Wine 11.19. Native
Windows regression status is available in the
[CI checks](https://github.com/Igordalessandro/mowas2-memory-guard/actions/workflows/regression-tests.yml).
These controlled tests are not a heavy-mod/gameplay-stability claim.

See the [field guide and implementation limits](https://github.com/Igordalessandro/mowas2-memory-guard/blob/v2.5.0-experimental.14/docs/EXPERIMENTAL-14.md).
