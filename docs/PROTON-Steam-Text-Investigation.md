# Proton Steam text investigation — 2026-10-21

Candidate: `2.5.0-experimental.11-dev`, based on experimental.10.
This is a local development candidate, **not a published release**. Do not
replace an installed DLL while the game/editor/Workshop utility is running.
Stable v2.4.2 and the published experimental.10 assets are unchanged.

## Evidence and limits of the conclusion

The Linux/Proton session loaded experimental.10. The user supplied screenshots
showing incorrect own/friend names, names appearing in lobby metadata columns,
numeric and hexadecimal-looking text in name fields, and a checkerboard image.
There were no observed allocation failures; approximately 3 GiB of game VA was
still free. This is not evidence of address-space exhaustion or altered Steam
account identities. The checkerboard's cause remains unconfirmed.

Read-only `/proc/<pid>/mem` checks, without ptrace attachment, suspension or
remote calls, verified that the own/friend persona method prologues and their
static dispatch slots were unchanged. The join guard detoured the expected
SteamUser018 authentication method, not a shared persona implementation.

The installed Proton was `proton-10.0-4b`. Its 32-bit process uses the 64-bit
Unix Steam client through WoW64. In its native `buffer_cache`, many retained
copies differed from the source bytes at their original native addresses.
Some retained strings matched text visible in the screenshots. Historical
cache entries can outlive their native source: a mismatch alone does not prove
that a particular stale entry was returned for a current UI lookup. Live-game
validation and an A/B comparison are still required to close causality.

The mechanism is nevertheless reproducible: Proton's `get_unix_buffer` accepts
`u_buffer { uint64_t ptr; uint64_t len; }`. A high native pointer cannot be
returned to an x86 game directly, so Proton allocates a low copy. Its native
cache hashes/compares the source address, and a cache hit returns the first
copy without refreshing it. Address reuse or mutation can therefore return an
old name, numeric string, or unrelated lobby value. Both own/friend names and
`GetLobbyData` use this helper. Low native pointers bypass this copy path.

Primary sources:

