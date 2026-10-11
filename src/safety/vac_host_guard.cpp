#include "safety/vac_host_guard.h"

#include "shared/logging/logger.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace vac_host_guard {
namespace {

// This copy of steam_api.dll is from the 2015 Steamworks ABI.  Disassembly of
// its SteamGameServer_Init export confirms six cdecl arguments.
using SteamGameServerInitFn = bool (__cdecl *)(
    std::uint32_t ip,
    std::uint16_t steam_port,
    std::uint16_t game_port,
    std::uint16_t query_port,
    int server_mode,
    const char* version_string);

constexpr int SERVER_MODE_NO_AUTHENTICATION = 1;
constexpr int SERVER_MODE_AUTHENTICATION = 2;

SteamGameServerInitFn g_original_init = nullptr;
volatile LONG g_warning_visible = 0;
volatile LONG g_installed = 0;

bool is_allowed_insecure_mode(int mode) {
    return mode == SERVER_MODE_NO_AUTHENTICATION ||
           mode == SERVER_MODE_AUTHENTICATION;
}

bool __cdecl guarded_steam_game_server_init(
    std::uint32_t ip,
    std::uint16_t steam_port,
    std::uint16_t game_port,
    std::uint16_t query_port,
    int server_mode,
    const char* version_string) {
    if (!is_allowed_insecure_mode(server_mode)) {
        logger::log(
            "VAC HOST GUARD: blocked SteamGameServer_Init mode=%d "
            "(ports steam=%u game=%u query=%u)",
            server_mode,
            static_cast<unsigned>(steam_port),
            static_cast<unsigned>(game_port),
            static_cast<unsigned>(query_port));
        logger::flush_now();

        if (InterlockedCompareExchange(&g_warning_visible, 1, 0) == 0) {
            MessageBoxW(
                nullptr,
                L"Memory Guard blocked creation of a VAC-secured host.\n\n"
                L"Return to Create Session, uncheck 'VAC Secured', and try "
                L"again. No secure server was initialized.",
                L"Memory Guard - Secure host blocked",
                MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
            InterlockedExchange(&g_warning_visible, 0);
        }
        SetLastError(ERROR_ACCESS_DISABLED_BY_POLICY);
        return false;
    }

    SteamGameServerInitFn original = g_original_init;
    if (!original) {
        logger::log("VAC HOST GUARD: original SteamGameServer_Init is unavailable; blocking host");
        SetLastError(ERROR_PROC_NOT_FOUND);
        return false;
    }

    logger::log("VAC HOST GUARD: allowing insecure SteamGameServer_Init mode=%d", server_mode);
    return original(ip, steam_port, game_port, query_port, server_mode, version_string);
}

bool rva_is_mapped(DWORD rva, DWORD bytes, DWORD image_size) {
    return rva != 0 && rva < image_size && bytes <= image_size - rva;
}

}  // namespace

bool install() {
    if (InterlockedCompareExchange(&g_installed, 0, 0) != 0) {
        return true;
    }

    HMODULE game_module = GetModuleHandleW(L"mowas_2.exe");
    if (!game_module) {
        logger::log("VAC HOST GUARD install failed: mowas_2.exe module not found");
        return false;
    }

    auto* base = reinterpret_cast<unsigned char*>(game_module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        logger::log("VAC HOST GUARD install failed: invalid DOS header");
        return false;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        logger::log("VAC HOST GUARD install failed: invalid PE32 header");
        return false;
    }

    const DWORD image_size = nt->OptionalHeader.SizeOfImage;
    const IMAGE_DATA_DIRECTORY& imports_dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!rva_is_mapped(imports_dir.VirtualAddress,
                       sizeof(IMAGE_IMPORT_DESCRIPTOR), image_size)) {
        logger::log("VAC HOST GUARD install failed: import directory is unavailable");
        return false;
    }

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + imports_dir.VirtualAddress);
    const DWORD descriptor_limit =
        imports_dir.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);

    for (DWORD descriptor_index = 0;
         descriptor_index < descriptor_limit && descriptor->Name != 0;
         ++descriptor_index, ++descriptor) {
        if (!rva_is_mapped(descriptor->Name, 1, image_size)) {
            continue;
        }
        const char* dll_name = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(dll_name, "steam_api.dll") != 0) {
            continue;
        }

        const DWORD names_rva = descriptor->OriginalFirstThunk;
        const DWORD iat_rva = descriptor->FirstThunk;
        if (!rva_is_mapped(names_rva, sizeof(IMAGE_THUNK_DATA32), image_size) ||
            !rva_is_mapped(iat_rva, sizeof(IMAGE_THUNK_DATA32), image_size)) {
            logger::log("VAC HOST GUARD install failed: invalid Steam import thunks");
            return false;
        }

        auto* name_thunk = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + names_rva);
        auto* iat_thunk = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + iat_rva);
        const DWORD max_thunks = (image_size - names_rva) / sizeof(IMAGE_THUNK_DATA32);

        for (DWORD thunk_index = 0;
             thunk_index < max_thunks && name_thunk->u1.AddressOfData != 0;
             ++thunk_index, ++name_thunk, ++iat_thunk) {
            if (IMAGE_SNAP_BY_ORDINAL32(name_thunk->u1.Ordinal)) {
                continue;
            }
            const DWORD import_rva = name_thunk->u1.AddressOfData;
            if (!rva_is_mapped(import_rva, sizeof(IMAGE_IMPORT_BY_NAME), image_size)) {
                continue;
            }
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + import_rva);
            if (std::strcmp(reinterpret_cast<const char*>(import->Name),
                            "SteamGameServer_Init") != 0) {
                continue;
            }

            const DWORD original_address = iat_thunk->u1.Function;
            if (original_address == 0) {
                logger::log("VAC HOST GUARD install failed: SteamGameServer_Init resolved to null");
                return false;
            }

            DWORD old_protection = 0;
            if (!VirtualProtect(&iat_thunk->u1.Function,
                                sizeof(iat_thunk->u1.Function),
                                PAGE_READWRITE,
                                &old_protection)) {
                logger::log("VAC HOST GUARD install failed: VirtualProtect error=%lu",
                            GetLastError());
                return false;
            }

            g_original_init = reinterpret_cast<SteamGameServerInitFn>(
                static_cast<std::uintptr_t>(original_address));
            InterlockedExchange(
                reinterpret_cast<volatile LONG*>(&iat_thunk->u1.Function),
                static_cast<LONG>(reinterpret_cast<std::uintptr_t>(
                    &guarded_steam_game_server_init)));

            DWORD ignored = 0;
            if (!VirtualProtect(&iat_thunk->u1.Function,
                                sizeof(iat_thunk->u1.Function),
                                old_protection,
                                &ignored)) {
                logger::log(
                    "VAC HOST GUARD install failed: could not restore IAT protection error=%lu",
                    GetLastError());
                return false;
            }
            FlushInstructionCache(GetCurrentProcess(),
                                  &iat_thunk->u1.Function,
                                  sizeof(iat_thunk->u1.Function));
            InterlockedExchange(&g_installed, 1);
            logger::log(
                "VAC HOST GUARD installed: SteamGameServer_Init IAT=%p original=%p guard=%p",
                &iat_thunk->u1.Function,
                reinterpret_cast<void*>(g_original_init),
                reinterpret_cast<void*>(&guarded_steam_game_server_init));
            return true;
        }

        logger::log("VAC HOST GUARD install failed: SteamGameServer_Init import not found");
        return false;
    }

    logger::log("VAC HOST GUARD install failed: steam_api.dll import descriptor not found");
    return false;
}

}  // namespace vac_host_guard
