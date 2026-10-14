#include "safety/vac_join_guard.h"

#include "shared/logging/logger.h"

#include <windows.h>
#include <cstdint>

namespace vac_join_guard {
namespace {

// SteamUser018 provides a late boundary before the game obtains the
// authentication blob used by a secure game server.  Do not hook or query
// SteamMatchmaking here: even an interface-local JoinLobby substitution was
// observed to destabilize friend/persona names under Proton's WoW64 bridge.
using SteamUserAccessorFn = void* (__cdecl *)();
using InitiateGameConnectionFn = int (__thiscall *)(
    void* self,
    void* auth_blob,
    int auth_blob_capacity,
    std::uint64_t game_server_steam_id,
    std::uint32_t server_ip,
    std::uint16_t server_port,
    bool secure);

InitiateGameConnectionFn g_original_initiate = nullptr;
void* g_initiate_target = nullptr;
void** g_initiate_slot = nullptr;
volatile LONG g_installed = 0;
volatile LONG g_warning_visible = 0;

bool is_executable_address(void* address) {
    MEMORY_BASIC_INFORMATION info = {};
    if (!address || VirtualQuery(address, &info, sizeof(info)) != sizeof(info)) {
        return false;
    }
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        return false;
    }
    const DWORD executable = info.Protect & 0xffu;
    return executable == PAGE_EXECUTE ||
           executable == PAGE_EXECUTE_READ ||
           executable == PAGE_EXECUTE_READWRITE ||
           executable == PAGE_EXECUTE_WRITECOPY;
}

void show_secure_join_warning() {
    if (InterlockedCompareExchange(&g_warning_visible, 1, 0) != 0) return;

    MessageBoxW(
        nullptr,
        L"Memory Guard blocked a VAC-secured game-server connection.\n\n"
        L"No secure-session authentication ticket was created.",
        L"Memory Guard - Secure server blocked",
        MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    InterlockedExchange(&g_warning_visible, 0);
}

int __fastcall guarded_initiate_game_connection(
    void* self,
    void* /*fastcall_edx*/,
    void* auth_blob,
    int auth_blob_capacity,
    std::uint64_t game_server_steam_id,
    std::uint32_t server_ip,
    std::uint16_t server_port,
    bool secure) {
    if (secure) {
        logger::log(
            "VAC JOIN GUARD: blocked secure InitiateGameConnection "
            "steam_id=%llu ip=0x%08lX port=%u",
            static_cast<unsigned long long>(game_server_steam_id),
            static_cast<unsigned long>(server_ip),
            static_cast<unsigned>(server_port));
        logger::flush_now();
        show_secure_join_warning();
        SetLastError(ERROR_ACCESS_DISABLED_BY_POLICY);
        return 0;
    }

    InitiateGameConnectionFn original = g_original_initiate;
    if (!original) {
        logger::log(
            "VAC JOIN GUARD: original InitiateGameConnection unavailable; blocking connection");
        SetLastError(ERROR_PROC_NOT_FOUND);
        return 0;
    }

    logger::log(
        "VAC JOIN GUARD: allowing insecure InitiateGameConnection "
        "steam_id=%llu ip=0x%08lX port=%u",
        static_cast<unsigned long long>(game_server_steam_id),
        static_cast<unsigned long>(server_ip),
        static_cast<unsigned>(server_port));
    return original(self,
                    auth_blob,
                    auth_blob_capacity,
                    game_server_steam_id,
                    server_ip,
                    server_port,
                    secure);
}

bool replace_vtable_slot(
    void** slot,
    void* replacement,
    void** original,
    const char* name) {
    if (!slot || !replacement || !original) return false;

    void* target = *slot;
    if (!is_executable_address(target)) {
        logger::log(
            "VAC JOIN GUARD install failed: %s target is not executable target=%p slot=%p",
            name, target, slot);
        return false;
    }

    DWORD old_protect = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old_protect)) {
        logger::log(
            "VAC JOIN GUARD install failed: VirtualProtect %s slot=%p error=%lu",
            name, slot, GetLastError());
        return false;
    }

    // Publish the original before the replacement can be called on another
    // thread.  Only this interface's dispatch slot is changed.  In
    // particular, do not inline-patch the implementation address: Proton can
    // expose shared/generated Steam client entry points whose code is also
    // used by unrelated friend and persona operations.
    *original = target;
    MemoryBarrier();
    void* observed = InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(slot), replacement, target);

    DWORD ignored = 0;
    const BOOL protection_restored =
        VirtualProtect(slot, sizeof(*slot), old_protect, &ignored);

    if (observed != target) {
        logger::log(
            "VAC JOIN GUARD install failed: %s slot changed concurrently "
            "slot=%p expected=%p observed=%p",
            name, slot, target, observed);
        *original = nullptr;
        return false;
    }

    if (!protection_restored) {
        logger::log(
            "VAC JOIN GUARD warning: could not restore %s vtable protection "
            "slot=%p error=%lu",
            name, slot, GetLastError());
    }
    return true;
}

}  // namespace

bool install() {
    if (InterlockedCompareExchange(&g_installed, 0, 0) != 0) {
        return true;
    }

    HMODULE steam_api = GetModuleHandleW(L"steam_api.dll");
    if (!steam_api) {
        logger::log("VAC JOIN GUARD install failed: steam_api.dll is not loaded");
        return false;
    }

    auto steam_user = reinterpret_cast<SteamUserAccessorFn>(
        GetProcAddress(steam_api, "SteamUser"));
    if (!steam_user) {
        logger::log(
            "VAC JOIN GUARD install failed: SteamUser export unavailable");
        return false;
    }

    void* user = nullptr;
    constexpr DWORD RETRY_INTERVAL_MS = 100;
    constexpr DWORD RETRY_TIMEOUT_MS = 15000;
    for (DWORD elapsed = 0; elapsed < RETRY_TIMEOUT_MS; elapsed += RETRY_INTERVAL_MS) {
        user = steam_user();
        if (user) break;
        Sleep(RETRY_INTERVAL_MS);
    }
    if (!user) {
        logger::log(
            "VAC JOIN GUARD install failed: SteamUser unavailable after %lu ms",
            RETRY_TIMEOUT_MS);
        return false;
    }

    auto** user_vtable = *reinterpret_cast<void***>(user);
    if (!user_vtable) {
        logger::log("VAC JOIN GUARD install failed: SteamUser vtable is null");
        return false;
    }

    void** initiate_slot = &user_vtable[3];
    void* initiate_target = *initiate_slot;
    if (!is_executable_address(initiate_target)) {
        logger::log(
            "VAC JOIN GUARD install failed: InitiateGameConnection target not executable %p",
            initiate_target);
        return false;
    }

    if (!replace_vtable_slot(
            initiate_slot,
            reinterpret_cast<void*>(&guarded_initiate_game_connection),
            reinterpret_cast<void**>(&g_original_initiate),
            "InitiateGameConnection")) {
        return false;
    }

    g_initiate_target = initiate_target;
    g_initiate_slot = initiate_slot;
    InterlockedExchange(&g_installed, 1);
    logger::log(
        "VAC JOIN GUARD installed without SteamMatchmaking hooks: "
        "InitiateGameConnection slot=%p target=%p original=%p",
        g_initiate_slot,
        g_initiate_target,
        reinterpret_cast<void*>(g_original_initiate));
    return true;
}

}  // namespace vac_join_guard
