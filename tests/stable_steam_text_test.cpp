#include "compat/stable_steam_text.h"
#include <cassert>
#include <cstring>
#include <limits>
using namespace steam_text_guard;
int main() {
    StableTextStore<4096, 128> store;
    constexpr std::uint64_t address = 0x555500000000ull;
    const char *source = "original player";
    unsigned reads = 0;
    bool fail = false;
    auto read = [&](std::uint64_t at, void *out, std::size_t bytes) {
        ++reads;
        assert(at == address);
        if (fail)
            return false;
        std::memcpy(out, source, bytes);
        return true;
    };
    TextStatus status;
    auto get = [&] {
        return fresh_text(NativeBuffer{address, std::strlen(source) + 1}, read, store, status);
    };
    const char *first = get();
    assert(status == TextStatus::fresh && !std::strcmp(first, source));
    assert(get() == first && store.count() == 1);
    source = "another player";
    const char *second = get();
    assert(reinterpret_cast<std::uintptr_t>(first) % 16 == 0);
    assert(reinterpret_cast<std::uintptr_t>(second) % 16 == 0);
    assert(second != first && !std::strcmp(second, source));
    source = "0";
    const char *zero = get();
    assert(!std::strcmp(zero, "0"));
    source = "0x00000000";
    assert(!std::strcmp(get(), source));
    source = "[USA] Oasis Defense [GER]";
    assert(!std::strcmp(get(), source));
    source = "\xc3\xa9\xe6\x97\xa5";
    assert(!std::strcmp(get(), source));
    source = "";
    assert(!std::strcmp(get(), ""));
    assert(!std::strcmp(first, "original player") && !std::strcmp(second, "another player") &&
           !std::strcmp(zero, "0"));
    fail = true;
    assert(!get() && status == TextStatus::read_failed);
    fail = false;
    const unsigned before = reads;
    assert(!fresh_text(NativeBuffer{123, 4}, read, store, status));
    assert(status == TextStatus::passthrough && reads == before);
    assert(!fresh_text(NativeBuffer{address, 0}, read, store, status));
    assert(!fresh_text(NativeBuffer{address, MAX_TEXT_BYTES + 1}, read, store, status));
    assert(status == TextStatus::too_large && reads == before);
    assert(!fresh_text(NativeBuffer{std::numeric_limits<std::uint64_t>::max(), 4}, read, store,
                       status) &&
           reads == before);
    const char binary[] = {'a', '\0', 'b', '\0'};
    source = binary;
    assert(!fresh_text(NativeBuffer{address, sizeof(binary)}, read, store, status) &&
           status == TextStatus::not_text);
    const char unterminated[] = {'a', 'b'};
    source = unterminated;
    assert(!fresh_text(NativeBuffer{address, sizeof(unterminated)}, read, store, status) &&
           status == TextStatus::not_text);
    StableTextStore<5, 8> small;
    const char *pinned = small.intern("abcd", 5);
    assert(pinned && !small.intern("e", 2) && small.intern("abcd", 5) == pinned &&
           small.bytes_used() == 5);
    StableTextStore<32, 1> one_slot;
    assert(one_slot.intern("first", 6) && !one_slot.intern("second", 7) && one_slot.count() == 1);
}
