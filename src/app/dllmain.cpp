// Keep DllMain loader-lock safe; logger + hooks init from a worker thread.

#include <windows.h>

#include "shared/logging/logger.h"
#include "orchestration/hooks.h"

static const DWORD HOOK_INSTALL_DELAY_MS = 3000;

static DWORD WINAPI delayed_init_thread(LPVOID) {
    if (!logger::init()) {
        OutputDebugStringA("mowas2-perf: logger::init() failed, continuing without log\n");
    }
    logger::log("delayed_init thread starting");

    Sleep(HOOK_INSTALL_DELAY_MS);

    logger::log("delayed_init: %lu ms elapsed, installing hooks now", HOOK_INSTALL_DELAY_MS);

    if (!hooks::install()) {
        logger::log("delayed_init: hooks::install FAILED - game will crash normally on the 2 GB bug");
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
            OutputDebugStringA("mowas2-perf: CreateThread for delayed_init FAILED\n");
        }
    }
    // No detach cleanup: FreeLibrary is unsupported for installed trampolines.
    return TRUE;
}
