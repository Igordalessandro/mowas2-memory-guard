// dbghelp.dll runtime proxy : forwards ImageNtHeader to the real system DLL.
// Windows dedups loaded modules by base name, so we copy the real one to
// %TEMP% and LoadLibrary that copy.

#include "shared/logging/logger.h"

#include <windows.h>
#include <cstdio>
#include <cwchar>

#pragma comment(linker, "/EXPORT:ImageNtHeader=_proxied_ImageNtHeader@4")

namespace {

using PFN_ImageNtHeader = PIMAGE_NT_HEADERS (WINAPI *)(PVOID);

constexpr LONG STATE_PENDING       = 0;
constexpr LONG STATE_BUSY          = 1;
constexpr LONG STATE_DONE          = 2;
constexpr LONG STATE_PERMANENT_FAIL = 3;

static volatile LONG     g_initOnce        = STATE_PENDING;
static volatile LONG     g_initFailures    = 0;
static HMODULE           g_realDbghelp     = nullptr;
static PFN_ImageNtHeader g_pfnImageNtHeader = nullptr;

constexpr int INIT_MAX_RETRIES   = 5;
constexpr int INIT_SPIN_WAIT_MS  = 500;

static void fail_init(bool permanent) {
    LONG fails = InterlockedIncrement(&g_initFailures);
    InterlockedExchange(&g_initOnce,
        (permanent || fails >= INIT_MAX_RETRIES) ? STATE_PERMANENT_FAIL : STATE_PENDING);
}

// Lazy : DllMain's loader-lock forbids CopyFileW / LoadLibraryW here.
static void ensure_init() {
    logger::init();
    for (;;) {
        LONG prev = InterlockedCompareExchange(&g_initOnce, STATE_BUSY, STATE_PENDING);
        if (prev == STATE_DONE || prev == STATE_PERMANENT_FAIL) return;
        if (prev == STATE_PENDING) break;
        bool owner_failed = false;
        for (int i = 0; i < INIT_SPIN_WAIT_MS; ++i) {
            LONG s = g_initOnce;
            if (s == STATE_DONE || s == STATE_PERMANENT_FAIL) return;
            if (s == STATE_PENDING) { owner_failed = true; break; }
            Sleep(1);
        }
        if (!owner_failed) return;
    }

    wchar_t sysDir[MAX_PATH] = {0};
    UINT len = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH - 16) {
        logger::log("dbghelp_proxy : GetSystemDirectoryW failed (err=%lu)",
                    GetLastError());
        fail_init(false);
        return;
    }
    wchar_t srcPath[MAX_PATH] = {0};
    swprintf_s(srcPath, MAX_PATH, L"%s\\dbghelp.dll", sysDir);

    wchar_t tempDir[MAX_PATH] = {0};
    DWORD tlen = GetTempPathW(MAX_PATH, tempDir);
    if (tlen == 0 || tlen >= MAX_PATH - 48) {
        logger::log("dbghelp_proxy : GetTempPathW failed (err=%lu)",
                    GetLastError());
        fail_init(false);
        return;
    }
    wchar_t copyPath[MAX_PATH] = {0};
    swprintf_s(copyPath, MAX_PATH, L"%smowas2_real_dbghelp.dll", tempDir);

    // SHARING_VIOLATION = previous run still maps the file, OK to reuse.
    // Anything else : refuse to load a possibly stale/tampered temp file.
    bool can_load = true;
    if (!CopyFileW(srcPath, copyPath, FALSE)) {
        DWORD err = GetLastError();
        if (err != ERROR_SHARING_VIOLATION) {
            logger::log("dbghelp_proxy : CopyFile FAILED src=%ws dst=%ws err=%lu — refusing to LoadLibrary stale temp file",
                        srcPath, copyPath, err);
            can_load = false;
        }
    }
    if (!can_load) {
        fail_init(false);
        return;
    }

    g_realDbghelp = LoadLibraryW(copyPath);
    if (!g_realDbghelp) {
        logger::log("dbghelp_proxy : LoadLibrary FAILED %ws err=%lu",
                    copyPath, GetLastError());
        fail_init(false);
        return;
    }

    HMODULE selfHandle = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)&ensure_init, &selfHandle) || !selfHandle) {
        logger::log("dbghelp_proxy : GetModuleHandleExW(self) FAILED err=%lu — cannot verify dedup, refusing to forward",
                    GetLastError());
        FreeLibrary(g_realDbghelp);
        g_realDbghelp = nullptr;
        fail_init(true);
        return;
    }
    if (g_realDbghelp == selfHandle) {
        logger::log("dbghelp_proxy : dedup FAILED (LoadLibrary returned self!)");
        FreeLibrary(g_realDbghelp);
        g_realDbghelp = nullptr;
        fail_init(true);
        return;
    }

    g_pfnImageNtHeader = reinterpret_cast<PFN_ImageNtHeader>(
        GetProcAddress(g_realDbghelp, "ImageNtHeader"));
    if (!g_pfnImageNtHeader) {
        logger::log("dbghelp_proxy : GetProcAddress(ImageNtHeader) FAILED err=%lu",
                    GetLastError());
        FreeLibrary(g_realDbghelp);
        g_realDbghelp = nullptr;
        fail_init(true);
        return;
    }
    logger::log("dbghelp_proxy : forwarded to %ws (handle=%p, self=%p)",
                copyPath, g_realDbghelp, selfHandle);
    InterlockedExchange(&g_initFailures, 0);
    InterlockedExchange(&g_initOnce, STATE_DONE);
}

} // anonymous namespace

extern "C" PIMAGE_NT_HEADERS WINAPI
proxied_ImageNtHeader(PVOID Base) {
    ensure_init();
    if (!g_pfnImageNtHeader) return nullptr;
    return g_pfnImageNtHeader(Base);
}
