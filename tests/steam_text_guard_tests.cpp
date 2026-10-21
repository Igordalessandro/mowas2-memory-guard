// Production wrapper and ABI recognizer; fake Steam/reader, no live hooks.
#include "../src/compat/steam_text_guard.cpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

namespace core {
void register_module(const ModuleSpec &) {}
} // namespace core
namespace logger {
void log(const char *, ...) { SetLastError(0xBAD); }
void report_event(const char *, ...) { SetLastError(0xBAD); }
} // namespace logger
#ifndef MOWAS2_REAL_HOOK_SMOKE
extern "C" {
MH_STATUS WINAPI MH_CreateHook(LPVOID, LPVOID, LPVOID *) {
    assert(false);
    return MH_UNKNOWN;
}
MH_STATUS WINAPI MH_EnableHook(LPVOID) {
    assert(false);
    return MH_UNKNOWN;
}
MH_STATUS WINAPI MH_RemoveHook(LPVOID) {
    assert(false);
    return MH_UNKNOWN;
}
}
#endif
namespace {
constexpr std::uint64_t source_address = 0x555500000000ull;
const char *source = "first player";
const char stale[] = "wrong player";
bool read_fails = false;
unsigned original_calls = 0;
LONG WINAPI reader(HANDLE, std::uint64_t address, void *out, std::uint64_t size,
                   std::uint64_t *copied) {
    assert(address == source_address && size == std::strlen(source) + 1);
    SetLastError(0xBAD);
    if (read_fails) {
        *copied = 0;
        return -1;
    }
    std::memcpy(out, source, static_cast<std::size_t>(size));
    *copied = size;
    return 0;
}
void *__cdecl original(steam_text_guard::NativeBuffer) {
    ++original_calls;
    assert(GetLastError() == 0xBEEF);
    SetLastError(0xCAFE);
    return const_cast<char *>(stale);
}
void test_wrapper() {
    using namespace steam_text_guard;
    static TextStore test_store;
    store = &test_store;
    read64 = &reader;
    original_buffer = &original;
    SetLastError(0xBEEF);
    const auto buffer = NativeBuffer{source_address, std::strlen(source) + 1};
    const char *first = static_cast<char *>(guarded_buffer(buffer));
    assert(!std::strcmp(first, "first player") && GetLastError() == 0xBEEF);
    assert(mismatch_reported == 1 && original_calls == 1);
    source = "other player"; // same source pointer/length, different content
    SetLastError(0xBEEF);
    const char *second = static_cast<char *>(guarded_buffer(buffer));
    assert(!std::strcmp(second, source) && !std::strcmp(first, "first player"));
    assert(GetLastError() == 0xBEEF && original_calls == 1);
    read_fails = true;
    SetLastError(0xBEEF);
    assert(guarded_buffer(buffer) == stale && GetLastError() == 0xCAFE);
    assert(original_calls == 2 && failure_reported == 1);
    SetLastError(0xBEEF);
    assert(guarded_buffer(NativeBuffer{0x1234, 4}) == stale && GetLastError() == 0xCAFE);
    assert(original_calls == 3);
}
void test_abi() {
    using namespace steam_text_guard;
    unsigned char image[1024] = {};
    const unsigned char call[] = {0x8b, 0x45, 0xf0, 0x89, 0x04, 0x24, 0x8b, 0x45, 0xf4,
                                  0x89, 0x44, 0x24, 0x04, 0x8b, 0x45, 0xf8, 0x89, 0x44,
                                  0x24, 0x08, 0x8b, 0x45, 0xfc, 0x89, 0x44, 0x24, 0x0c,
                                  0xe8, 0,    0,    0,    0,    0xc9, 0xc3};
    const unsigned char prologue[] = {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x51, 0x8d, 0x4d,
                                      0x08, 0x83, 0xec, 0x28, 0x8b, 0x01, 0x8b, 0x51, 0x04};
    std::memcpy(image + 20, call, sizeof(call));
    std::int32_t displacement = 512 - 20 - 32;
    std::memcpy(image + 20 + 28, &displacement, 4);
    std::memcpy(image + 512, prologue, sizeof(prologue));
    MODULEINFO info = {image, sizeof(image), nullptr};
    assert(find_buffer_helper(image, info, 0) == image + 512);
    assert(!find_buffer_helper(image, info, 8));
    image[20 + 33] = 0xc2;
    image[20 + 34] = 8;
    assert(find_buffer_helper(image, info, 8) == image + 512);
    image[20 + 34] = 4;
    assert(!find_buffer_helper(image, info, 8));
    image[20 + 34] = 8;
    image[512] = 0xe9;
    assert(!find_buffer_helper(image, info, 8));
    image[512] = 0x55;
    image[20 + 15] = 0xf7;
    assert(!find_buffer_helper(image, info, 8));
    assert(!find_buffer_helper(image + 1000, info, 0));
    // Negative relative displacement and relocated image bases.
    image[20 + 15] = 0xf8;
    std::memcpy(image + 700, image + 20, 36);
    displacement = 512 - 700 - 32;
    std::memcpy(image + 700 + 28, &displacement, 4);
    assert(find_buffer_helper(image + 700, info, 8) == image + 512);
}
} // namespace
int main(int argc, char **argv) {
    test_wrapper();
    test_abi();
    if (argc == 6 && !std::strcmp(argv[1], "--probe-proton")) {
        // Read-only image mapping: no DllMain, Steam initialization or hooks.
        HMODULE bridge = LoadLibraryExA(argv[2], nullptr, DONT_RESOLVE_DLL_REFERENCES);
        assert(bridge);
        MODULEINFO image = {};
        assert(GetModuleInformation(GetCurrentProcess(), bridge, &image, sizeof(image)));
        const auto base = reinterpret_cast<std::uintptr_t>(bridge);
        const auto own = static_cast<std::uintptr_t>(std::strtoul(argv[3], nullptr, 0));
        const auto friends = static_cast<std::uintptr_t>(std::strtoul(argv[4], nullptr, 0));
        const auto helper = static_cast<std::uintptr_t>(std::strtoul(argv[5], nullptr, 0));
        assert(own < image.SizeOfImage && friends < image.SizeOfImage &&
               helper < image.SizeOfImage);
        using namespace steam_text_guard;
        assert(find_buffer_helper(reinterpret_cast<void *>(base + own), image, 0) ==
               reinterpret_cast<void *>(base + helper));
        assert(find_buffer_helper(reinterpret_cast<void *>(base + friends), image, 8) ==
               reinterpret_cast<void *>(base + helper));
#ifdef MOWAS2_REAL_HOOK_SMOKE
        // Patch only this isolated test process's private image mapping. Do
        // not execute the real helper/imports or initialize Steam. The fake
        // high-address reader lets us check the real detour + struct ABI.
        assert(MH_Initialize() == MH_OK);
        void *target = reinterpret_cast<void *>(base + helper);
        assert(MH_CreateHook(target, reinterpret_cast<void *>(&guarded_buffer),
                             reinterpret_cast<void **>(&original_buffer)) == MH_OK);
        comparisons = 64; // original helper's imports deliberately unresolved
        read_fails = false;
        source = "third player";
        assert(MH_EnableHook(target) == MH_OK);
        SetLastError(0xBEEF);
        const char *result = static_cast<char *>(reinterpret_cast<BufferFn>(target)(
            NativeBuffer{source_address, std::strlen(source) + 1}));
        assert(result && !std::strcmp(result, source) && GetLastError() == 0xBEEF);
        assert(MH_DisableHook(target) == MH_OK);
        assert(MH_RemoveHook(target) == MH_OK);
        original_buffer = &original;
        assert(MH_Uninitialize() == MH_OK);
        std::puts("Real MinHook detour on isolated Proton helper: x86 struct ABI passed.");
#endif
        FreeLibrary(bridge);
        std::puts("Installed Proton bridge image: both wrapper ABIs and helper matched.");
    }
}
