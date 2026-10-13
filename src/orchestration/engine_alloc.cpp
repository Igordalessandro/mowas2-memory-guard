#include "orchestration/hooks_internal.h"
#include "app/config.h"
#include "diagnostics/allocator_profiler.h"
#include "orchestration/memory_guard.h"
#include "shared/logging/logger.h"

#include <windows.h>
#include <intrin.h>
#include <cstdio>

PFN_EngineAlloc g_origEngineAlloc     = nullptr;
PFN_EngineAlloc g_origEngineAllocCore = nullptr;

static volatile LONG g_workaroundCount = 0;
static thread_local int g_hookDepth = 0;

struct HookDepthScope {
    bool outermost;
    HookDepthScope() : outermost(g_hookDepth++ == 0) {}
    ~HookDepthScope() { --g_hookDepth; }
};

static inline size_t apply_known_lobby_workaround(size_t size, const char* hookName) {
    if (size != KNOWN_BAD_LOBBY_ALLOCATION) return size;

    void* stack[8] = {0};
    USHORT nFrames = RtlCaptureStackBackTrace(0, 8, stack, nullptr);
    char stackStr[256] = {0};
    int written = 0;
    for (USHORT i = 0; i < nFrames && written < (int)sizeof(stackStr) - 20; ++i) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(stack[i]);
        uintptr_t off  = (g_gameBase != 0 && addr >= g_gameBase) ? (addr - g_gameBase) : addr;
        int n = _snprintf_s(stackStr + written, sizeof(stackStr) - written,
                            _TRUNCATE,
                            "%s+0x%X", (i == 0 ? "" : " | "), (unsigned)off);
        if (n < 0) break;
        written += n;
    }
    LONG count = InterlockedIncrement(&g_workaroundCount);
    logger::log("KNOWN LOBBY BUG WORKAROUND #%ld (%s) : %zu bytes -> %zu bytes [stack: %s]",
                (long)count, hookName, size, LOBBY_ALLOCATION_REPLACEMENT, stackStr);
    return LOBBY_ALLOCATION_REPLACEMENT;
}

static void* guarded_allocate(PFN_EngineAlloc original, size_t size, const char* hookName,
                              std::uintptr_t caller) {
    if (!original) return nullptr;

    HookDepthScope depth;
    if (depth.outermost) {
        size = apply_known_lobby_workaround(size, hookName);
        allocator_profiler::record_engine_request(size, caller);
        memory_guard::record_request(size);
        memory_guard::before_allocation(size);
    }

    void* result = original(size);
    if (depth.outermost && !result) {
        const bool released = memory_guard::release_after_failure();
        if (released) result = original(size);
        memory_guard::record_failure(size, hookName, released, result != nullptr);
    }
    return result;
}

void* __cdecl hooked_engineAlloc(size_t size) {
    return guarded_allocate(g_origEngineAlloc, size, "engineAlloc",
                            reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
}

void* __cdecl hooked_engineAllocCore(size_t size) {
    return guarded_allocate(g_origEngineAllocCore, size, "engineAllocCore",
                            reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
}
