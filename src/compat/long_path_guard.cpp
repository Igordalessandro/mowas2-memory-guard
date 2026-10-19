#include "app/config.h"
#include "shared/core/module.h"
#include "shared/logging/logger.h"

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace long_path_guard {
namespace {

using CreateFileAFn = HANDLE (WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                      DWORD, DWORD, HANDLE);
using CreateFileWFn = HANDLE (WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                      DWORD, DWORD, HANDLE);
using FindFirstFileWFn = HANDLE (WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
using FindFirstFileExAFn = HANDLE (WINAPI*)(LPCSTR, FINDEX_INFO_LEVELS, LPVOID,
                                           FINDEX_SEARCH_OPS, LPVOID, DWORD);
using FindFirstFileExWFn = HANDLE (WINAPI*)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID,
                                           FINDEX_SEARCH_OPS, LPVOID, DWORD);
using GetFileAttributesWFn = DWORD (WINAPI*)(LPCWSTR);
using GetFileAttributesExWFn = BOOL (WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);

constexpr std::size_t LONG_PATH_TRIGGER = 248;
constexpr std::size_t MAX_EXTENDED_PATH_CHARS = 32760;

static CreateFileAFn g_create_file_a = nullptr;
static CreateFileWFn g_create_file_w = nullptr;
static FindFirstFileWFn g_find_first_file_w = nullptr;
static FindFirstFileExAFn g_find_first_file_ex_a = nullptr;
static FindFirstFileExWFn g_find_first_file_ex_w = nullptr;
static GetFileAttributesWFn g_get_file_attributes_w = nullptr;
static GetFileAttributesExWFn g_get_file_attributes_ex_w = nullptr;

static volatile LONG g_rewrites = 0;
static volatile LONG g_failures = 0;
static volatile LONG g_reported = 0;

static void* allocate_bytes(std::size_t bytes) {
    if (bytes == 0 || bytes > static_cast<std::size_t>(MAXDWORD)) return nullptr;
    return HeapAlloc(GetProcessHeap(), 0, bytes);
}

static void release_bytes(void* value) {
    if (value) HeapFree(GetProcessHeap(), 0, value);
}

template <typename Char>
static std::size_t bounded_length(const Char* value) {
    if (!value) return 0;
    for (std::size_t i = 0; i <= MAX_EXTENDED_PATH_CHARS; ++i) {
        if (value[i] == 0) return i;
    }
    return MAX_EXTENDED_PATH_CHARS + 1;
}

static bool is_separator(wchar_t value) {
    return value == L'\\' || value == L'/';
}

static bool is_drive_absolute(const wchar_t* path) {
    return path && path[0] && path[1] == L':' && is_separator(path[2]);
}

static bool is_unc(const wchar_t* path) {
    return path && is_separator(path[0]) && is_separator(path[1]) &&
           path[2] != L'?' && path[2] != L'.';
}

static bool is_extended_or_device(const wchar_t* path) {
    return path && is_separator(path[0]) && is_separator(path[1]) &&
           (path[2] == L'?' || path[2] == L'.') && is_separator(path[3]);
}

struct WidePath {
    wchar_t* owned = nullptr;
    const wchar_t* value = nullptr;
    std::size_t original_length = 0;
    bool rewritten = false;

    WidePath() = default;
    WidePath(const WidePath&) = delete;
    WidePath& operator=(const WidePath&) = delete;
    WidePath(WidePath&& other) noexcept
        : owned(other.owned), value(other.value),
          original_length(other.original_length), rewritten(other.rewritten) {
        other.owned = nullptr;
    }
    ~WidePath() { release_bytes(owned); }
};

struct AnsiPath {
    char* owned = nullptr;
    const char* value = nullptr;
    std::size_t original_length = 0;
    bool rewritten = false;

    AnsiPath() = default;
    AnsiPath(const AnsiPath&) = delete;
    AnsiPath& operator=(const AnsiPath&) = delete;
    AnsiPath(AnsiPath&& other) noexcept
        : owned(other.owned), value(other.value),
          original_length(other.original_length), rewritten(other.rewritten) {
        other.owned = nullptr;
    }
    ~AnsiPath() { release_bytes(owned); }
};

static bool copy_normalized(const wchar_t* input, std::size_t length,
                            wchar_t** output) {
    auto* result = static_cast<wchar_t*>(
        allocate_bytes((length + 1) * sizeof(wchar_t)));
    if (!result) return false;
    for (std::size_t i = 0; i < length; ++i) {
        result[i] = input[i] == L'/' ? L'\\' : input[i];
    }
    result[length] = L'\0';
    *output = result;
    return true;
}

static bool make_absolute(wchar_t** path, std::size_t* length) {
    wchar_t* current = *path;
    if (is_drive_absolute(current) || is_unc(current) ||
        is_extended_or_device(current)) {
        return true;
    }

    const DWORD cwd_required = GetCurrentDirectoryW(0, nullptr);
    if (cwd_required == 0 || cwd_required > MAX_EXTENDED_PATH_CHARS) return false;
    auto* cwd = static_cast<wchar_t*>(
        allocate_bytes(static_cast<std::size_t>(cwd_required) * sizeof(wchar_t)));
    if (!cwd) return false;
    const DWORD cwd_length = GetCurrentDirectoryW(cwd_required, cwd);
    if (cwd_length == 0 || cwd_length >= cwd_required) {
        release_bytes(cwd);
        return false;
    }

    const bool root_relative = is_separator(current[0]) && !is_separator(current[1]);
    const bool drive_relative = current[0] && current[1] == L':' &&
                                !is_separator(current[2]);
    if (drive_relative || (root_relative && cwd_length < 2)) {
        release_bytes(cwd);
        return false;
    }

    const std::size_t prefix_length = root_relative ? 2u : cwd_length;
    const bool separator_needed = !root_relative &&
        prefix_length > 0 && !is_separator(cwd[prefix_length - 1]);
    const std::size_t combined_length = prefix_length +
        (separator_needed ? 1u : 0u) + *length;
    if (combined_length > MAX_EXTENDED_PATH_CHARS) {
        release_bytes(cwd);
        return false;
    }

    auto* combined = static_cast<wchar_t*>(
        allocate_bytes((combined_length + 1) * sizeof(wchar_t)));
    if (!combined) {
        release_bytes(cwd);
        return false;
    }
    std::memcpy(combined, cwd, prefix_length * sizeof(wchar_t));
    std::size_t out = prefix_length;
    if (separator_needed) combined[out++] = L'\\';
    std::memcpy(combined + out, current, (*length + 1) * sizeof(wchar_t));
    release_bytes(cwd);
    release_bytes(current);
    *path = combined;
    *length = combined_length;
    return true;
}

static bool canonicalize_absolute(wchar_t* path, std::size_t* length) {
    if (!path || !length) return false;

    std::size_t root_length = 0;
    std::size_t read = 0;
    if (is_drive_absolute(path)) {
        root_length = 2;  // Keep "C:"; component writing restores the slash.
        read = 2;
    } else if (is_unc(path)) {
        read = 2;
        while (read < *length && !is_separator(path[read])) ++read;
        if (read == 2 || read >= *length) return false;
        while (read < *length && is_separator(path[read])) ++read;
        const std::size_t share_start = read;
        while (read < *length && !is_separator(path[read])) ++read;
        if (read == share_start) return false;
        root_length = read;
    } else {
        return false;
    }

    std::size_t write = root_length;
    while (read < *length) {
        while (read < *length && is_separator(path[read])) ++read;
        const std::size_t component_start = read;
        while (read < *length && !is_separator(path[read])) ++read;
        const std::size_t component_length = read - component_start;
        if (component_length == 0) break;
        if (component_length == 1 && path[component_start] == L'.') continue;
        if (component_length == 2 && path[component_start] == L'.' &&
            path[component_start + 1] == L'.') {
            while (write > root_length && path[write - 1] != L'\\') --write;
            if (write > root_length && path[write - 1] == L'\\') --write;
            continue;
        }
        if (write == 0 || path[write - 1] != L'\\') path[write++] = L'\\';
        if (write != component_start) {
            std::memmove(path + write, path + component_start,
                         component_length * sizeof(wchar_t));
        }
        write += component_length;
    }

    if (write == root_length && is_drive_absolute(path)) path[write++] = L'\\';
    path[write] = L'\0';
    *length = write;
    return true;
}

static bool add_extended_prefix(wchar_t** path, std::size_t* length) {
    wchar_t* current = *path;
    if (is_extended_or_device(current)) return true;

    const bool unc = is_unc(current);
    if (!unc && !is_drive_absolute(current)) return false;
    const wchar_t* prefix = unc ? L"\\\\?\\UNC\\" : L"\\\\?\\";
    const std::size_t prefix_length = unc ? 8u : 4u;
    const std::size_t source_offset = unc ? 2u : 0u;
    const std::size_t new_length = prefix_length + *length - source_offset;
    if (new_length > MAX_EXTENDED_PATH_CHARS) return false;

    auto* extended = static_cast<wchar_t*>(
        allocate_bytes((new_length + 1) * sizeof(wchar_t)));
    if (!extended) return false;
    std::memcpy(extended, prefix, prefix_length * sizeof(wchar_t));
    std::memcpy(extended + prefix_length, current + source_offset,
                (*length - source_offset + 1) * sizeof(wchar_t));
    release_bytes(current);
    *path = extended;
    *length = new_length;
    return true;
}

static WidePath prepare_wide_path(const wchar_t* input, bool force = false) {
    WidePath result;
    result.value = input;
    result.original_length = bounded_length(input);
    if (!input || result.original_length == 0 ||
        result.original_length > MAX_EXTENDED_PATH_CHARS ||
        (!force && result.original_length < LONG_PATH_TRIGGER) ||
        is_extended_or_device(input)) {
        return result;
    }

    wchar_t* normalized = nullptr;
    if (!copy_normalized(input, result.original_length, &normalized)) return result;
    std::size_t normalized_length = result.original_length;
    if (!make_absolute(&normalized, &normalized_length) ||
        !canonicalize_absolute(normalized, &normalized_length) ||
        !add_extended_prefix(&normalized, &normalized_length)) {
        release_bytes(normalized);
        return result;
    }

    result.owned = normalized;
    result.value = normalized;
    result.rewritten = true;
    return result;
}

static AnsiPath prepare_ansi_path(const char* input) {
    AnsiPath result;
    result.value = input;
    result.original_length = bounded_length(input);
    if (!input || result.original_length == 0 ||
        result.original_length > MAX_EXTENDED_PATH_CHARS ||
        result.original_length < LONG_PATH_TRIGGER) {
        return result;
    }

    const int wide_required = MultiByteToWideChar(CP_ACP, 0, input, -1, nullptr, 0);
    if (wide_required <= 0 || wide_required > static_cast<int>(MAX_EXTENDED_PATH_CHARS)) {
        return result;
    }
    auto* wide = static_cast<wchar_t*>(
        allocate_bytes(static_cast<std::size_t>(wide_required) * sizeof(wchar_t)));
    if (!wide) return result;
    if (MultiByteToWideChar(CP_ACP, 0, input, -1, wide, wide_required) == 0) {
        release_bytes(wide);
        return result;
    }

    WidePath prepared = prepare_wide_path(wide, true);
    if (!prepared.rewritten) {
        release_bytes(wide);
        return result;
    }
    const int ansi_required = WideCharToMultiByte(
        CP_ACP, 0, prepared.value, -1, nullptr, 0, nullptr, nullptr);
    if (ansi_required <= 0 || ansi_required > static_cast<int>(MAX_EXTENDED_PATH_CHARS)) {
        release_bytes(wide);
        return result;
    }
    auto* ansi = static_cast<char*>(allocate_bytes(static_cast<std::size_t>(ansi_required)));
    if (!ansi || WideCharToMultiByte(CP_ACP, 0, prepared.value, -1, ansi,
                                     ansi_required, nullptr, nullptr) == 0) {
        release_bytes(ansi);
        release_bytes(wide);
        return result;
    }
    release_bytes(wide);
    result.owned = ansi;
    result.value = ansi;
    result.rewritten = true;
    return result;
}

static void record_result(const char* api, std::size_t length, bool rewritten,
                          bool succeeded, DWORD error) {
    if (length < LONG_PATH_TRIGGER) return;
    if (rewritten) InterlockedIncrement(&g_rewrites);
    if (!succeeded) InterlockedIncrement(&g_failures);

    const LONG event_number = InterlockedIncrement(&g_reported);
    if (event_number <= 32 || !succeeded) {
        logger::log(
            "LONG PATH : api=%s chars=%zu rewritten=%s result=%s error=%lu total_rewrites=%ld failures=%ld",
            api, length, rewritten ? "yes" : "no", succeeded ? "ok" : "failed",
            static_cast<unsigned long>(error), static_cast<long>(g_rewrites),
            static_cast<long>(g_failures));
    }
    if (rewritten && event_number == 1) {
        logger::report_event("event=long_path api=%s chars=%zu action=extended_prefix", api,
                             length);
    }
}

static HANDLE WINAPI guarded_create_file_a(
    LPCSTR path, DWORD access, DWORD sharing, LPSECURITY_ATTRIBUTES security,
    DWORD creation, DWORD flags, HANDLE template_file) {
    AnsiPath prepared = prepare_ansi_path(path);
    HANDLE result = g_create_file_a(prepared.value, access, sharing, security,
                                    creation, flags, template_file);
    const DWORD error = result == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    record_result("CreateFileA", prepared.original_length, prepared.rewritten,
                  result != INVALID_HANDLE_VALUE, error);
    if (result == INVALID_HANDLE_VALUE) SetLastError(error);
    return result;
}

static HANDLE WINAPI guarded_create_file_w(
    LPCWSTR path, DWORD access, DWORD sharing, LPSECURITY_ATTRIBUTES security,
    DWORD creation, DWORD flags, HANDLE template_file) {
    WidePath prepared = prepare_wide_path(path);
    HANDLE result = g_create_file_w(prepared.value, access, sharing, security,
                                    creation, flags, template_file);
    const DWORD error = result == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    record_result("CreateFileW", prepared.original_length, prepared.rewritten,
                  result != INVALID_HANDLE_VALUE, error);
    if (result == INVALID_HANDLE_VALUE) SetLastError(error);
    return result;
}

static HANDLE WINAPI guarded_find_first_file_w(LPCWSTR path, LPWIN32_FIND_DATAW data) {
    WidePath prepared = prepare_wide_path(path);
    HANDLE result = g_find_first_file_w(prepared.value, data);
    const DWORD error = result == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    record_result("FindFirstFileW", prepared.original_length, prepared.rewritten,
                  result != INVALID_HANDLE_VALUE, error);
    if (result == INVALID_HANDLE_VALUE) SetLastError(error);
    return result;
}

static HANDLE WINAPI guarded_find_first_file_ex_a(
    LPCSTR path, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS search,
    LPVOID filter, DWORD flags) {
    AnsiPath prepared = prepare_ansi_path(path);
    HANDLE result = g_find_first_file_ex_a(prepared.value, level, data, search, filter, flags);
    const DWORD error = result == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    record_result("FindFirstFileExA", prepared.original_length, prepared.rewritten,
                  result != INVALID_HANDLE_VALUE, error);
    if (result == INVALID_HANDLE_VALUE) SetLastError(error);
    return result;
}

static HANDLE WINAPI guarded_find_first_file_ex_w(
    LPCWSTR path, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS search,
    LPVOID filter, DWORD flags) {
    WidePath prepared = prepare_wide_path(path);
    HANDLE result = g_find_first_file_ex_w(prepared.value, level, data, search, filter, flags);
    const DWORD error = result == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    record_result("FindFirstFileExW", prepared.original_length, prepared.rewritten,
                  result != INVALID_HANDLE_VALUE, error);
    if (result == INVALID_HANDLE_VALUE) SetLastError(error);
    return result;
}

static DWORD WINAPI guarded_get_file_attributes_w(LPCWSTR path) {
    WidePath prepared = prepare_wide_path(path);
    const DWORD result = g_get_file_attributes_w(prepared.value);
    const DWORD error = result == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    record_result("GetFileAttributesW", prepared.original_length, prepared.rewritten,
                  result != INVALID_FILE_ATTRIBUTES, error);
    if (result == INVALID_FILE_ATTRIBUTES) SetLastError(error);
    return result;
}

static BOOL WINAPI guarded_get_file_attributes_ex_w(
    LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    WidePath prepared = prepare_wide_path(path);
    const BOOL result = g_get_file_attributes_ex_w(prepared.value, level, info);
    const DWORD error = result ? ERROR_SUCCESS : GetLastError();
    record_result("GetFileAttributesExW", prepared.original_length, prepared.rewritten,
                  result != FALSE, error);
    if (!result) SetLastError(error);
    return result;
}

struct IatPatch {
    const char* name;
    void* replacement;
    void** original;
    IMAGE_THUNK_DATA32* slot;
    DWORD old_value;
};

static bool find_iat_slots(std::uintptr_t base, IatPatch* patches, unsigned count) {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(
        base + static_cast<std::uintptr_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return false;
    const DWORD image_size = nt->OptionalHeader.SizeOfImage;
    const auto& import_dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!import_dir.VirtualAddress || import_dir.VirtualAddress >= image_size) return false;

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + import_dir.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        if (descriptor->Name >= image_size) return false;
        const char* dll_name = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(dll_name, "KERNEL32.dll") != 0) continue;
        if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) return false;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(
            base + descriptor->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) continue;
            if (names->u1.AddressOfData >= image_size) return false;
            auto* imported = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + names->u1.AddressOfData);
            const char* name = reinterpret_cast<const char*>(imported->Name);
            for (unsigned i = 0; i < count; ++i) {
                if (!patches[i].slot && std::strcmp(name, patches[i].name) == 0) {
                    patches[i].slot = iat;
                }
            }
        }
        return true;
    }
    return false;
}

