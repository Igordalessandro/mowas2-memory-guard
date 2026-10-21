#include "MinHook.h"
#include "compat/stable_steam_text.h"
#include "shared/core/module.h"
#include "shared/logging/logger.h"
#include <new>
#include <psapi.h>
#include <windows.h>
namespace steam_text_guard {
namespace {
using Read64Fn = LONG(WINAPI *)(HANDLE, std::uint64_t, void *, std::uint64_t, std::uint64_t *);
using BufferFn = void *(__cdecl *)(NativeBuffer);
using AccessorFn = void *(__cdecl *)();
Read64Fn read64 = nullptr;
BufferFn original_buffer = nullptr;
HANDLE process_handle = nullptr;
SRWLOCK store_lock = SRWLOCK_INIT;
using TextStore = StableTextStore<2u * 1024u * 1024u, 32768>;
TextStore *store = nullptr;
volatile LONG comparisons = 0, sample_tick = 0, mismatch_reported = 0, success_reported = 0,
              failure_reported = 0;
struct LockedStore {
    const char *intern(const char *text, std::size_t length) {
        AcquireSRWLockExclusive(&store_lock);
        const char *result = store->intern(text, length);
        ReleaseSRWLockExclusive(&store_lock);
        return result;
    }
};
void *__cdecl guarded_buffer(NativeBuffer buffer) {
    const DWORD incoming_error = GetLastError();
    LockedStore locked;
    TextStatus status;
    const char *fresh = fresh_text(
        buffer,
        [](std::uint64_t address, void *out, std::size_t size) {
            std::uint64_t copied = 0;
            return read64(process_handle, address, out, size, &copied) == 0 && copied == size;
        },
        locked, status);
    if (fresh) {
        // Initially compare 16 calls, then sparsely sample until 64 comparisons
        // or the first mismatch. Capture later pointer reuse without per-call
        // original-bridge allocations or unbounded counters/logging.
        const LONG compared = InterlockedCompareExchange(&comparisons, 0, 0);
        const bool compare =
            InterlockedCompareExchange(&mismatch_reported, 0, 0) == 0 && compared < 64 &&
            (compared < 16 ||
             (static_cast<ULONG>(InterlockedIncrement(&sample_tick)) & 1023u) == 0);
        if (compare && InterlockedIncrement(&comparisons) <= 64) {
            SetLastError(incoming_error);
            void *old = original_buffer(buffer);
            char cached[MAX_TEXT_BYTES];
            SIZE_T copied = 0;
            const auto size = static_cast<SIZE_T>(buffer.length);
            if (old && ReadProcessMemory(GetCurrentProcess(), old, cached, size, &copied) &&
                copied == size && std::memcmp(cached, fresh, size) &&
                InterlockedCompareExchange(&mismatch_reported, 1, 0) == 0) {
                logger::log("STEAM TEXT : confirmed stale Proton buffer copy; fresh text "
                            "substituted (contents omitted)");
                logger::report_event("event=steam_text stale_cache=confirmed fresh_copy=active");
            }
        }
        if (InterlockedCompareExchange(&success_reported, 1, 0) == 0)
            logger::log("STEAM TEXT : fresh native text active; immutable cache, 2 MiB/32768 "
                        "entries maximum");
        SetLastError(incoming_error);
        return const_cast<char *>(fresh);
    }
    if ((status == TextStatus::read_failed || status == TextStatus::full) &&
        InterlockedCompareExchange(&failure_reported, 1, 0) == 0) {
        logger::log("STEAM TEXT : fallback status=%u (no text logged)",
                    static_cast<unsigned>(status));
        logger::report_event("event=steam_text fallback=original_bridge check_full_log=yes");
    }
    SetLastError(incoming_error);
    return original_buffer(buffer);
}
bool read_local(const void *source, void *out, SIZE_T size) {
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), source, out, size, &copied) && copied == size;
}
bool in_image(std::uintptr_t address, SIZE_T bytes, const MODULEINFO &image) {
    const auto base = reinterpret_cast<std::uintptr_t>(image.lpBaseOfDll);
    return address >= base && bytes <= image.SizeOfImage &&
           address - base <= image.SizeOfImage - bytes;
}
// Match the complete 16-byte argument setup and helper prologue. Two distinct
// name wrappers must resolve the same target. No fixed RVAs/vtable writes.
void *find_buffer_helper(void *method, const MODULEINFO &image, unsigned stack_pop) {
    unsigned char code[160];
    const auto address = reinterpret_cast<std::uintptr_t>(method);
    if (!in_image(address, sizeof(code), image) || !read_local(method, code, sizeof(code)))
        return nullptr;
    for (std::size_t i = 0; i + 34 < sizeof(code); ++i) {
        const auto *p = code + i;
        const unsigned char expected[] = {
            0x8b, 0x45, 0,    0x89, 0x04, 0x24, 0x8b, 0x45, 0, 0x89, 0x44, 0x24, 0x04, 0x8b,
            0x45, 0,    0x89, 0x44, 0x24, 0x08, 0x8b, 0x45, 0, 0x89, 0x44, 0x24, 0x0c, 0xe8};
        bool match = true;
        for (std::size_t j = 0; j < sizeof(expected); ++j) {
            if (j == 2 || j == 8 || j == 15 || j == 22)
                continue;
            if (p[j] != expected[j]) {
                match = false;
                break;
            }
        }
        if (!match || p[8] != static_cast<unsigned char>(p[2] + 4) ||
            p[15] != static_cast<unsigned char>(p[2] + 8) ||
            p[22] != static_cast<unsigned char>(p[2] + 12) || p[32] != 0xc9 ||
            (stack_pop == 0
                 ? p[33] != 0xc3
                 : (i + 35 >= sizeof(code) || p[33] != 0xc2 || p[34] != stack_pop || p[35] != 0)))
            continue;
        std::int32_t displacement;
        std::memcpy(&displacement, p + 28, sizeof(displacement));
        const auto target = address + i + 32 + static_cast<std::uintptr_t>(displacement);
        const unsigned char prologue[] = {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x51, 0x8d, 0x4d,
                                          0x08, 0x83, 0xec, 0x28, 0x8b, 0x01, 0x8b, 0x51, 0x04};
        unsigned char actual[sizeof(prologue)];
        if (in_image(target, sizeof(actual), image) &&
            read_local(reinterpret_cast<void *>(target), actual, sizeof(actual)) &&
            !std::memcmp(actual, prologue, sizeof(actual)))
            return reinterpret_cast<void *>(target);
    }
    return nullptr;
}
bool install(std::uintptr_t) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll || !GetProcAddress(ntdll, "wine_get_version")) {
        logger::log("STEAM TEXT : native Windows; compatibility hook not needed");
        return true;
    }
    HMODULE bridge = GetModuleHandleW(L"lsteamclient.dll"),
            api = GetModuleHandleW(L"steam_api.dll");
    read64 = reinterpret_cast<Read64Fn>(GetProcAddress(ntdll, "NtWow64ReadVirtualMemory64"));
    auto accessor =
        api ? reinterpret_cast<AccessorFn>(GetProcAddress(api, "SteamFriends")) : nullptr;
    MODULEINFO image = {};
    void *friends = accessor ? accessor() : nullptr;
    void **vtable = nullptr;
    void *methods[8] = {};
    if (!bridge || !read64 || !friends ||
        !GetModuleInformation(GetCurrentProcess(), bridge, &image, sizeof(image)) ||
        !read_local(friends, &vtable, sizeof(vtable)) ||
        !read_local(vtable, methods, sizeof(methods))) {
        logger::log("STEAM TEXT : bridge/reader/interface unavailable; original behavior retained");
        return false;
    }
    void *target = find_buffer_helper(methods[0], image, 0);
    if (!target || find_buffer_helper(methods[7], image, 8) != target) {
        logger::log("STEAM TEXT : unsupported Proton helper ABI; no hook installed");
        return false;
    }
    process_handle = OpenProcess(PROCESS_VM_READ, FALSE, GetCurrentProcessId());
    if (!process_handle)
        return false;
    unsigned char probe = 0;
    std::uint64_t copied = 0;
    if (read64(process_handle, reinterpret_cast<std::uintptr_t>(target), &probe, 1, &copied) != 0 ||
        copied != 1 || probe != 0x55) {
        CloseHandle(process_handle);
        process_handle = nullptr;
        logger::log("STEAM TEXT : native reader failed self-test; no hook installed");
        return false;
    }
    void *storage =
        VirtualAlloc(nullptr, sizeof(TextStore), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!storage) {
        CloseHandle(process_handle);
        process_handle = nullptr;
        logger::log("STEAM TEXT : bounded store allocation failed; no hook installed");
        return false;
    }
    store = new (storage) TextStore{};
    const MH_STATUS created = MH_CreateHook(target, reinterpret_cast<void *>(&guarded_buffer),
                                            reinterpret_cast<void **>(&original_buffer));
    const MH_STATUS enabled = created == MH_OK ? MH_EnableHook(target) : created;
    if (enabled != MH_OK) {
        if (created == MH_OK)
            MH_RemoveHook(target);
        original_buffer = nullptr;
        VirtualFree(store, 0, MEM_RELEASE);
        store = nullptr;
        CloseHandle(process_handle);
        process_handle = nullptr;
        logger::log("STEAM TEXT : hook installation failed status=%d", static_cast<int>(enabled));
        return false;
    }
    logger::log("STEAM TEXT : validated Proton helper %p; fresh text enabled; VAC guards unchanged",
                target);
    return true;
}
} // namespace
MOWAS_REGISTER_MODULE(steam_text, (core::ModuleSpec{"steam_text_guard", 85, &install}));
} // namespace steam_text_guard
