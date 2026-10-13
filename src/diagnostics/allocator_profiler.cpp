#include "diagnostics/allocator_profiler.h"

#include "app/config.h"
#include "orchestration/memory_guard.h"
#include "shared/core/module.h"
#include "shared/logging/logger.h"

#include <windows.h>
#include <intrin.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace allocator_profiler {
namespace {

using HeapAllocFn = LPVOID (WINAPI*)(HANDLE, DWORD, SIZE_T);
using HeapReAllocFn = LPVOID (WINAPI*)(HANDLE, DWORD, LPVOID, SIZE_T);
using HeapFreeFn = BOOL (WINAPI*)(HANDLE, DWORD, LPVOID);
using VirtualAllocFn = LPVOID (WINAPI*)(LPVOID, SIZE_T, DWORD, DWORD);
using VirtualFreeFn = BOOL (WINAPI*)(LPVOID, SIZE_T, DWORD);

constexpr unsigned long long MiB = 1024ull * 1024ull;
constexpr DWORD TARGET_IMAGE_SIZE = 0x00CFC000u;
constexpr unsigned CALLSITE_CAPACITY = 1024;
constexpr unsigned SAMPLE_CAPACITY = 16384;
constexpr unsigned SAMPLE_RATE = 1024;
constexpr unsigned ENGINE_SAMPLE_RATE = 4096;
constexpr unsigned SAMPLE_PROBES = 16;
constexpr DWORD SUMMARY_INTERVAL_MS = 30000;
constexpr DWORD DETAIL_INTERVAL_MS = 120000;

static HeapAllocFn g_originalHeapAlloc = nullptr;
static HeapReAllocFn g_originalHeapReAlloc = nullptr;
static HeapFreeFn g_originalHeapFree = nullptr;
static VirtualAllocFn g_originalVirtualAlloc = nullptr;
static VirtualFreeFn g_originalVirtualFree = nullptr;
static std::uintptr_t g_base = 0;
static volatile LONG g_installed = 0;
static thread_local unsigned g_engine_sample_counter = 0;

__declspec(align(8)) struct CallsiteStat {
    volatile LONG rva;
    volatile LONG alloc_calls;
    volatile LONG realloc_calls;
    volatile LONG free_calls;
    volatile LONG failures;
    volatile LONG sample_live_count;
    volatile LONG sample_free_count;
    volatile LONG engine_sample_calls;
    volatile LONG64 requested_bytes;
    volatile LONG64 sample_live_bytes;
    volatile LONG64 sample_freed_bytes;
    volatile LONG64 sample_lifetime_ms;
    volatile LONG64 engine_sample_bytes;
};

struct SampleEntry {
    void* pointer;
    DWORD size;
    DWORD alloc_rva;
    DWORD tick;
};

static CallsiteStat g_callsites[CALLSITE_CAPACITY] = {};
static SampleEntry g_samples[SAMPLE_CAPACITY] = {};
static volatile LONG g_sample_lock = 0;
static volatile LONG g_sample_entries = 0;
static volatile LONG g_sample_drops = 0;
static volatile LONG g_callsite_drops = 0;

static volatile LONG g_heap_alloc_calls = 0;
static volatile LONG g_heap_realloc_calls = 0;
static volatile LONG g_heap_free_calls = 0;
static volatile LONG g_heap_failures = 0;
static volatile LONG g_virtual_alloc_calls = 0;
static volatile LONG g_virtual_free_calls = 0;
static volatile LONG g_virtual_failures = 0;
__declspec(align(8)) static volatile LONG64 g_heap_requested_bytes = 0;
__declspec(align(8)) static volatile LONG64 g_virtual_requested_bytes = 0;

static __forceinline DWORD caller_rva() {
    const std::uintptr_t caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (caller >= g_base && caller < g_base + TARGET_IMAGE_SIZE) {
        return static_cast<DWORD>(caller - g_base);
    }
    return 0xFFFFFFFFu;
}

static DWORD mix_pointer(const void* pointer) {
    DWORD value = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(pointer) >> 4);
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    value *= 0x846CA68Bu;
    value ^= value >> 16;
    return value;
}