static bool patch_slot(IatPatch& patch) {
    DWORD old_protection = 0;
    if (!VirtualProtect(&patch.slot->u1.Function, sizeof(DWORD), PAGE_READWRITE,
                        &old_protection)) return false;
    patch.old_value = patch.slot->u1.Function;
    *patch.original = reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(patch.old_value));
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&patch.slot->u1.Function),
                        static_cast<LONG>(reinterpret_cast<std::uintptr_t>(patch.replacement)));
    DWORD ignored = 0;
    const bool restored = VirtualProtect(&patch.slot->u1.Function, sizeof(DWORD),
                                         old_protection, &ignored) != FALSE;
    FlushInstructionCache(GetCurrentProcess(), &patch.slot->u1.Function, sizeof(DWORD));
    return restored;
}

static void restore_slot(IatPatch& patch) {
    if (!patch.slot || !patch.old_value) return;
    DWORD old_protection = 0;
    if (!VirtualProtect(&patch.slot->u1.Function, sizeof(DWORD), PAGE_READWRITE,
                        &old_protection)) return;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&patch.slot->u1.Function),
                        static_cast<LONG>(patch.old_value));
    DWORD ignored = 0;
    VirtualProtect(&patch.slot->u1.Function, sizeof(DWORD), old_protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), &patch.slot->u1.Function, sizeof(DWORD));
    *patch.original = nullptr;
}

