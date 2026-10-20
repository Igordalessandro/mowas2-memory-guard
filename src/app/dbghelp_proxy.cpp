// dbghelp.dll runtime proxy : forwards the APIs used by the game to the real
// system DLL.  In particular, MiniDumpWriteDump must be exported or the game's
// own crash reporter cannot preserve a useful dump.
// Windows dedups loaded modules by base name, so we copy the real one to
// %TEMP% and LoadLibrary that copy.

#include "shared/logging/logger.h"
#include "app/dbghelp_proxy.h"

// windows.h may declare the system ImageNtHeader import.  Rename that
// declaration in this translation unit because this file defines the proxy
// export with the real name.
#define ImageNtHeader SystemDeclared_ImageNtHeader
#include <windows.h>
#undef ImageNtHeader
#include <cstdio>
#include <cstring>
#include <cwchar>

#if defined(_MSC_VER) && !defined(__MINGW32__)
#pragma comment(linker, "/EXPORT:ImageNtHeader=_ImageNtHeader@4")
#pragma comment(linker, "/EXPORT:MiniDumpWriteDump=_MiniDumpWriteDump@28")
#endif

namespace {

using PFN_ImageNtHeader = PIMAGE_NT_HEADERS (WINAPI *)(PVOID);
using PFN_MiniDumpWriteDump = BOOL (WINAPI *)(
    HANDLE, DWORD, HANDLE, DWORD, PVOID, PVOID, PVOID);

constexpr LONG STATE_PENDING       = 0;
constexpr LONG STATE_BUSY          = 1;
constexpr LONG STATE_DONE          = 2;
constexpr LONG STATE_PERMANENT_FAIL = 3;

static volatile LONG     g_initOnce        = STATE_PENDING;
static volatile LONG     g_initFailures    = 0;
static HMODULE           g_realDbghelp     = nullptr;
static PFN_ImageNtHeader g_pfnImageNtHeader = nullptr;
static PFN_MiniDumpWriteDump g_pfnMiniDumpWriteDump = nullptr;

constexpr int INIT_MAX_RETRIES   = 5;
constexpr int INIT_SPIN_WAIT_MS  = 500;

static void fail_init(bool permanent) {
    LONG fails = InterlockedIncrement(&g_initFailures);
    InterlockedExchange(&g_initOnce,
        (permanent || fails >= INIT_MAX_RETRIES) ? STATE_PERMANENT_FAIL : STATE_PENDING);
}

// A mapped copy can legitimately prevent CopyFile from overwriting it. Only
// reuse that copy if it still matches this installation's system DLL. Compare
// sequentially with a fixed 8 KiB stack budget; no allocation is needed here.
static bool same_file_contents(const wchar_t* source, const wchar_t* candidate) {
    struct ReadHandle {
        HANDLE value;
        ~ReadHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    };
    // Deny writers while comparing. An unreadable or concurrently writable
    // copy is not evidence that the existing file is safe to reuse.
    ReadHandle original{CreateFileW(source, GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING,
                                    FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (original.value == INVALID_HANDLE_VALUE) return false;
    ReadHandle copy{CreateFileW(candidate, GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING,
                                FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (copy.value == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER source_size = {};
    LARGE_INTEGER copy_size = {};
    constexpr LONGLONG MAX_COPY_BYTES = 64LL * 1024LL * 1024LL;
    if (!GetFileSizeEx(original.value, &source_size) ||
        !GetFileSizeEx(copy.value, &copy_size) || source_size.QuadPart <= 0 ||
        source_size.QuadPart > MAX_COPY_BYTES ||
        source_size.QuadPart != copy_size.QuadPart) return false;

    unsigned char source_bytes[4096];
    unsigned char copy_bytes[sizeof(source_bytes)];
    LONGLONG remaining = source_size.QuadPart;
    while (remaining > 0) {
        const DWORD requested = remaining < static_cast<LONGLONG>(sizeof(source_bytes))
            ? static_cast<DWORD>(remaining) : static_cast<DWORD>(sizeof(source_bytes));
        DWORD source_read = 0;
        DWORD copy_read = 0;
        if (!ReadFile(original.value, source_bytes, requested, &source_read, nullptr) ||
            !ReadFile(copy.value, copy_bytes, requested, &copy_read, nullptr) ||
            source_read != requested || copy_read != requested ||
            std::memcmp(source_bytes, copy_bytes, requested) != 0) return false;
        remaining -= requested;
    }
    return true;
}

// Lazy : DllMain's loader-lock forbids CopyFileW / LoadLibraryW here.
static void ensure_init() {
    // The editor and Workshop utility load this proxy from the shared game
    // directory. Forward dbghelp for them, but do not create/overwrite game
    // telemetry or the compact crash report. DllMain independently applies
    // the same executable gate before starting any game-only worker.
    if (GetModuleHandleW(L"mowas_2.exe")) {
        logger::init();
    }
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

    // CopyFile preserves the system DLL's read-only attribute.  Without
    // clearing it, every launch after the first fails with ACCESS_DENIED.
    // Ignore FILE_NOT_FOUND on the first run.
    SetFileAttributesW(copyPath, FILE_ATTRIBUTE_NORMAL);

    // A previous process may still map the copy. Reuse only after verifying
    // its complete contents against the current system DLL.
    bool can_load = true;
    if (!CopyFileW(srcPath, copyPath, FALSE)) {
        DWORD err = GetLastError();
        if (err == ERROR_SHARING_VIOLATION) {
            if (!same_file_contents(srcPath, copyPath)) {
                logger::log("dbghelp_proxy : locked temp copy differs from system DLL or could not be verified; refusing to LoadLibrary %ws",
                            copyPath);
                can_load = false;
            }
        } else {
            logger::log("dbghelp_proxy : CopyFile FAILED src=%ws dst=%ws err=%lu — refusing to LoadLibrary stale temp file",
                        srcPath, copyPath, err);
            can_load = false;
        }
    }
    SetFileAttributesW(copyPath, FILE_ATTRIBUTE_NORMAL);
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
    g_pfnMiniDumpWriteDump = reinterpret_cast<PFN_MiniDumpWriteDump>(
        GetProcAddress(g_realDbghelp, "MiniDumpWriteDump"));
    if (!g_pfnMiniDumpWriteDump) {
        logger::log("dbghelp_proxy : GetProcAddress(MiniDumpWriteDump) FAILED err=%lu; crash dumps unavailable",
                    GetLastError());
    }
    logger::log("dbghelp_proxy : forwarded to %ws (handle=%p, self=%p)",
                copyPath, g_realDbghelp, selfHandle);
    InterlockedExchange(&g_initFailures, 0);
    InterlockedExchange(&g_initOnce, STATE_DONE);
}

} // anonymous namespace

bool dbghelp_proxy::prewarm() {
    ensure_init();
    return InterlockedCompareExchange(&g_initOnce, 0, 0) == STATE_DONE &&
           g_pfnImageNtHeader && g_pfnMiniDumpWriteDump;
}

extern "C" __declspec(dllexport) PIMAGE_NT_HEADERS WINAPI
ImageNtHeader(PVOID Base) {
    const DWORD incoming_error = GetLastError();
    ensure_init();
    if (InterlockedCompareExchange(&g_initOnce, 0, 0) != STATE_DONE ||
        !g_pfnImageNtHeader) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    SetLastError(incoming_error);
    return g_pfnImageNtHeader(Base);
}

extern "C" __declspec(dllexport) BOOL WINAPI
MiniDumpWriteDump(HANDLE process, DWORD process_id, HANDLE file,
                  DWORD dump_type, PVOID exception_info,
                  PVOID user_stream_info, PVOID callback_info) {
    const DWORD incoming_error = GetLastError();
    ensure_init();
    if (InterlockedCompareExchange(&g_initOnce, 0, 0) != STATE_DONE ||
        !g_pfnMiniDumpWriteDump) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }
    SetLastError(incoming_error);
    return g_pfnMiniDumpWriteDump(process, process_id, file, dump_type,
                                  exception_info, user_stream_info, callback_info);
}
