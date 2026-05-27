#include "orchestration/hooks.h"
#include "orchestration/hooks_internal.h"
#include "app/config.h"
#include "shared/logging/logger.h"
#include "shared/core/module.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include "MinHook.h"

uintptr_t g_gameBase = 0;

namespace hooks {

static const uint8_t SIG_ENGINE_ALLOC[]      = { 0x55, 0x8B, 0xEC, 0x5D };
static const uint8_t SIG_ENGINE_ALLOC_CORE[] = { 0x55, 0x8B, 0xEC, 0xF0 };

static bool validate_hook_site(const uint8_t* p, const char* name,
                                const uint8_t* expected, size_t expected_len) {
    uint8_t actual[8] = {0};
    size_t n = expected_len > sizeof(actual) ? sizeof(actual) : expected_len;
    __try {
        for (size_t i = 0; i < n; ++i) actual[i] = p[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logger::log("VALIDATE %s : read at %p faulted", name, p);
        return false;
    }
    char hexbuf[48] = {0};
    int written = 0;
    for (size_t i = 0; i < n && written < (int)sizeof(hexbuf) - 4; ++i) {
        int w = _snprintf_s(hexbuf + written, sizeof(hexbuf) - written, _TRUNCATE,
                             "%s%02X", (i == 0 ? "" : " "), actual[i]);
        if (w < 0) break;
        written += w;
    }
    if (memcmp(actual, expected, n) != 0) {
        logger::log("VALIDATE %s @ %p : signature mismatch [got: %s] — wrong game build?",
                    name, p, hexbuf);
        return false;
    }
    logger::log("VALIDATE %s @ %p : signature OK [%s]", name, p, hexbuf);
    return true;
}

bool install() {
    MH_STATUS init_status = MH_Initialize();
    if (init_status != MH_OK && init_status != MH_ERROR_ALREADY_INITIALIZED) {
        logger::log("MH_Initialize failed : status=%d", (int)init_status);
        return false;
    }
    bool mh_owned_here = (init_status == MH_OK);

    HMODULE hGame = GetModuleHandleA("mowas_2.exe");
    if (!hGame) {
        logger::log("GetModuleHandleA(mowas_2.exe) failed — aborting");
        if (mh_owned_here) MH_Uninitialize();
        return false;
    }
    uintptr_t base = reinterpret_cast<uintptr_t>(hGame);
    g_gameBase = base;
    logger::log("mowas_2.exe loaded at 0x%p (preferred=0x%X, ASLR shift=0x%X)",
                hGame, MOWAS2_PREFERRED_BASE,
                (unsigned)(base - MOWAS2_PREFERRED_BASE));

    struct MandatoryHook {
        const char* name;
        void*       addr;
        void*       target;
        void**      trampoline;
    };
    void* allocAddr     = reinterpret_cast<void*>(base + OFFSET_ENGINE_ALLOC);
    void* allocCoreAddr = reinterpret_cast<void*>(base + OFFSET_ENGINE_ALLOC_CORE);
    if (!validate_hook_site((const uint8_t*)allocAddr, "engineAlloc",
                             SIG_ENGINE_ALLOC, sizeof(SIG_ENGINE_ALLOC)) ||
        !validate_hook_site((const uint8_t*)allocCoreAddr, "engineAllocCore",
                             SIG_ENGINE_ALLOC_CORE, sizeof(SIG_ENGINE_ALLOC_CORE))) {
        logger::log("FATAL : hook-site validation failed — aborting install");
        if (mh_owned_here) MH_Uninitialize();
        return false;
    }
    MandatoryHook mandatory[] = {
        { "engineAlloc (FUN_00c09041)", allocAddr,
          reinterpret_cast<void*>(&hooked_engineAlloc),
          reinterpret_cast<void**>(&g_origEngineAlloc) },
        { "engineAllocCore (FUN_0050e4f0)", allocCoreAddr,
          reinterpret_cast<void*>(&hooked_engineAllocCore),
          reinterpret_cast<void**>(&g_origEngineAllocCore) },
    };
    void* installed_addrs[sizeof(mandatory)/sizeof(mandatory[0])] = {nullptr};
    int installed_count = 0;
    for (const auto& mh : mandatory) {
        MH_STATUS create_s = MH_CreateHook(mh.addr, mh.target, mh.trampoline);
        MH_STATUS enable_s = MH_OK;
        if (create_s == MH_OK) {
            enable_s = MH_EnableHook(mh.addr);
            if (enable_s != MH_OK) {
                MH_RemoveHook(mh.addr);
                *mh.trampoline = nullptr;
            }
        }
        MH_STATUS mhs = (create_s != MH_OK) ? create_s : enable_s;
        if (mhs != MH_OK) {
            logger::log("FATAL : hook %s @ 0x%p FAILED status=%d — rolling back",
                        mh.name, mh.addr, (int)mhs);
            for (int i = 0; i < installed_count; ++i) {
                MH_RemoveHook(installed_addrs[i]);
            }
            g_origEngineAlloc = nullptr;
            g_origEngineAllocCore = nullptr;
            if (mh_owned_here) MH_Uninitialize();
            return false;
        }
        installed_addrs[installed_count++] = mh.addr;
        logger::log("Hook installed : %s @ 0x%p", mh.name, mh.addr);
    }

    int installed = ::core::install_all(base);
    logger::log("Module registry : %d modules installed", installed);
    return true;
}

}  // namespace hooks