static bool install(std::uintptr_t game_base) {
    IatPatch patches[] = {
        {"CreateFileA", reinterpret_cast<void*>(&guarded_create_file_a),
         reinterpret_cast<void**>(&g_create_file_a), nullptr, 0},
        {"CreateFileW", reinterpret_cast<void*>(&guarded_create_file_w),
         reinterpret_cast<void**>(&g_create_file_w), nullptr, 0},
        {"FindFirstFileW", reinterpret_cast<void*>(&guarded_find_first_file_w),
         reinterpret_cast<void**>(&g_find_first_file_w), nullptr, 0},
        {"FindFirstFileExA", reinterpret_cast<void*>(&guarded_find_first_file_ex_a),
         reinterpret_cast<void**>(&g_find_first_file_ex_a), nullptr, 0},
        {"FindFirstFileExW", reinterpret_cast<void*>(&guarded_find_first_file_ex_w),
         reinterpret_cast<void**>(&g_find_first_file_ex_w), nullptr, 0},
        {"GetFileAttributesW", reinterpret_cast<void*>(&guarded_get_file_attributes_w),
         reinterpret_cast<void**>(&g_get_file_attributes_w), nullptr, 0},
        {"GetFileAttributesExW", reinterpret_cast<void*>(&guarded_get_file_attributes_ex_w),
         reinterpret_cast<void**>(&g_get_file_attributes_ex_w), nullptr, 0},
    };
    constexpr unsigned patch_count = sizeof(patches) / sizeof(patches[0]);
    if (!find_iat_slots(game_base, patches, patch_count)) {
        logger::log("LONG PATH install failed: KERNEL32 import table unavailable");
        return false;
    }
    for (unsigned i = 0; i < patch_count; ++i) {
        if (!patches[i].slot) {
            logger::log("LONG PATH install failed: %s import not found", patches[i].name);
            return false;
        }
    }

    unsigned installed = 0;
    for (; installed < patch_count; ++installed) {
        if (!patch_slot(patches[installed])) {
            logger::log("LONG PATH install failed: could not patch %s", patches[installed].name);
            restore_slot(patches[installed]);
            while (installed > 0) restore_slot(patches[--installed]);
            return false;
        }
    }
    logger::log(
        "LONG PATH installed: game-only IAT compatibility for %u file APIs; trigger=%zu chars",
        patch_count, LONG_PATH_TRIGGER);
    return true;
}

}  // namespace

namespace {
bool install_module(std::uintptr_t game_base) {
    return long_path_guard::install(game_base);
}
}

MOWAS_REGISTER_MODULE(
    long_path_guard_module,
    (::core::ModuleSpec{"long_path_guard", 90, &install_module}));

}  // namespace long_path_guard