static bool should_sample(const void* pointer) {
    return pointer && ((mix_pointer(pointer) & (SAMPLE_RATE - 1u)) == 0);
}

static DWORD sampled_upstream_rva(DWORD direct_rva) {
    void* frames[8] = {};
    const USHORT count = RtlCaptureStackBackTrace(0, 8, frames, nullptr);
    bool saw_direct = false;
    for (USHORT i = 0; i < count; ++i) {
        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(frames[i]);
        if (address < g_base || address >= g_base + TARGET_IMAGE_SIZE) continue;
        const DWORD rva = static_cast<DWORD>(address - g_base);
        if (!saw_direct) {
            if (rva == direct_rva) saw_direct = true;
            continue;
        }
        if (rva != direct_rva) return rva;
    }
    return direct_rva;
}

static void lock_samples() {
    while (InterlockedCompareExchange(&g_sample_lock, 1, 0) != 0) {
        SwitchToThread();
    }
}

static void unlock_samples() {
    InterlockedExchange(&g_sample_lock, 0);
}

static LONG64 load64(volatile LONG64* value) {
    return InterlockedCompareExchange64(value, 0, 0);
}

static CallsiteStat* callsite_for(DWORD rva) {
    // Zero means unused, so keep the out-of-image sentinel but shift a true
    // RVA zero to one (the game has no allocation call at its DOS header).
    LONG key = static_cast<LONG>(rva == 0 ? 1u : rva);
    unsigned index = (rva * 2654435761u) & (CALLSITE_CAPACITY - 1u);
    for (unsigned probe = 0; probe < CALLSITE_CAPACITY; ++probe) {
        CallsiteStat& slot = g_callsites[(index + probe) & (CALLSITE_CAPACITY - 1u)];
        LONG observed = slot.rva;
        if (observed == key) return &slot;
        if (observed == 0) {
            observed = InterlockedCompareExchange(&slot.rva, key, 0);
            if (observed == 0 || observed == key) return &slot;
        }
    }
    InterlockedIncrement(&g_callsite_drops);
    return nullptr;
}

static void record_alloc_site(DWORD rva, SIZE_T size, bool realloc_call) {
    CallsiteStat* site = callsite_for(rva);
    if (!site) return;
    if (realloc_call) InterlockedIncrement(&site->realloc_calls);
    else InterlockedIncrement(&site->alloc_calls);
    InterlockedExchangeAdd64(&site->requested_bytes, static_cast<LONG64>(size));
}

static void record_free_site(DWORD rva) {
    CallsiteStat* site = callsite_for(rva);
    if (site) InterlockedIncrement(&site->free_calls);
}

static void record_failure_site(DWORD rva) {
    CallsiteStat* site = callsite_for(rva);
    if (site) InterlockedIncrement(&site->failures);
}

static bool insert_sample(void* pointer, SIZE_T size, DWORD alloc_rva) {
    if (!should_sample(pointer)) return false;
    const DWORD origin_rva = sampled_upstream_rva(alloc_rva);
    const DWORD hash = mix_pointer(pointer);
    // The low bits select the 1/SAMPLE_RATE cohort. Use different hash bits
    // for the table index or every sampled pointer clusters into a few slots.
    const unsigned start = (hash / SAMPLE_RATE) & (SAMPLE_CAPACITY - 1u);
    bool inserted = false;
    lock_samples();
    for (unsigned probe = 0; probe < SAMPLE_PROBES; ++probe) {
        SampleEntry& slot = g_samples[(start + probe) & (SAMPLE_CAPACITY - 1u)];
        if (!slot.pointer) {
            slot.size = size > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<DWORD>(size);
            slot.alloc_rva = origin_rva;
            slot.tick = GetTickCount();
            slot.pointer = pointer;
            inserted = true;
            break;
        }
    }
    unlock_samples();

    if (!inserted) {
        InterlockedIncrement(&g_sample_drops);
        return false;
    }

    InterlockedIncrement(&g_sample_entries);
    CallsiteStat* site = callsite_for(origin_rva);
    if (site) {
        InterlockedIncrement(&site->sample_live_count);
        InterlockedExchangeAdd64(&site->sample_live_bytes,
                                 size > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<LONG64>(size));
    }
    return true;
}

