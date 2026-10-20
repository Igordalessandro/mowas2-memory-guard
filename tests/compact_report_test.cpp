#include "shared/logging/compact_report.h"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace {

void check_bytes(const char* output, std::size_t used, std::size_t capacity) {
    assert(used <= compact_report::max_bytes);
    assert(used < capacity);
    assert(std::strlen(output) == used);
    if (used) assert(used >= 2 && output[used - 2] == '\r' && output[used - 1] == '\n');
    for (std::size_t i = 0; i < used; ++i) {
        const unsigned char character = static_cast<unsigned char>(output[i]);
        if (character == '\r') {
            assert(i + 1 < used && output[i + 1] == '\n');
        } else if (character == '\n') {
            assert(i > 0 && output[i - 1] == '\r');
        } else {
            assert(character >= 32 && character <= 126);
        }
    }
}

void fill_field(char* field, std::size_t size, const char* prefix) {
    const std::size_t prefix_size = std::strlen(prefix);
    assert(prefix_size + 1 < size);
    std::memcpy(field, prefix, prefix_size);
    std::memset(field + prefix_size, '9', size - prefix_size - 1);
    field[size - 1] = '\0';
}

void maximum_fields_preserve_essentials() {
    // These are the actual logger buffer limits, including their NUL bytes.
    char session[160], crash[160], failure[144], memory[160];
    char config[128], allocator[160], hotspot[128], event[128];
    fill_field(session, sizeof(session), "session=");
    fill_field(crash, sizeof(crash), "crash=");
    fill_field(failure, sizeof(failure), "failure=");
    fill_field(memory, sizeof(memory), "mem=");
    fill_field(config, sizeof(config), "guard=");
    fill_field(allocator, sizeof(allocator), "alloc=");
    fill_field(hotspot, sizeof(hotspot), "hot=");
    fill_field(event, sizeof(event), "event=");
    const compact_report::Fields fields = {
        session, crash, failure, memory, config, allocator, hotspot, event
    };
    char output[901];
    const std::size_t used = compact_report::build(output, sizeof(output), fields);
    check_bytes(output, used, sizeof(output));
    const char* essential[] = {session, crash, failure, memory};
    for (const char* line : essential) {
        const char* found = std::strstr(output, line);
        assert(found && found[std::strlen(line)] == '\r');
    }
    const char* optional[] = {config, allocator, hotspot, event};
    const char* prefixes[] = {"guard=", "alloc=", "hot=", "event="};
    for (std::size_t i = 0; i < 4; ++i) {
        if (const char* found = std::strstr(output, prefixes[i])) {
            assert(std::strncmp(found, optional[i], std::strlen(optional[i])) == 0);
            assert(found[std::strlen(optional[i])] == '\r');
        }
    }
    assert(std::strstr(output, "omitted=optional_fields\r\n"));
    assert(std::strstr(output, "session=") < std::strstr(output, "crash="));
    assert(std::strstr(output, "crash=") < std::strstr(output, "failure="));
    assert(std::strstr(output, "failure=") < std::strstr(output, "mem="));
}

void realistic_large_counters() {
    const compact_report::Fields fields = {
        "session v=2.5.0-experimental.8 build=Oct 20 2026_14:14:24 pid=4294967295 env=native-windows process=x86 native=x64 wow64=yes ram=65536MiB",
        "crash=unhandled code=C0000005 address=FFFFFFFF thread=4294967295 ip=FFFFFFFF sp=FFFFFFFF params=15",
        "failure=HeapAlloc bytes=4294967295 site=+FFFFFFFF released=96MiB retries=4 result=failed",
        "mem free=55MiB largest=512KiB commit=3715MiB reserve=324MiB guard=0MiB private=3334MiB ws=3324MiB req=9223372036854775807 total=999999999MiB max=4194303KiB",
        "guard=8/8/48/32MiB release=192/24,96/8MiB final=segmented report_max=900B",
        "alloc calls=9223372036854775807 realloc=9223372036854775807 free=9223372036854775807 fail=999 requested=999999999MiB sampled_live_est=99999MiB drops=0 sites_saturated=1",
        "hot req=+0093176C/1177MiB/115890calls retained=+0003D7CE/96MiB/1260blocks",
        "event=reserve_release stage=recovery size=32MiB reason=allocation failure"
    };
    char output[901];
    const std::size_t used = compact_report::build(output, sizeof(output), fields);
    check_bytes(output, used, sizeof(output));
    assert(std::strstr(output, fields.session));
    assert(std::strstr(output, fields.crash));
    assert(std::strstr(output, fields.failure));
    assert(std::strstr(output, fields.memory));
}

void sanitize_and_small_capacity() {
    const compact_report::Fields fields = {
        "session=wine\nforged\t\xC3\xA9", "crash=none", "failure=none", "mem=ok",
        "guard=ok", "alloc=ok", "hot=ok", "event=ok"
    };
    char output[901];
    const std::size_t used = compact_report::build(output, sizeof(output), fields);
    check_bytes(output, used, sizeof(output));
    assert(std::strstr(output, "session=wine?forged???\r\n"));
    assert(std::strstr(output, "Add mods, last action, error text.\r\n"));
    for (std::size_t capacity = 1; capacity < 100; ++capacity) {
        char guarded[102];
        std::memset(guarded, '!', sizeof(guarded));
        const std::size_t count = compact_report::build(guarded + 1, capacity, fields);
        check_bytes(guarded + 1, count, capacity);
        assert(guarded[0] == '!' && guarded[capacity + 1] == '!');
    }
    assert(compact_report::build(nullptr, 0, fields) == 0);
}

}  // namespace

int main() {
    maximum_fields_preserve_essentials();
    realistic_large_counters();
    sanitize_and_small_capacity();
    std::puts("compact report tests passed");
}
