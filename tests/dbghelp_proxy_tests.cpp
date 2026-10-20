// Exercises the production comparison helper against disposable Windows files.
// No proxy initialization, LoadLibrary, hooks, game or Steam calls are made.
#include <cassert>
#include <cwchar>
#include <string>
#include <vector>
#include "../src/app/dbghelp_proxy.cpp"

namespace logger {
bool init() { assert(false); return false; }
void log(const char*, ...) { assert(false); }
}

namespace {
struct TempFile {
    wchar_t path[MAX_PATH] = {};
    TempFile() {
        wchar_t directory[MAX_PATH] = {};
        const DWORD length = GetTempPathW(MAX_PATH, directory);
        assert(length > 0 && length < MAX_PATH);
        assert(GetTempFileNameW(directory, L"mgt", 0, path) != 0);
    }
    ~TempFile() { assert(DeleteFileW(path)); }
    void write(const std::vector<unsigned char>& bytes) const {
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        assert(file != INVALID_HANDLE_VALUE);
        if (!bytes.empty()) {
            DWORD written = 0;
            assert(WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                              &written, nullptr));
            assert(written == bytes.size());
        }
        assert(CloseHandle(file));
    }
};
}

int main() {
    TempFile source;
    TempFile candidate;
    // Cross multiple 4096-byte chunks and end with a partial chunk, so neither
    // a header-only comparison nor a loop that skips its tail passes this test.
    std::vector<unsigned char> bytes(3 * 4096 + 13);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<unsigned char>((i * 37u + 11u) & 255u);
    }
    source.write(bytes);
    candidate.write(bytes);
    assert(same_file_contents(source.path, candidate.path));

    bytes[0] ^= 1u;
    candidate.write(bytes);
    assert(!same_file_contents(source.path, candidate.path));
    bytes[0] ^= 1u;
    bytes.back() ^= 1u;
    candidate.write(bytes);
    assert(!same_file_contents(source.path, candidate.path));
    bytes.back() ^= 1u;
    bytes.pop_back();
    candidate.write(bytes);
    assert(!same_file_contents(source.path, candidate.path));

    // An existing writable handle makes verification fail closed; after it is
    // closed the helper must have released all its own handles on every path.
    bytes.push_back(static_cast<unsigned char>((bytes.size() * 37u + 11u) & 255u));
    candidate.write(bytes);
    assert(same_file_contents(source.path, candidate.path));
    HANDLE writer = CreateFileW(candidate.path, GENERIC_WRITE, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(writer != INVALID_HANDLE_VALUE);
    assert(!same_file_contents(source.path, candidate.path));
    assert(CloseHandle(writer));
    const std::wstring missing = std::wstring(source.path) + L"\\missing.dll";
    assert(!same_file_contents(source.path, missing.c_str()));
    assert(!same_file_contents(missing.c_str(), candidate.path));

    bytes.clear();
    source.write(bytes);
    candidate.write(bytes);
    assert(!same_file_contents(source.path, candidate.path));
    return 0;
}
