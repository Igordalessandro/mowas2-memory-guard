// Compiles the actual memory guard with a deterministic VirtualQuery map.
// Exercises release diagnostics and error preservation without starting the
// monitor, loading a game, allocating reserves, or inspecting a live process.
#include <windows.h>
#include <psapi.h>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {
struct Region { std::uintptr_t base; SIZE_T size; DWORD state; };
std::vector<Region> regions;
std::vector<std::string> messages;
std::string failure_memory;
DWORD test_error = 0;
DWORD maximum_address = 0x0fffffffu;
unsigned queries = 0;
unsigned releases = 0;
bool fail_query = false;

DWORD WINAPI mock_get_last_error() { return test_error; }
void WINAPI mock_set_last_error(DWORD error) { test_error = error; }
DWORD WINAPI mock_thread_id() { return 17; }
DWORD WINAPI mock_tick() { return 1234567; }
void WINAPI mock_system_info(LPSYSTEM_INFO info) {
    *info = {};
    info->lpMinimumApplicationAddress = reinterpret_cast<void*>(0x10000);
    info->lpMaximumApplicationAddress = reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(maximum_address));
    info->dwAllocationGranularity = 0x10000;
    test_error = 0x101;
}
SIZE_T WINAPI mock_query(LPCVOID address, PMEMORY_BASIC_INFORMATION out, SIZE_T) {
    ++queries;
    test_error = 0x102;
    if (fail_query) return 0;
    const auto requested = reinterpret_cast<std::uintptr_t>(address);
    for (const Region& region : regions) {
        if (requested >= region.base &&
            static_cast<std::uint64_t>(requested) <
            static_cast<std::uint64_t>(region.base) + region.size) {
            *out = {};
            out->BaseAddress = reinterpret_cast<void*>(region.base);
            out->RegionSize = region.size;
            out->State = region.state;
            return sizeof(*out);
        }
    }
    return 0;
}
BOOL WINAPI mock_free(LPVOID address, SIZE_T, DWORD flags) {
    assert(flags == MEM_RELEASE);
    test_error = 0x103;
    for (std::size_t i = 0; i < regions.size(); ++i) {
        if (regions[i].base != reinterpret_cast<std::uintptr_t>(address)) continue;
        assert(regions[i].state == MEM_RESERVE);
        regions[i].state = MEM_FREE;
        ++releases;
        for (std::size_t j = 1; j < regions.size();) {
            if (regions[j - 1].state == MEM_FREE && regions[j].state == MEM_FREE) {
                regions[j - 1].size += regions[j].size;
                regions.erase(regions.begin() + static_cast<std::ptrdiff_t>(j));
            } else ++j;
        }
        return TRUE;
    }
    assert(false);
    return FALSE;
}
}

#define GetLastError mock_get_last_error
#define SetLastError mock_set_last_error
#define GetSystemInfo mock_system_info
#define GetCurrentThreadId mock_thread_id
#define GetTickCount mock_tick
#define VirtualQuery mock_query
#define VirtualFree mock_free
#include "../src/orchestration/memory_guard.cpp"
#undef GetLastError
#undef SetLastError
#undef GetSystemInfo
#undef GetCurrentThreadId
#undef GetTickCount
#undef VirtualQuery
#undef VirtualFree

namespace logger {
void log(const char* fmt, ...) {
    char line[2048] = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    messages.emplace_back(line);
    test_error = 0x104;
}
void report_failure_memory(const char* fmt, ...) {
    char line[512] = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    failure_memory = line;
    test_error = 0x105;
}
void report_memory(const char*, ...) {}
void report_event(const char*, ...) {}
void report_failure(const char*, ...) {}
void flush_now() {}
}

namespace {
constexpr SIZE_T TestMiB = 1024u * 1024u;
void setup() {
    using namespace memory_guard;
    regions.clear();
    messages.clear();
    failure_memory.clear();
    releases = queries = 0;
    fail_query = false;
    maximum_address = 0x0fffffffu;
    g_primaryReserveSize = 8 * TestMiB;
    g_criticalReserveSize = 8 * TestMiB;
    g_finalReserveSize = 48 * TestMiB;
    g_primaryReserve = reinterpret_cast<void*>(0x10000);
    g_criticalReserve = reinterpret_cast<void*>(0x810000);
    g_finalReserve = reinterpret_cast<void*>(0x1010000);
    g_recovery.initialize(reinterpret_cast<void*>(0x4010000), 32 * TestMiB);
    g_releaseEvents = 0;
    regions.push_back({0x10000, 8 * TestMiB, MEM_RESERVE});
    regions.push_back({0x810000, 8 * TestMiB, MEM_RESERVE});
    regions.push_back({0x1010000, 48 * TestMiB, MEM_RESERVE});
    regions.push_back({0x4010000, 32 * TestMiB, MEM_RESERVE});
    regions.push_back({0x6010000, 0x0fc00000 - 0x6010000, MEM_COMMIT});
    regions.push_back({0x0fc00000, 4 * TestMiB, MEM_FREE});
    test_error = 0xbeef;
}
}

