# v2.5.0-experimental.15

**Diagnostic preparation, not an allocator replacement or a claimed RobZ fix.**
Full profiling, native allocation/recovery behavior, editor isolation and
fail-closed VAC-secured host/join refusals remain unchanged. No graphics
quality reduction. Stable GitHub Latest remains **v2.4.2**.

## What's new

- The <=900-byte ASCII **DLL Crash Report.txt** identifies the exact matching
  full log and session. Failure-time memory stays pinned; largest free space
  uses KiB. Retained sample and coverage outrank optional cumulative traffic.
- Full logs add twelve allocation-size sample buckets, sampled retirement
  sizes, same/cross-thread observations, realloc retirements and flags.
  Size sampling varies its position in each 1,024-attempt block; existing full
  request-site profiling is not reduced. Additional metadata is compile-time
  capped below 256 KiB. No per-allocation disk log or payload reads.
- Periodic CPU measurements and explicit query-availability indicators help
  investigate slow motion without confusing unknown counters with zeros.
- The optional **offline analyzer** reads only explicitly selected reports
  and produces JSON or Markdown with memory/retention trends, failure history,
  coverage warnings, allocator evidence checks and next investigation steps.
  It is available in the repository's `tools/analyze_logs.py`, not required
  for playing. Python 3 standard library only; no service or uploads.
- An evidence register and gated roadmap keep hypotheses separate from
  validated fixes. Current allocator assessments remain **insufficient evidence**:
  high allocation traffic and sample estimates do not prove replaceable waste.
- `MOWAS2_EXPERIMENT` is default-off. All non-off requests are currently
  refused as unvalidated; **no cleanup or replacement allocator is enabled**.

This remains one **32-bit dbghelp.dll**. There is no 64-bit engine conversion,
automatic cache purge, arbitrary resource freeing or added address space.

## Install / rollback

Close the game/editor and preserve your previous DLL first.

- **Windows:** put `dbghelp.dll` beside the supported Steam `mowas_2.exe`
  (version 3.262.1, October 2019).
- **Linux / Proton:** use the same DLL with the native-first override. Add
  `dbghelp` as **Native, then Builtin** in Protontricks/Wine configuration, or
  retain `WINEDLLOVERRIDES="dbghelp=n,b" %command%` with your other Proton options.

No experiment flag is needed. Confirm `.15`, `mode=full request-site=1/1`,
`REPORT SESSION : fmt=2`, and successful VAC guards in the full log.
To roll back, close the game/editor and restore your preserved DLL, or use
the public stable v2.4.2 artifact. Publication does not update either PC.

SHA-256 of `dbghelp.dll`:

`80a945a320b4612718ad5cc9c61f96033fc0442f981e9b5900784b1a9af678e6`

## Reporting and limitations

Paste **DLL Crash Report.txt** in Steam. For investigation, upload the matching
full timestamped log shown by `full_log=`. Review it for private paths/names
and inherited bounded game-code fragments before sharing. Nothing uploads
automatically. Mods, map/save, last action, any dialog, and approximate
load/menu/reload times are useful voluntary context.

Sampling can be biased, incomplete or saturated. Allocator ownership,
other-module frees and size queries are not audited. The analyzer does not
infer leaks, crash/clean exit from an ending log, or a controlled comparison
from matching version metadata. Its histories are bounded and omissions are
explicit. These are diagnostic improvements, not evidence of better FPS or
compatibility with every large mod.

VAC safeguards refuse secure sessions; they do not disable/bypass VAC or
guarantee account safety. **Use at your own risk.** The project owner and
contributors are not responsible for bans, account restrictions, crashes,
data loss or other damage. Do not use multiplayer if this risk is unacceptable.

## Validation

Publication requires the native Windows x86 and Linux sanitizer checks on the
release source commit. Local Wine and synthetic tests are not native gameplay
validation. See the [CI checks](https://github.com/Igordalessandro/mowas2-memory-guard/actions/workflows/regression-tests.yml)
and [implementation guide](https://github.com/Igordalessandro/mowas2-memory-guard/blob/v2.5.0-experimental.15/docs/EXPERIMENTAL-15.md).

Verified release source: `dd18ba9528efb1fbf6fc5f70afee42127b5a8568`.
[Run 39847951542](https://github.com/Igordalessandro/mowas2-memory-guard/actions/runs/39847951542)
passed nine portable sanitizer suites, ten analyzer tests and eighteen native
Windows x86 regression executables. Both uploaded assets were downloaded and
byte-compared before publication. Neither installed game was updated.
