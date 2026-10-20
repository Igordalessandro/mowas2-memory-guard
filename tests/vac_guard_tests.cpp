// Standalone policy regression executable using the production guard bodies.
// The tests never install hooks or invoke Steam. Dialogs and MinHook entry
// points are test doubles; delegates only capture their arguments in memory.
#include <windows.h>
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace {
unsigned dialog_calls = 0;
unsigned host_calls = 0;
unsigned join_calls = 0;
unsigned flush_calls = 0;
bool host_result = true;
int join_result = 7;
struct HostArguments {
    std::uint32_t ip;
    std::uint16_t steam_port;
    std::uint16_t game_port;
    std::uint16_t query_port;
    int mode;
    const char* version;
} host_arguments = {};
struct JoinArguments {
    void* self;
    void* blob;
    int capacity;
    std::uint64_t server_id;
    std::uint32_t ip;
    std::uint16_t port;
    bool secure;
} join_arguments = {};

int WINAPI mock_message_box(HWND owner, LPCWSTR message, LPCWSTR title, UINT flags) {
    assert(owner == nullptr && message && title);
    assert((flags & MB_ICONWARNING) != 0);
    ++dialog_calls;
    return IDOK;
}
}

#define MessageBoxW mock_message_box
#include "../src/safety/vac_host_guard.cpp"
#include "../src/safety/vac_join_guard.cpp"
#undef MessageBoxW

namespace logger {
void log(const char*, ...) { SetLastError(0x106); }
void flush_now() { ++flush_calls; SetLastError(0xF105); }
}

// Installation is deliberately outside the test scope. These stubs fail if a
// future test accidentally tries to detour any function in this executable.
extern "C" {
MH_STATUS WINAPI MH_Initialize() { assert(false); return MH_UNKNOWN; }
MH_STATUS WINAPI MH_Uninitialize() { assert(false); return MH_UNKNOWN; }
MH_STATUS WINAPI MH_CreateHook(LPVOID, LPVOID, LPVOID*) {
    assert(false);
    return MH_UNKNOWN;
}
MH_STATUS WINAPI MH_EnableHook(LPVOID) { assert(false); return MH_UNKNOWN; }
MH_STATUS WINAPI MH_RemoveHook(LPVOID) { assert(false); return MH_UNKNOWN; }
}

namespace {
bool __cdecl fake_host(std::uint32_t ip, std::uint16_t steam_port,
                       std::uint16_t game_port, std::uint16_t query_port,
                       int mode, const char* version) {
    ++host_calls;
    host_arguments = {ip, steam_port, game_port, query_port, mode, version};
    return host_result;
}
int __thiscall fake_join(void* self, void* blob, int capacity,
                         std::uint64_t server_id, std::uint32_t ip,
                         std::uint16_t port, bool secure) {
    ++join_calls;
    join_arguments = {self, blob, capacity, server_id, ip, port, secure};
    assert(capacity >= 4 && blob);
    std::memcpy(blob, "TEST", 4);
    return join_result;
}
}

