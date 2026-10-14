#include "safety/vac_join_guard.h"

#include "shared/logging/logger.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace vac_join_guard {
namespace {

// The game uses SteamMatchmaking009 lobby metadata for its Internet browser.
// steam_api.dll's flat wrappers confirm JoinLobby at vtable slot 14 and
// GetLobbyData at slot 19.  The game publishes the security state under the
// literal lobby-data key "secured".
using SteamMatchmakingAccessorFn = void* (__cdecl *)();
using GetLobbyDataFn = const char* (__cdecl *)(
    void* matchmaking,
    std::uint64_t lobby_steam_id,
    const char* key);
using JoinLobbyFn = std::uint64_t (__thiscall *)(
    void* matchmaking,
    std::uint64_t lobby_steam_id);

// SteamUser018 provides a second, later boundary before the game obtains the
// authentication blob used by a secure game server.
using SteamUserAccessorFn = void* (__cdecl *)();
using InitiateGameConnectionFn = int (__thiscall *)(
    void* self,
    void* auth_blob,
    int auth_blob_capacity,
    std::uint64_t game_server_steam_id,
    std::uint32_t server_ip,
    std::uint16_t server_port,
    bool secure);

GetLobbyDataFn g_get_lobby_data = nullptr;
JoinLobbyFn g_original_join_lobby = nullptr;
InitiateGameConnectionFn g_original_initiate = nullptr;
void* g_join_lobby_target = nullptr;
void* g_initiate_target = nullptr;
void** g_join_lobby_slot = nullptr;
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

bool lobby_value_is_explicitly_insecure(const char* value) {
    if (!value || !value[0]) return false;
    return std::strcmp(value, "0") == 0 ||
           _stricmp(value, "false") == 0 ||
           _stricmp(value, "no") == 0;
}

void show_secure_join_warning(bool lobby_boundary) {
    if (InterlockedCompareExchange(&g_warning_visible, 1, 0) != 0) return;

    MessageBoxW(
        nullptr,
        lobby_boundary
            ? L"Memory Guard blocked this lobby before joining because it is "
              L"VAC-secured (or did not provide an explicit insecure marker).\n\n"
              L"Join a lobby without the VAC shield, or have the other player host with "
              L"'VAC Secured' unchecked."
            : L"Memory Guard blocked a VAC-secured game-server connection.\n\n"
              L"No secure-session authentication ticket was created.",
        L"Memory Guard - Secure server blocked",
        MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    InterlockedExchange(&g_warning_visible, 0);
}

std::uint64_t __fastcall guarded_join_lobby(
    void* matchmaking,
    void* /*fastcall_edx*/,
    std::uint64_t lobby_steam_id) {
    const char* secured = g_get_lobby_data
        ? g_get_lobby_data(matchmaking, lobby_steam_id, "secured")
        : nullptr;

    if (!lobby_value_is_explicitly_insecure(secured)) {
        logger::log(
            "VAC JOIN GUARD: blocked JoinLobby lobby_id=%llu secured='%s'",
            static_cast<unsigned long long>(lobby_steam_id),
            secured && secured[0] ? secured : "<missing>");
        logger::flush_now();
        show_secure_join_warning(true);
        SetLastError(ERROR_ACCESS_DISABLED_BY_POLICY);
        return 0;  // k_uAPICallInvalid
    }

    JoinLobbyFn original = g_original_join_lobby;
    if (!original) {
        logger::log("VAC JOIN GUARD: original JoinLobby unavailable; blocking lobby");
        SetLastError(ERROR_PROC_NOT_FOUND);
        return 0;
    }

    logger::log(
        "VAC JOIN GUARD: allowing explicitly insecure JoinLobby lobby_id=%llu secured='%s'",
        static_cast<unsigned long long>(lobby_steam_id), secured);
    return original(matchmaking, lobby_steam_id);
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
        show_secure_join_warning(false);
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

bool restore_vtable_slot(
    void** slot,
    void* replacement,
    void* original,
    const char* name) {
    if (!slot || !replacement || !original) return false;

    DWORD old_protect = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old_protect)) {
        logger::log(
            "VAC JOIN GUARD rollback failed: VirtualProtect %s slot=%p error=%lu",
            name, slot, GetLastError());
        return false;
    }

    void* observed = InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(slot), original, replacement);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(*slot), old_protect, &ignored);

    if (observed != replacement) {
        logger::log(
            "VAC JOIN GUARD rollback failed: %s slot changed concurrently "
            "slot=%p expected=%p observed=%p",
            name, slot, replacement, observed);
        return false;
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

    auto steam_matchmaking = reinterpret_cast<SteamMatchmakingAccessorFn>(
        GetProcAddress(steam_api, "SteamMatchmaking"));
    auto steam_user = reinterpret_cast<SteamUserAccessorFn>(
        GetProcAddress(steam_api, "SteamUser"));
    g_get_lobby_data = reinterpret_cast<GetLobbyDataFn>(
        GetProcAddress(steam_api, "SteamAPI_ISteamMatchmaking_GetLobbyData"));
    if (!steam_matchmaking || !steam_user || !g_get_lobby_data) {
        logger::log(
            "VAC JOIN GUARD install failed: required Steam exports unavailable "
            "matchmaking=%p user=%p get_lobby_data=%p",
            reinterpret_cast<void*>(steam_matchmaking),
            reinterpret_cast<void*>(steam_user),
            reinterpret_cast<void*>(g_get_lobby_data));
        return false;
    }

    void* matchmaking = nullptr;
    void* user = nullptr;
    constexpr DWORD RETRY_INTERVAL_MS = 100;
    constexpr DWORD RETRY_TIMEOUT_MS = 15000;
    for (DWORD elapsed = 0; elapsed < RETRY_TIMEOUT_MS; elapsed += RETRY_INTERVAL_MS) {
        matchmaking = steam_matchmaking();
        user = steam_user();
        if (matchmaking && user) break;
        Sleep(RETRY_INTERVAL_MS);
    }
    if (!matchmaking || !user) {
        logger::log(
            "VAC JOIN GUARD install failed: Steam interfaces unavailable after %lu ms "
            "matchmaking=%p user=%p",
            RETRY_TIMEOUT_MS, matchmaking, user);
        return false;
    }

    auto** matchmaking_vtable = *reinterpret_cast<void***>(matchmaking);
    auto** user_vtable = *reinterpret_cast<void***>(user);
    if (!matchmaking_vtable || !user_vtable) {
        logger::log("VAC JOIN GUARD install failed: Steam interface vtable is null");
        return false;
    }

    void** join_lobby_slot = &matchmaking_vtable[14];
    void** initiate_slot = &user_vtable[3];
    void* join_lobby_target = *join_lobby_slot;
    void* initiate_target = *initiate_slot;
    if (!is_executable_address(join_lobby_target) ||
        !is_executable_address(initiate_target)) {
        logger::log(
            "VAC JOIN GUARD install failed: hook target not executable "
            "JoinLobby=%p InitiateGameConnection=%p",
            join_lobby_target, initiate_target);
        return false;
    }

    if (!replace_vtable_slot(
            join_lobby_slot,
            reinterpret_cast<void*>(&guarded_join_lobby),
            reinterpret_cast<void**>(&g_original_join_lobby),
            "JoinLobby")) {
        return false;
    }

    if (!replace_vtable_slot(
            initiate_slot,
            reinterpret_cast<void*>(&guarded_initiate_game_connection),
            reinterpret_cast<void**>(&g_original_initiate),
            "InitiateGameConnection")) {
        restore_vtable_slot(
            join_lobby_slot,
            reinterpret_cast<void*>(&guarded_join_lobby),
            reinterpret_cast<void*>(g_original_join_lobby),
            "JoinLobby");
        g_original_join_lobby = nullptr;
        return false;
    }

    g_join_lobby_target = join_lobby_target;
    g_initiate_target = initiate_target;
    g_join_lobby_slot = join_lobby_slot;
    g_initiate_slot = initiate_slot;
    InterlockedExchange(&g_installed, 1);
    logger::log(
        "VAC JOIN GUARD installed with interface-local vtable hooks: "
        "JoinLobby slot=%p target=%p original=%p; "
        "InitiateGameConnection slot=%p target=%p original=%p",
        g_join_lobby_slot,
        g_join_lobby_target,
        reinterpret_cast<void*>(g_original_join_lobby),
        g_initiate_slot,
        g_initiate_target,
        reinterpret_cast<void*>(g_original_initiate));
    return true;
}

}  // namespace vac_join_guard
