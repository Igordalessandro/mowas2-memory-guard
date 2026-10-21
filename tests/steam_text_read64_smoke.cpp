// Isolated x86 Wine smoke test: no Steam, game, hooks, dialogs or shared prefix.
// Allocate one page above 4 GiB, read it through the actual Wine syscall, and
// change its contents at the same address. Process exit releases the page.
#include "compat/stable_steam_text.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <windows.h>

int main() {
    using namespace steam_text_guard;
    using ReadFn = LONG(WINAPI *)(HANDLE, std::uint64_t, void *, std::uint64_t, std::uint64_t *);
    using WriteFn =
        LONG(WINAPI *)(HANDLE, std::uint64_t, const void *, std::uint64_t, std::uint64_t *);
    using AllocateFn =
        LONG(WINAPI *)(HANDLE, std::uint64_t *, std::uint64_t, std::uint64_t *, ULONG, ULONG);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    assert(GetProcAddress(ntdll, "wine_get_version"));
    const auto read = reinterpret_cast<ReadFn>(GetProcAddress(ntdll, "NtWow64ReadVirtualMemory64"));
    const auto write =
        reinterpret_cast<WriteFn>(GetProcAddress(ntdll, "NtWow64WriteVirtualMemory64"));
    const auto allocate =
        reinterpret_cast<AllocateFn>(GetProcAddress(ntdll, "NtWow64AllocateVirtualMemory64"));
    assert(read && write && allocate);
    HANDLE process = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE,
                                 GetCurrentProcessId());
    assert(process);
    char unix_pid[32] = {};
    if (GetEnvironmentVariableA("MOWAS2_SMOKE_UNIX_PID", unix_pid, sizeof(unix_pid))) {
        // Some Wine builds reserve the requested >4-GiB Windows VA. Exercise
        // their reader against existing native memory instead: only this
        // test process's ELF image, never the game/Steam or another process.
        std::ifstream maps(std::string("Z:\\proc\\") + unix_pid + "\\maps");
        assert(maps);
        std::string line;
        std::uint64_t native = 0;
        while (std::getline(maps, line)) {
            if (line.find("libc.so") == std::string::npos ||
                line.find("r--p 00000000") == std::string::npos)
                continue;
            native = std::strtoull(line.c_str(), nullptr, 16);
            break;
        }
        assert(native > 0xffffffffull);
        char bytes[65536];
        std::uint64_t read_bytes = 0;
        assert(read(process, native, bytes, sizeof(bytes), &read_bytes) == 0 &&
               read_bytes == sizeof(bytes));
        assert(!std::memcmp(bytes,
                            "\x7f"
                            "ELF",
                            4));
        bool found = false;
        StableTextStore<1024, 64> text_store;
        for (std::size_t start = 0; start + 65 < sizeof(bytes); ++start) {
            std::size_t length = 0;
            while (length < 64 && bytes[start + length] >= 32 && bytes[start + length] < 127)
                ++length;
            if (length < 3 || bytes[start + length] != '\0')
                continue;
            TextStatus status;
            const char *text = fresh_text(
                NativeBuffer{native + start, length + 1},
                [&](std::uint64_t at, void *out, std::size_t count) {
                    std::uint64_t copied_bytes = 0;
                    return read(process, at, out, count, &copied_bytes) == 0 &&
                           copied_bytes == count;
                },
                text_store, status);
            assert(text && status == TextStatus::fresh && !std::strcmp(text, bytes + start));
            found = true;
            break;
        }
        assert(found);
        CloseHandle(process);
        std::puts("Actual native Unix high-address string read passed (own process only).");
        return 0;
    }
    std::uint64_t address = 0x100000000ull, size = 4096, copied = 0;
    const LONG allocated =
        allocate(process, &address, 0, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (allocated != 0) {
        std::printf("64-bit allocation unsupported: status=%08lx\n",
                    static_cast<unsigned long>(allocated));
        CloseHandle(process);
        return 77;
    }
    assert(address >= 0x100000000ull);
    char source[] = "first player";
    assert(write(process, address, source, sizeof(source), &copied) == 0 &&
           copied == sizeof(source));
    StableTextStore<1024, 64> store;
    TextStatus status;
    auto reader = [&](std::uint64_t at, void *out, std::size_t length) {
        std::uint64_t count = 0;
        return read(process, at, out, length, &count) == 0 && count == length;
    };
    const NativeBuffer buffer{address, sizeof(source)};
    const char *first = fresh_text(buffer, reader, store, status);
    assert(first && status == TextStatus::fresh && !std::strcmp(first, "first player"));
    std::memcpy(source, "other player", sizeof(source));
    assert(write(process, address, source, sizeof(source), &copied) == 0 &&
           copied == sizeof(source));
    const char *second = fresh_text(buffer, reader, store, status);
    assert(second && !std::strcmp(second, "other player") && !std::strcmp(first, "first player"));
    CloseHandle(process);
    std::puts("Actual x86-to-64-bit read and immutable text test passed.");
}