int main() {
    using namespace memory_guard;
    setup();
    // Ordinary allocations remain scan-free and log-free.
    before_allocation(4096, 0x1234);
    assert(queries == 0 && messages.empty() && test_error == 0xbeef);

    // A 20 MiB request needs 24 MiB headroom: the original sequential
    // release decisions remain unchanged, and the recovery reserve is held.
    before_allocation(20 * TestMiB + 19, 0x00376136);
    assert(releases == 3 && messages.size() == 3);
    assert(test_error == 0xbeef && g_recovery.snapshot().reserve.bytes == 32 * TestMiB);
    assert(messages[0].find("stage=primary bytes=20971539 site=+00376136 thread=17 tick=1234567") != std::string::npos);
    assert(messages[0].find("before_free=4096KiB before_largest=4096KiB after_free=12288KiB after_largest=8192KiB") != std::string::npos);
    assert(messages[2].find("stage=final") != std::string::npos);
    before_allocation(20 * TestMiB + 19, 0x9999);
    assert(messages.size() == 3 && releases == 3);

    // The monitor may restore the primary stage after sustained recovery.
    // Its next real release must retain the new triggering caller as well.
    g_primaryReserve = reinterpret_cast<void*>(0x10000);
    assert(regions[0].base == 0x10000 && regions[0].size == 64 * TestMiB);
    regions[0] = {0x10000, 8 * TestMiB, MEM_RESERVE};
    regions.insert(regions.begin() + 1, {0x810000, 56 * TestMiB, MEM_FREE});
    before_allocation(60 * TestMiB, 0x0088abcd);
    assert(releases == 4 && messages.size() == 4 && test_error == 0xbeef);
    assert(messages.back().find("stage=primary bytes=62914560 site=+0088ABCD") != std::string::npos);

    capture_failure_snapshot("HeapAlloc", 16777235, 0x00376136, "initial");
    assert(test_error == 0xbeef);
    assert(messages.back().find("api=HeapAlloc bytes=16777235 site=+00376136 phase=initial") != std::string::npos);
    assert(messages.back().find("fit4/8/16/32MiB=2/1/1/1 complete=1") != std::string::npos);
    assert(failure_memory.find("phase=initial") != std::string::npos);
    assert(failure_memory.size() < 160);
    fail_query = true;
    capture_failure_snapshot("HeapAlloc", 1, 0, "afterretry");
    assert(test_error == 0xbeef);
    assert(messages.back().find("complete=0") != std::string::npos);
    assert(failure_memory.find("phase=afterretry") != std::string::npos);

    // The last x86 region ends at 4 GiB; completion uses wide arithmetic.
    setup();
    maximum_address = 0xffffffffu;
    regions = {{0x10000, 0xfdff0000u, MEM_COMMIT},
               {0xfe000000u, 32 * TestMiB, MEM_FREE}};
    const auto final = scan_address_space();
    assert(final.complete && final.holes.counts[3] == 1);
    assert(final.free_bytes == 32ull * TestMiB);

    // Raw largest-free policy size is unchanged even when alignment prevents
    // a same-size new reservation, and periodic detail uses the same histogram.
    setup();
    regions = {{0x10000, 0x1000, MEM_COMMIT},
               {0x11000, 4 * TestMiB, MEM_FREE},
               {0x411000, 0x10000000 - 0x411000, MEM_COMMIT}};
    const auto misaligned = scan_address_space();
    assert(misaligned.complete && misaligned.largest_free == 4 * TestMiB);
    assert(misaligned.holes.counts[0] == 0);
    log_snapshot(misaligned, false);
    assert(messages.back().find("MEM HOLES : fit4/8/16/32MiB=0/0/0/0 complete=1") != std::string::npos);
    return 0;
}