static bool remove_sample(void* pointer, DWORD* size, DWORD* alloc_rva, DWORD* lifetime_ms) {
    if (!should_sample(pointer)) return false;
    const DWORD hash = mix_pointer(pointer);
    const unsigned start = (hash / SAMPLE_RATE) & (SAMPLE_CAPACITY - 1u);
    SampleEntry found = {};

    lock_samples();
    for (unsigned probe = 0; probe < SAMPLE_PROBES; ++probe) {
        SampleEntry& slot = g_samples[(start + probe) & (SAMPLE_CAPACITY - 1u)];
        if (slot.pointer == pointer) {
            found = slot;
            slot.pointer = nullptr;
            slot.size = 0;
            slot.alloc_rva = 0;
            slot.tick = 0;
            break;
        }
    }
    unlock_samples();

    if (!found.pointer) return false;
    InterlockedDecrement(&g_sample_entries);
    if (size) *size = found.size;
    if (alloc_rva) *alloc_rva = found.alloc_rva;
    if (lifetime_ms) *lifetime_ms = GetTickCount() - found.tick;

    CallsiteStat* origin = callsite_for(found.alloc_rva);
    if (origin) {
        InterlockedDecrement(&origin->sample_live_count);
        InterlockedExchangeAdd64(&origin->sample_live_bytes,
                                 -static_cast<LONG64>(found.size));
        InterlockedIncrement(&origin->sample_free_count);
        InterlockedExchangeAdd64(&origin->sample_freed_bytes, found.size);
        InterlockedExchangeAdd64(&origin->sample_lifetime_ms,
                                 lifetime_ms ? *lifetime_ms : 0);
    }
    return true;
}

static void log_low_level_failure(const char* api, SIZE_T size, DWORD rva,
                                  bool reserve_released, bool recovered) {
    InterlockedIncrement(&g_heap_failures);
    record_failure_site(rva);
    logger::log(
        "LOW-LEVEL ALLOC FAILURE%s : %s size=%zu caller=mowas_2.exe+0x%08lX reserve_released=%s",
        recovered ? " RECOVERED" : "",
        api,
        size,
        static_cast<unsigned long>(rva),
        reserve_released ? "yes" : "no");
    log_summary(true);
    logger::flush_now();
}

static LPVOID WINAPI profiled_heap_alloc(HANDLE heap, DWORD flags, SIZE_T size) {
    const DWORD rva = caller_rva();
    InterlockedIncrement(&g_heap_alloc_calls);
    InterlockedExchangeAdd64(&g_heap_requested_bytes, static_cast<LONG64>(size));
    record_alloc_site(rva, size, false);

    LPVOID result = g_originalHeapAlloc(heap, flags, size);
    if (!result) {
        const bool released = memory_guard::release_after_failure();
        if (released) result = g_originalHeapAlloc(heap, flags, size);
        log_low_level_failure("HeapAlloc", size, rva, released, result != nullptr);
    }
    if (result) insert_sample(result, size, rva);
    return result;
}

static LPVOID WINAPI profiled_heap_realloc(HANDLE heap, DWORD flags,
                                           LPVOID pointer, SIZE_T size) {
    const DWORD rva = caller_rva();
    InterlockedIncrement(&g_heap_realloc_calls);
    InterlockedExchangeAdd64(&g_heap_requested_bytes, static_cast<LONG64>(size));
    record_alloc_site(rva, size, true);

    LPVOID result = g_originalHeapReAlloc(heap, flags, pointer, size);
    if (!result) {
        const bool released = memory_guard::release_after_failure();
        if (released) result = g_originalHeapReAlloc(heap, flags, pointer, size);
        log_low_level_failure("HeapReAlloc", size, rva, released, result != nullptr);
    }
    if (result) {
        remove_sample(pointer, nullptr, nullptr, nullptr);
        insert_sample(result, size, rva);
    }
    return result;
}

