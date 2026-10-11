// Keep DllMain loader-lock safe; logger + hooks init from a worker thread.

#include <windows.h>

#include "shared/logging/logger.h"
#include "orchestration/hooks.h"
#include "orchestration/memory_guard.h"
#include "safety/vac_host_guard.h"
#include "safety/vac_join_guard.h"

static const DWORD HOOK_INSTALL_DELAY_MS = 3000;

static DWORD WINAPI delayed_init_thread(LPVOID) {
    if (!logger::init()) {
        OutputDebugStringA("mowas2-perf: logger::init() failed, continuing without log\n");
    }
    logger::log("delayed_init thread starting");

    // Install the host-safety boundary before the delayed allocator hooks.  If
    // it cannot be installed, stop the process before a secured host can be
    // created with this custom DLL loaded.
    if (!vac_host_guard::install()) {
        logger::log("delayed_init: VAC host guard FAILED; exiting fail-closed");
        logger::flush_now();
        MessageBoxW(
            nullptr,
            L"Memory Guard could not install its secure-host safeguard.\n\n"
            L"The game will close so it cannot accidentally create a "
            L"VAC-secured host with the custom DLL loaded.",
            L"Memory Guard - Safety check failed",
            MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        ExitProcess(ERROR_ACCESS_DISABLED_BY_POLICY);
        return ERROR_ACCESS_DISABLED_BY_POLICY;
    }

    if (!memory_guard::install()) {
        logger::log("delayed_init: memory guard installed without an emergency reserve");
    }

    Sleep(HOOK_INSTALL_DELAY_MS);

    logger::log("delayed_init: %lu ms elapsed, installing client and allocator hooks now",
                HOOK_INSTALL_DELAY_MS);

    if (!vac_join_guard::install()) {
        logger::log("delayed_init: VAC join guard FAILED; exiting fail-closed");
        logger::flush_now();
        MessageBoxW(
            nullptr,
            L"Memory Guard could not install its secure-server join safeguard.\n\n"
            L"The game will close so it cannot accidentally join a "
            L"VAC-secured server with the custom DLL loaded.",
            L"Memory Guard - Safety check failed",
            MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        ExitProcess(ERROR_ACCESS_DISABLED_BY_POLICY);
        return ERROR_ACCESS_DISABLED_BY_POLICY;
    }

    if (!hooks::install()) {
        logger::log("delayed_init: hooks::install FAILED - telemetry remains active but allocator interception is disabled");
    } else {
        logger::log("delayed_init: hooks installed successfully");
    }

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*lpReserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        HANDLE th = CreateThread(nullptr, 0, &delayed_init_thread, nullptr, 0, nullptr);
        if (th) {
            CloseHandle(th);
        } else {
            // Returning FALSE from a load-time DllMain aborts startup.  Do not
            // allow the proxy DLL to load without its host-safety worker.
            OutputDebugStringA("mowas2-perf: CreateThread for delayed_init FAILED; aborting load\n");
            return FALSE;
        }
    }
    // No detach cleanup: FreeLibrary is unsupported for installed trampolines.
    return TRUE;
}