- [Native buffer cache](https://github.com/ValveSoftware/Proton/blob/proton_10.0/lsteamclient/unixlib.cpp#L801-L830).
- [x86 helper](https://github.com/ValveSoftware/Proton/blob/proton_10.0/lsteamclient/steamclient_main.c#L328-L344).
- [Name wrappers](https://github.com/ValveSoftware/Proton/blob/proton_10.0/lsteamclient/winISteamFriends.c).
- [Lobby text wrappers](https://github.com/ValveSoftware/Proton/blob/proton_10.0/lsteamclient/winISteamMatchmaking.c).
- [Wine's native-address read syscall](https://github.com/wine-mirror/wine/blob/master/dlls/ntdll/unix/virtual.c).

This supersedes the earlier assumption that removing Steam vtable changes had
established a complete fix. It does **not** establish that every earlier DLL
version was harmless or that timing changes cannot expose the Proton defect.

## Candidate implementation

`src/compat/steam_text_guard.cpp` registers an optional priority-85 module.
Native Windows returns before accessing Steam interfaces or allocating a
cache. On supported Wine/Proton it:

1. Gets the existing SteamFriends interface (no Steam initialization or new
   login), reads its own-name and friend-name method pointers without writing
   any dispatch table, and examines at most 160 bytes per wrapper.
2. Requires both complete x86 by-value argument sequences to call the same
   module-local helper, including the expected caller stack cleanup and exact
   helper prologue. It does not scan the disk, search whole modules, use fixed
   runtime addresses, or modify an unsupported layout.
3. Resolves `NtWow64ReadVirtualMemory64`, opens a read-only handle to its own
   process, validates the reader, then installs one helper detour with MinHook.
4. For a high native pointer to a complete C string of at most 4096 bytes,
   reads current source bytes and interns them by **content**, not address.
   Unicode and legitimate numeric/hexadecimal names are preserved byte-for-byte.
5. Returns immutable low-address copies. Subsequent lookups cannot overwrite
   older returned strings. The pool never evicts live pointers or frees
   engine-owned memory. Copies retain 16-byte alignment. It is bounded to 2 MiB
   of text storage (including alignment padding) plus 32768 lookup slots
   (approximately 2.5 MiB total on x86), allocated only on supported Proton.

Low pointers, binary data, oversized strings, failed reads and pool exhaustion
retain the original bridge behavior. Thus saturation/read failure can leave
the original bug unfixed; a one-time diagnostic makes that limitation visible.
Unsupported helper layouts similarly remain unchanged. No fallback kills the
game or disables a VAC guard. Existing allocator sizes, emergency reserve
policy and secure host/join refusal code are unchanged.

The wrapper preserves the incoming error state on substituted results and the
original delegate's error state on fallback. There is no per-request disk log.
Initially 16 results and then sparse samples (at most 64 comparisons total,
stopping on the first mismatch) are compared to the original helper's copy.
A detected mismatch records `stale_cache=confirmed` without text, Steam IDs or
native addresses. A single active/fallback message is also recorded.

## Validation and handoff

- Portable production-core tests cover repeated native-address reuse,
  changing sizes, immutable earlier pointers, Unicode, empty/numeric/hex names,
  binary/unterminated buffers, read failures, overflow and bounded saturation.
- Windows x86 wrapper tests cover actual production fallback/error behavior,
  stale-copy detection and ABI recognition, including relocation, backwards
  relative calls, wrong argument cleanup, edited prologues and invalid ranges.
- `tests/steam_text_read64_smoke.cpp` is a separate opt-in Wine executable. It
  allocates one page above 4 GiB in its **own process**, writes two successive
  strings to the same address, and exercises the actual native read syscall.
  It never loads Steam/the game or installs hooks; process exit reclaims its page.

### Recorded local results

- All six portable suites passed with ASan/UBSan.
- All fifteen Windows x86 regression executables compiled with the pinned
  llvm-mingw toolchain and **executed successfully under isolated Wine 11.19**.
  This is not a native-Windows runtime validation claim.
- The real above-4-GiB allocation/read/write/reuse smoke passed under Wine 11.19.
- Under the **installed Proton 10.0-4b WoW64 runtime**, a fixed-address
  4-GiB allocation returned `STATUS_CONFLICTING_ADDRESSES`; that allocation
  subtest is not counted as a pass. An alternate read-only probe of the test
  process's existing high Unix libc mapping successfully read its ELF bytes
  and copied a real native C string through the production `fresh_text` path.
  `MOWAS2_SMOKE_UNIX_PID` enables that probe; a wrapper must set it to its
  Linux PID immediately before `exec wine`. It does not inspect another process.
- Both production ABI recognizers matched the installed Proton DLL in an
  image-only `DONT_RESOLVE_DLL_REFERENCES` mapping. An opt-in build of
  `steam_text_guard_tests.cpp` with `MOWAS2_REAL_HOOK_SMOKE` and real MinHook
  objects then installed/called/removed the real helper detour **only in its
  own isolated process**. The 16-byte x86 by-value ABI and error preservation
  passed; no Steam initialization or real helper imports were executed.

The optional image probe takes `--probe-proton <dll> <own-name-rva>
<friend-name-rva> <helper-rva>`. For the installed image these are
`0x50770`, `0x4fcc0`, `0xb870`, respectively. They are **fixture inputs**, not
addresses embedded in the shipping installer. Image SHA-256:
`b7e92d767b16884c706cd7dc7ba2e52bc7f87f9816cc46ae4157bee35ff9e99a`.

Built candidate: `dist/v2.5.0-experimental.11-dev/dbghelp.dll`, PE32/x86,
exports only `ImageNtHeader` and `MiniDumpWriteDump`. SHA-256:
`09ed0886d5898fe90c9eb09a45ced06aab8e9de0b59e16bc0b3858100996bd45`.
At the investigation checkpoint the installed game DLL remained experimental.10;
neither PC was updated and no GitHub release was created. Live UI verification
and a controlled comparison remained outstanding.

Subsequent deployment, explicitly requested on 2026-10-21: the above
experimental.11-dev artifact was installed on both the local Proton game and
Tety's native-Windows game, with both games/editors closed. Installed hashes
matched the candidate on both PCs, and verified experimental.10 rollback copies
were retained on each PC. The local native/builtin dbghelp override was preserved.
The remote update was silent and neither game was launched. This update adds
the candidate text workaround, not a reduced-overhead profiler. No GitHub
publication was requested or performed; live UI verification is still pending.

Before deployment, run the portable suite under ASan/UBSan, compile the x86
tests and candidate DLL, and record which Windows/Wine executables actually
ran. Cross-compilation alone is not a runtime pass. Do not use the game prefix
for synthetic tests, modify the globally installed Proton, or kill unrelated
Wine processes. Keep diagnostics scoped and avoid recursive filesystem scans.

After the user exits: install the candidate locally with an exact rollback
copy and hash verification. On the next launch verify `STEAM TEXT` activation,
exercise own/friend names, lobby titles/maps/player counts, repeated refreshes,
and a private insecure session. Monitor bounded cache fallback and allocation
telemetry. Check the checkerboard separately rather than declaring it fixed.
If symptoms persist, compare with the previous DLL and/or traditional Proton
mode (`PROTON_USE_WOW64=0` where the installed Proton supports it) during
controlled restarts. Do not claim the bug is fixed until the UI is verified.