static BOOL WINAPI profiled_heap_free(HANDLE heap, DWORD flags, LPVOID pointer) {
    const DWORD rva = caller_rva();
    InterlockedIncrement(&g_heap_free_calls);
    const DWORD sampled_free_rva = should_sample(pointer)
        ? sampled_upstream_rva(rva) : 0;
    const BOOL result = g_originalHeapFree(heap, flags, pointer);
    if (result) {
        if (sampled_free_rva) record_free_site(sampled_free_rva);
        remove_sample(pointer, nullptr, nullptr, nullptr);
    }
    return result;
}

static LPVOID WINAPI profiled_virtual_alloc(LPVOID address, SIZE_T size,
                                            DWORD allocation_type, DWORD protect) {
    const DWORD rva = caller_rva();
    InterlockedIncrement(&g_virtual_alloc_calls);
    InterlockedExchangeAdd64(&g_virtual_requested_bytes, static_cast<LONG64>(size));

    LPVOID result = g_originalVirtualAlloc(address, size, allocation_type, protect);
    if (!result && size != 0) {
        const DWORD original_error = GetLastError();
        const bool released = memory_guard::release_after_failure();
        if (released) result = g_originalVirtualAlloc(address, size, allocation_type, protect);
        InterlockedIncrement(&g_virtual_failures);
        record_failure_site(rva);
        logger::log(
            "LOW-LEVEL VIRTUAL FAILURE%s : VirtualAlloc size=%zu type=0x%08lX caller=mowas_2.exe+0x%08lX error=%lu reserve_released=%s",
            result ? " RECOVERED" : "",
            size,
            static_cast<unsigned long>(allocation_type),
            static_cast<unsigned long>(rva),
            static_cast<unsigned long>(original_error),
            released ? "yes" : "no");
        log_summary(true);
        logger::flush_now();
        if (!result) SetLastError(original_error);
    }
    return result;
}

static BOOL WINAPI profiled_virtual_free(LPVOID address, SIZE_T size, DWORD free_type) {
    InterlockedIncrement(&g_virtual_free_calls);
    return g_originalVirtualFree(address, size, free_type);
}

struct IatPatch {
    const char* name;
    void* replacement;
    void** original;
    IMAGE_THUNK_DATA32* slot;
    DWORD old_value;
};

static bool patch_slot(IatPatch& patch) {
    DWORD old_protection = 0;
    if (!VirtualProtect(&patch.slot->u1.Function, sizeof(DWORD), PAGE_READWRITE,
                        &old_protection)) {
        return false;
    }
    patch.old_value = patch.slot->u1.Function;
    *patch.original = reinterpret_cast<void*>(static_cast<std::uintptr_t>(patch.old_value));
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&patch.slot->u1.Function),
                        static_cast<LONG>(reinterpret_cast<std::uintptr_t>(patch.replacement)));
    DWORD ignored = 0;
    const bool restored = VirtualProtect(&patch.slot->u1.Function, sizeof(DWORD),
                                         old_protection, &ignored) != FALSE;
    FlushInstructionCache(GetCurrentProcess(), &patch.slot->u1.Function, sizeof(DWORD));
    return restored;
}

static void restore_slot(IatPatch& patch) {
    if (!patch.slot || !patch.old_value) return;
    DWORD old_protection = 0;
    if (!VirtualProtect(&patch.slot->u1.Function, sizeof(DWORD), PAGE_READWRITE,
                        &old_protection)) return;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&patch.slot->u1.Function),
                        static_cast<LONG>(patch.old_value));
    DWORD ignored = 0;
    VirtualProtect(&patch.slot->u1.Function, sizeof(DWORD), old_protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), &patch.slot->u1.Function, sizeof(DWORD));
    *patch.original = nullptr;
}

