#include "orchestration/hooks_internal.h"
#include "app/config.h"
#include "shared/logging/logger.h"

#include <windows.h>
#include <cstdio>

PFN_EngineAlloc g_origEngineAlloc     = nullptr;
PFN_EngineAlloc g_origEngineAllocCore = nullptr;

static volatile LONG g_clampCount = 0;

static inline size_t apply_clamp(size_t size, const char* hookName) {
    if (size <= ALLOC_SANITY_LIMIT) return size;

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
    LONG count = InterlockedIncrement(&g_clampCount);
    logger::log("ALLOC CLAMPED #%ld (%s) : %zu bytes -> %d bytes [stack: %s]",
                (long)count, hookName, size, ALLOC_CLAMP_REPLACEMENT, stackStr);
    return ALLOC_CLAMP_REPLACEMENT;
}

void* __cdecl hooked_engineAlloc(size_t size) {
    size = apply_clamp(size, "engineAlloc");
    return g_origEngineAlloc ? g_origEngineAlloc(size) : nullptr;
}

void* __cdecl hooked_engineAllocCore(size_t size) {
    size = apply_clamp(size, "engineAllocCore");
    return g_origEngineAllocCore ? g_origEngineAllocCore(size) : nullptr;
}
