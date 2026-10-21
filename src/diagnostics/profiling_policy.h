#pragma once
#include <cstddef>
#include <cstdint>

namespace diagnostics {
// Thread-owned, fixed-size cache. A valid null value caches a saturated-table
// miss; colliding keys must never be confused with each other.
template<typename Value, std::size_t Capacity = 64>
class CallerCache {
    static_assert(Capacity && !(Capacity & (Capacity - 1)), "power-of-two capacity");
    struct Entry { std::uint32_t key = 0; Value value{}; bool valid = false; };
    Entry entries_[Capacity]{};
    static std::size_t index(std::uint32_t key) {
        key ^= key >> 16;
        key *= 0x7feb352du;
        key ^= key >> 15;
        return key & (Capacity - 1);
    }
public:
    bool lookup(std::uint32_t key, Value& value) const {
        const Entry& entry = entries_[index(key)];
        if (!entry.valid || entry.key != key) return false;
        value = entry.value;
        return true;
    }
    void remember(std::uint32_t key, Value value) { entries_[index(key)] = {key, value, true}; }
    void clear() { for (auto& entry : entries_) entry = {}; }
};

} // namespace diagnostics