static bool find_iat_slots(std::uintptr_t base, IatPatch* patches, unsigned count) {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return false;

    const DWORD image_size = nt->OptionalHeader.SizeOfImage;
    const auto& import_dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!import_dir.VirtualAddress || import_dir.VirtualAddress >= image_size) return false;

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + import_dir.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        if (descriptor->Name >= image_size) return false;
        const char* dll_name = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(dll_name, "KERNEL32.dll") != 0) continue;
        if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) return false;

        auto* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(
            base + descriptor->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) continue;
            if (names->u1.AddressOfData >= image_size) return false;
            auto* imported = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + names->u1.AddressOfData);
            const char* name = reinterpret_cast<const char*>(imported->Name);
            for (unsigned i = 0; i < count; ++i) {
                if (!patches[i].slot && std::strcmp(name, patches[i].name) == 0) {
                    patches[i].slot = iat;
                }
            }
        }
        return true;
    }
    return false;
}

struct RankedSite {
    DWORD rva;
    LONG calls;
    LONG frees;
    LONG sample_live_count;
    LONG64 requested;
    LONG64 sample_live;
    LONG64 sample_freed;
    LONG64 sample_lifetime;
    LONG sample_free_count;
    LONG engine_sample_calls;
    LONG64 engine_sample_bytes;
};

enum RankMode {
    RANK_REQUESTED,
    RANK_LIVE,
    RANK_FREES,
    RANK_ENGINE,
};

static void insert_top(RankedSite* top, unsigned capacity, const RankedSite& candidate,
                       RankMode mode) {
    const LONG64 score = mode == RANK_LIVE ? candidate.sample_live :
                         mode == RANK_FREES ? candidate.frees :
                         mode == RANK_ENGINE ? candidate.engine_sample_bytes :
                         candidate.requested;
    if (score <= 0) return;
    unsigned position = capacity;
    for (unsigned i = 0; i < capacity; ++i) {
        const LONG64 current = mode == RANK_LIVE ? top[i].sample_live :
                               mode == RANK_FREES ? top[i].frees :
                               mode == RANK_ENGINE ? top[i].engine_sample_bytes :
                               top[i].requested;
        if (score > current) {
            position = i;
            break;
        }
    }
    if (position == capacity) return;
    for (unsigned i = capacity - 1; i > position; --i) top[i] = top[i - 1];
    top[position] = candidate;
}

static DWORD WINAPI summary_thread(LPVOID) {
    DWORD elapsed = 0;
    for (;;) {
        Sleep(SUMMARY_INTERVAL_MS);
        elapsed += SUMMARY_INTERVAL_MS;
        const bool detailed = elapsed >= DETAIL_INTERVAL_MS;
        log_summary(detailed);
        if (detailed) elapsed = 0;
    }
}

} // anonymous namespace

void record_engine_request(std::size_t size, std::uintptr_t caller) {
    if (!g_installed) return;
    if ((++g_engine_sample_counter & (ENGINE_SAMPLE_RATE - 1u)) != 0) return;

    const DWORD rva = caller >= g_base && caller < g_base + TARGET_IMAGE_SIZE
        ? static_cast<DWORD>(caller - g_base) : 0xFFFFFFFFu;
    CallsiteStat* site = callsite_for(rva);
    if (!site) return;
    InterlockedIncrement(&site->engine_sample_calls);
    InterlockedExchangeAdd64(&site->engine_sample_bytes, static_cast<LONG64>(size));
}

