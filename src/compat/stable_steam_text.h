#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
namespace steam_text_guard {
struct NativeBuffer {
    std::uint64_t address, length;
};
static_assert(sizeof(NativeBuffer) == 16, "Proton x86 u_buffer ABI");
constexpr std::size_t MAX_TEXT_BYTES = 4096;
// Append-only content-keyed strings; never mutate/evict a returned pointer.
// Caller serializes access. No dynamic allocations or native-address keys.
template <std::size_t ByteCapacity, std::size_t SlotCapacity> class StableTextStore {
    static_assert(ByteCapacity && SlotCapacity, "nonempty store");
    struct Entry {
        std::uint64_t hash = 0;
        std::size_t offset = 0, length = 0;
    };
    Entry entries_[SlotCapacity] = {};
    alignas(16) char bytes_[ByteCapacity] = {};
    std::size_t used_ = 0, count_ = 0;

  public:
    const char *intern(const char *text, std::size_t length) {
        if (!text || !length || text[length - 1] || std::memchr(text, '\0', length - 1))
            return nullptr;
        std::uint64_t hash = 14695981039346656037ull;
        for (std::size_t i = 0; i < length; ++i) {
            hash ^= static_cast<unsigned char>(text[i]);
            hash *= 1099511628211ull;
        }
        std::size_t slot = static_cast<std::size_t>(hash % SlotCapacity);
        for (std::size_t probe = 0; probe < SlotCapacity; ++probe) {
            Entry &entry = entries_[slot];
            if (!entry.length) {
                // Preserve at least the alignment of the bridge's heap copy,
                // including for a text-like const binary buffer.
                const std::size_t padding = (16 - used_ % 16) % 16;
                if (padding > ByteCapacity - used_ || length > ByteCapacity - used_ - padding)
                    return nullptr;
                const std::size_t offset = used_ + padding;
                std::memcpy(bytes_ + offset, text, length);
                entry = {hash, offset, length};
                used_ = offset + length;
                ++count_;
                return bytes_ + entry.offset;
            }
            if (entry.hash == hash && entry.length == length &&
                !std::memcmp(bytes_ + entry.offset, text, length))
                return bytes_ + entry.offset;
            slot = (slot + 1) % SlotCapacity;
        }
        return nullptr;
    }
    std::size_t bytes_used() const { return used_; }
    std::size_t count() const { return count_; }
};
enum class TextStatus { fresh, passthrough, too_large, read_failed, not_text, full };
template <typename Reader, typename Store>
const char *fresh_text(NativeBuffer buffer, Reader &&read, Store &store, TextStatus &status) {
    status = TextStatus::passthrough;
    constexpr std::uint64_t x86_max = 0xffffffffull;
    if (!buffer.address || !buffer.length ||
        buffer.length > std::numeric_limits<std::uint64_t>::max() - buffer.address ||
        (buffer.address <= x86_max && buffer.length <= x86_max - buffer.address))
        return nullptr;
    if (buffer.length > MAX_TEXT_BYTES) {
        status = TextStatus::too_large;
        return nullptr;
    }
    char text[MAX_TEXT_BYTES];
    const auto length = static_cast<std::size_t>(buffer.length);
    if (!read(buffer.address, text, length)) {
        status = TextStatus::read_failed;
        return nullptr;
    }
    if (text[length - 1] || std::memchr(text, '\0', length - 1)) {
        status = TextStatus::not_text;
        return nullptr;
    }
    const char *result = store.intern(text, length);
    status = result ? TextStatus::fresh : TextStatus::full;
    return result;
}
} // namespace steam_text_guard
