#pragma once

#include <cstddef>

// Portable, allocation-free formatting shared by the DLL and native tests.
namespace compact_report {

constexpr std::size_t max_bytes = 900;

struct Fields {
    const char* session;
    const char* crash;
    const char* failure;
    const char* memory;
    const char* config;
    const char* allocator;
    const char* hotspot;
    const char* event;
    const char* full_log = nullptr;
    const char* coverage = nullptr;
};

inline bool append_line(char* output, std::size_t limit, std::size_t& used,
                        const char* line) {
    if (!line || !line[0]) return true;
    std::size_t length = 0;
    while (line[length] && length <= limit) ++length;
    if (length > limit || used > limit || limit - used < length + 2) return false;
    for (std::size_t i = 0; i < length; ++i) {
        const unsigned char character = static_cast<unsigned char>(line[i]);
        output[used++] = character >= 32 && character <= 126
            ? static_cast<char>(character) : '?';
    }
    output[used++] = '\r';
    output[used++] = '\n';
    output[used] = '\0';
    return true;
}

// Capacity includes the trailing NUL; return value counts only file bytes.
// Source fields are already bounded by the logger's fixed-size buffers.
inline std::size_t build(char* output, std::size_t capacity, const Fields& fields) {
    if (!output || capacity == 0) return 0;
    output[0] = '\0';
    const std::size_t limit = capacity - 1 < max_bytes ? capacity - 1 : max_bytes;
    std::size_t used = 0;
    append_line(output, limit, used, "DLL Crash Report");
    const char* essential[] = {fields.session, fields.crash, fields.failure, fields.memory};
    for (const char* line : essential) append_line(output, limit, used, line);

    // Correlation is essential: budget it before optional details can consume
    // the remaining space. Actual production core buffers leave room for it.
    append_line(output, limit, used, fields.full_log);
    constexpr char omitted[] = "omitted=optional_fields";
    constexpr std::size_t marker_bytes = sizeof(omitted) - 1 + 2;
    const std::size_t optional_limit = limit >= marker_bytes ? limit - marker_bytes : 0;
    const char* optional[] = {fields.hotspot, fields.coverage, fields.config, fields.allocator, fields.event};
    bool did_omit = false;
    for (const char* line : optional) {
        if (!append_line(output, optional_limit, used, line)) did_omit = true;
    }
    if (did_omit) append_line(output, limit, used, omitted);
    append_line(output, limit, used, "Add mods, last action, error text.");
    return used;
}

}  // namespace compact_report
