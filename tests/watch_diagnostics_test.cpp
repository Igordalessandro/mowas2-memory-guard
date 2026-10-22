#include "diagnostics/watch_diagnostics.h"
#include <cassert>
#include <cstring>
#include <limits>
#include <initializer_list>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
static int code_probe() { return 42; }
#endif

int main() {
    using namespace diagnostics;
    for (unsigned i = 0; i < WATCH_SITE_COUNT; ++i) assert(watch_index(WATCH_SITES[i]) == i);
    assert(watch_index(0) == WATCH_SITE_COUNT);
    AgeTotals ages;
    const std::uint32_t now = 100000;
    for (const std::uint32_t age : {0u, 999u, 1000u, 9999u, 10000u, 59999u, 60000u, 90000u})
        ages.add(64, now - age, now);
    assert(ages.blocks == 8 && ages.bytes == 512 && ages.oldest_ms == 90000);
    for (auto bytes : ages.age_bytes) assert(bytes == 128);
    AgeTotals wrap;
    wrap.add(0xffffffffu, 0xfffffff0u, 0x10u);
    wrap.add(0xffffffffu, 0xfffffff0u, 0x10u);
    assert(wrap.oldest_ms == 32 && wrap.bytes == 0x1fffffffeull);
    assert(wrap.age_bytes[0] == wrap.bytes);
    std::uint32_t rva = 999;
    assert(!image_rva(0x400000, 4096, 0x3fffff, rva) && rva == 999);
    assert(!image_rva(0x400000, 4096, 0x401000, rva));
    assert(image_rva(0x400000, 4096, 0x400123, rva) && rva == 0x123);
    unsigned reads = 0;
    std::uintptr_t address_read = 0;
    unsigned char code[CODE_BYTES]{};
    const auto reader = [&](std::uintptr_t address, void* output, std::size_t length) {
        ++reads;
        address_read = address;
        assert(length == CODE_BYTES);
        std::memset(output, 0x90, length);
        return true;
    };
    assert(!read_code_window(0x400000, 4096, 15, reader, code));
    assert(!read_code_window(0x400000, 4096, 4096, reader, code));
    assert(!read_code_window(0x400000, 32, 16, reader, code));
    assert(!read_code_window(std::numeric_limits<std::uintptr_t>::max() - 16, 4096, 64, reader, code));
    assert(reads == 0);
    assert(read_code_window(0x400000, 4096, 16, reader, code));
    assert(reads == 1 && address_read == 0x400000);
    for (unsigned char byte : code) assert(byte == 0x90);
    assert(read_code_window(0x400000, 4096, 4096 - CODE_BYTES + CODE_BEFORE, reader, code));
    assert(address_read == 0x401000 - CODE_BYTES);
    const auto failed_reader = [](std::uintptr_t, void*, std::size_t) { return false; };
    assert(!read_code_window(0x400000, 4096, 16, failed_reader, code));
#ifdef _WIN32
    // Real bounded read of this isolated test's executable code, not any
    // game process, allocation payload or other process's address space.
    const auto own_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(own_base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(own_base + static_cast<std::uintptr_t>(dos->e_lfanew));
    assert(image_rva(own_base, nt->OptionalHeader.SizeOfImage,
                     reinterpret_cast<std::uintptr_t>(&code_probe), rva));
    const auto own_reader = [](std::uintptr_t address, void* output, std::size_t length) {
        SIZE_T copied = 0;
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                                 output, length, &copied) && copied == length;
    };
    assert(read_code_window(own_base, nt->OptionalHeader.SizeOfImage, rva, own_reader, code));
    assert(code_probe() == 42);
#endif
}