int main() {
    constexpr std::uint32_t ip = 0xF1234567u;
    constexpr std::uint16_t steam_port = 1111;
    constexpr std::uint16_t game_port = 2222;
    constexpr std::uint16_t query_port = 3333;
    const char version[] = "guard-policy-test";
    vac_host_guard::g_original_init = &fake_host;

    // Only the two explicit insecure modes may reach Steam. Secure mode 3,
    // unknown modes, negative modes and extreme integers all fail closed.
    const int rejected_modes[] = {-1, 0, 3, 4, INT_MIN, INT_MAX};
    for (int mode : rejected_modes) {
        const unsigned before = host_calls;
        const unsigned dialogs_before = dialog_calls;
        assert(!vac_host_guard::guarded_steam_game_server_init(
            ip, steam_port, game_port, query_port, mode, version));
        assert(host_calls == before && dialog_calls == dialogs_before + 1);
        assert(GetLastError() == ERROR_ACCESS_DISABLED_BY_POLICY);
        assert(vac_host_guard::g_warning_visible == 0);
    }
    for (int mode = -32; mode <= 32; ++mode) {
        assert(vac_host_guard::is_allowed_insecure_mode(mode) ==
               (mode == 1 || mode == 2));
    }
    for (int mode : {1, 2}) {
        for (bool result : {false, true}) {
            host_result = result;
            const unsigned before = host_calls;
            assert(vac_host_guard::guarded_steam_game_server_init(
                ip, steam_port, game_port, query_port, mode, version) == result);
            assert(host_calls == before + 1);
            assert(host_arguments.ip == ip && host_arguments.steam_port == steam_port);
            assert(host_arguments.game_port == game_port && host_arguments.query_port == query_port);
            assert(host_arguments.mode == mode && host_arguments.version == version);
        }
    }
    vac_host_guard::g_original_init = nullptr;
    const unsigned host_before = host_calls;
    assert(!vac_host_guard::guarded_steam_game_server_init(
        ip, steam_port, game_port, query_port, 1, version));
    assert(GetLastError() == ERROR_PROC_NOT_FOUND && host_calls == host_before);

    // The secure join path must leave the blob untouched and never call the
    // original authentication delegate, even if capacity is invalid.
    vac_join_guard::g_original_initiate = &fake_join;
    unsigned char blob[32];
    std::memset(blob, 0xA5, sizeof(blob));
    int user_object = 0;
    constexpr std::uint64_t server_id = 0xFEDCBA9876543210ull;
    constexpr std::uint16_t server_port = 65530;
    const unsigned joins_before = join_calls;
    for (int capacity : {-1, 0, 32}) {
        const unsigned dialogs_before = dialog_calls;
        assert(vac_join_guard::guarded_initiate_game_connection(
            &user_object, nullptr, blob, capacity, server_id, ip, server_port, true) == 0);
        assert(GetLastError() == ERROR_ACCESS_DISABLED_BY_POLICY);
        assert(join_calls == joins_before && dialog_calls == dialogs_before + 1);
        for (unsigned char value : blob) assert(value == 0xA5);
        assert(vac_join_guard::g_warning_visible == 0);
    }
    for (int result : {0, 7}) {
        join_result = result;
        const unsigned before = join_calls;
        assert(vac_join_guard::guarded_initiate_game_connection(
            &user_object, nullptr, blob, 32, server_id, ip, server_port, false) == result);
        assert(join_calls == before + 1);
        assert(join_arguments.self == &user_object && join_arguments.blob == blob);
        assert(join_arguments.capacity == 32 && join_arguments.server_id == server_id);
        assert(join_arguments.ip == ip && join_arguments.port == server_port);
        assert(!join_arguments.secure && std::memcmp(blob, "TEST", 4) == 0);
    }
    vac_join_guard::g_original_initiate = nullptr;
    const unsigned join_before = join_calls;
    assert(vac_join_guard::guarded_initiate_game_connection(
        &user_object, nullptr, blob, 32, server_id, ip, server_port, false) == 0);
    assert(GetLastError() == ERROR_PROC_NOT_FOUND && join_calls == join_before);

    // An already visible warning suppresses another dialog without bypassing
    // either policy boundary or forwarding a secure request.
    const unsigned dialogs_before = dialog_calls;
    vac_host_guard::g_warning_visible = 1;
    assert(!vac_host_guard::guarded_steam_game_server_init(
        ip, steam_port, game_port, query_port, 3, version));
    assert(GetLastError() == ERROR_ACCESS_DISABLED_BY_POLICY);
    vac_join_guard::g_warning_visible = 1;
    assert(vac_join_guard::guarded_initiate_game_connection(
        &user_object, nullptr, blob, 32, server_id, ip, server_port, true) == 0);
    assert(GetLastError() == ERROR_ACCESS_DISABLED_BY_POLICY);
    assert(dialog_calls == dialogs_before && host_calls == host_before && join_calls == join_before);
    assert(flush_calls > 0);
    return 0;
}