void log_summary(bool detailed) {
    LONG64 sampled_live = 0;
    for (unsigned i = 0; i < CALLSITE_CAPACITY; ++i) {
        if (g_callsites[i].rva != 0) sampled_live += load64(&g_callsites[i].sample_live_bytes);
    }
    const unsigned long long estimated_live = sampled_live > 0
        ? static_cast<unsigned long long>(sampled_live) * SAMPLE_RATE : 0ull;

    logger::log(
        "ALLOC PROFILE : heap alloc=%ld realloc=%ld free=%ld failures=%ld requested=%llu MiB | virtual alloc=%ld free=%ld failures=%ld requested=%llu MiB | samples=%ld live=%llu KiB estimate=%llu MiB drops=%ld sites_dropped=%ld",
        static_cast<long>(g_heap_alloc_calls),
        static_cast<long>(g_heap_realloc_calls),
        static_cast<long>(g_heap_free_calls),
        static_cast<long>(g_heap_failures),
        static_cast<unsigned long long>(load64(&g_heap_requested_bytes)) / MiB,
        static_cast<long>(g_virtual_alloc_calls),
        static_cast<long>(g_virtual_free_calls),
        static_cast<long>(g_virtual_failures),
        static_cast<unsigned long long>(load64(&g_virtual_requested_bytes)) / MiB,
        static_cast<long>(g_sample_entries),
        static_cast<unsigned long long>(sampled_live) / 1024ull,
        estimated_live / MiB,
        static_cast<long>(g_sample_drops),
        static_cast<long>(g_callsite_drops));

    if (!detailed) return;
    constexpr unsigned TOP_COUNT = 6;
    RankedSite top_requested[TOP_COUNT] = {};
    RankedSite top_live[TOP_COUNT] = {};
    RankedSite top_frees[TOP_COUNT] = {};
    RankedSite top_engine[TOP_COUNT] = {};
    for (unsigned i = 0; i < CALLSITE_CAPACITY; ++i) {
        CallsiteStat& site = g_callsites[i];
        const LONG key = site.rva;
        if (!key) continue;
        RankedSite candidate = {};
        candidate.rva = static_cast<DWORD>(key);
        candidate.calls = site.alloc_calls + site.realloc_calls;
        candidate.frees = site.free_calls;
        candidate.sample_live_count = site.sample_live_count;
        candidate.requested = load64(&site.requested_bytes);
        candidate.sample_live = load64(&site.sample_live_bytes);
        candidate.sample_freed = load64(&site.sample_freed_bytes);
        candidate.sample_lifetime = load64(&site.sample_lifetime_ms);
        candidate.sample_free_count = site.sample_free_count;
        candidate.engine_sample_calls = site.engine_sample_calls;
        candidate.engine_sample_bytes = load64(&site.engine_sample_bytes);
        insert_top(top_requested, TOP_COUNT, candidate, RANK_REQUESTED);
        insert_top(top_live, TOP_COUNT, candidate, RANK_LIVE);
        insert_top(top_frees, TOP_COUNT, candidate, RANK_FREES);
        insert_top(top_engine, TOP_COUNT, candidate, RANK_ENGINE);
    }

    for (unsigned i = 0; i < TOP_COUNT && top_requested[i].requested > 0; ++i) {
        const RankedSite& site = top_requested[i];
        logger::log(
            "ALLOC TOP REQUEST #%u : mowas_2.exe+0x%08lX calls=%ld requested=%llu MiB sample_live=%llu KiB",
            i + 1,
            static_cast<unsigned long>(site.rva),
            static_cast<long>(site.calls),
            static_cast<unsigned long long>(site.requested) / MiB,
            static_cast<unsigned long long>(site.sample_live) / 1024ull);
    }
    for (unsigned i = 0; i < TOP_COUNT && top_live[i].sample_live > 0; ++i) {
        const RankedSite& site = top_live[i];
        const unsigned long long average_lifetime = site.sample_free_count > 0
            ? static_cast<unsigned long long>(site.sample_lifetime) /
              static_cast<unsigned long long>(site.sample_free_count)
            : 0ull;
        logger::log(
            "ALLOC TOP RETAINED #%u : mowas_2.exe+0x%08lX sample_live=%llu KiB (%ld blocks, est=%llu MiB) sampled_freed=%llu KiB avg_lifetime=%llu ms",
            i + 1,
            static_cast<unsigned long>(site.rva),
            static_cast<unsigned long long>(site.sample_live) / 1024ull,
            static_cast<long>(site.sample_live_count),
            static_cast<unsigned long long>(site.sample_live) * SAMPLE_RATE / MiB,
            static_cast<unsigned long long>(site.sample_freed) / 1024ull,
            average_lifetime);
    }
    for (unsigned i = 0; i < TOP_COUNT && top_frees[i].frees > 0; ++i) {
        const RankedSite& site = top_frees[i];
        logger::log(
            "ALLOC SAMPLED FREE #%u : mowas_2.exe+0x%08lX samples=%ld estimated_calls=%llu",
            i + 1,
            static_cast<unsigned long>(site.rva),
            static_cast<long>(site.frees),
            static_cast<unsigned long long>(site.frees) * SAMPLE_RATE);
    }
    for (unsigned i = 0; i < TOP_COUNT && top_engine[i].engine_sample_calls > 0; ++i) {
        const RankedSite& site = top_engine[i];
        logger::log(
            "ENGINE ALLOC SAMPLE #%u : mowas_2.exe+0x%08lX samples=%ld sampled=%llu MiB estimated_calls=%llu estimated_requested=%llu MiB",
            i + 1,
            static_cast<unsigned long>(site.rva),
            static_cast<long>(site.engine_sample_calls),
            static_cast<unsigned long long>(site.engine_sample_bytes) / MiB,
            static_cast<unsigned long long>(site.engine_sample_calls) * ENGINE_SAMPLE_RATE,
            static_cast<unsigned long long>(site.engine_sample_bytes) * ENGINE_SAMPLE_RATE / MiB);
    }
}

bool install(std::uintptr_t game_base) {
    g_base = game_base;
    IatPatch patches[] = {
        {"HeapAlloc", reinterpret_cast<void*>(&profiled_heap_alloc),
         reinterpret_cast<void**>(&g_originalHeapAlloc), nullptr, 0},
        {"HeapReAlloc", reinterpret_cast<void*>(&profiled_heap_realloc),
         reinterpret_cast<void**>(&g_originalHeapReAlloc), nullptr, 0},
        {"HeapFree", reinterpret_cast<void*>(&profiled_heap_free),
         reinterpret_cast<void**>(&g_originalHeapFree), nullptr, 0},
        {"VirtualAlloc", reinterpret_cast<void*>(&profiled_virtual_alloc),
         reinterpret_cast<void**>(&g_originalVirtualAlloc), nullptr, 0},
        {"VirtualFree", reinterpret_cast<void*>(&profiled_virtual_free),
         reinterpret_cast<void**>(&g_originalVirtualFree), nullptr, 0},
    };
    constexpr unsigned patch_count = sizeof(patches) / sizeof(patches[0]);
    if (!find_iat_slots(game_base, patches, patch_count)) {
        logger::log("ALLOC PROFILER install failed: KERNEL32 import table unavailable");
        return false;
    }
    for (unsigned i = 0; i < patch_count; ++i) {
        if (!patches[i].slot) {
            logger::log("ALLOC PROFILER install failed: %s import not found", patches[i].name);
            return false;
        }
    }

    unsigned installed = 0;
    for (; installed < patch_count; ++installed) {
        if (!patch_slot(patches[installed])) {
            logger::log("ALLOC PROFILER install failed: could not patch %s",
                        patches[installed].name);
            restore_slot(patches[installed]);
            while (installed > 0) restore_slot(patches[--installed]);
            return false;
        }
        logger::log("ALLOC PROFILER hook : %s IAT=%p original=%p",
                    patches[installed].name,
                    &patches[installed].slot->u1.Function,
                    *patches[installed].original);
    }

    HANDLE thread = CreateThread(nullptr, 0, &summary_thread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    else logger::log("ALLOC PROFILER : summary thread creation failed error=%lu", GetLastError());

    InterlockedExchange(&g_installed, 1);
    logger::log(
        "ALLOC PROFILER installed: game-only IAT observation, 1/%u lifetime sampling, 1/%u engine sampling, original allocators retained",
        SAMPLE_RATE, ENGINE_SAMPLE_RATE);
    return true;
}

namespace {
bool install_module(std::uintptr_t game_base) {
    return allocator_profiler::install(game_base);
}
}

MOWAS_REGISTER_MODULE(
    allocator_profiler_module,
    (::core::ModuleSpec{"allocator_profiler", 100, &install_module}));

} // namespace allocator_profiler
