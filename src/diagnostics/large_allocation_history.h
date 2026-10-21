#pragma once

#include <cstddef>
#include <cstdint>

namespace allocator_profiler {

enum class LargeAllocationKind : std::uint32_t { engine, heap, realloc, virtual_alloc };

struct LargeAllocationEvent {
    std::uint64_t sequence;
    std::uint32_t tick;
    std::uint32_t thread_id;
    std::uint32_t caller_rva;
    std::uint32_t engine_rva;
    std::size_t requested;
    std::size_t engine_requested;
    std::uintptr_t result;
    std::uintptr_t heap;
    std::uint32_t request_flags;
    std::uint32_t flags;
    LargeAllocationKind kind;
};

// The lock adapter supplies allocation-free lock()/unlock(). Both producers
// and readers use the same lock: no torn slots or concurrent-writer races.
// Callers must copy a snapshot before performing any logging or OS queries.
template<class Lock, std::size_t Capacity = 32>
class LargeAllocationHistory {
public:
    static constexpr std::size_t minimum_bytes = 1024u * 1024u;

    void record(LargeAllocationEvent event) {
        if (event.requested < minimum_bytes) return;
        lock_.lock();
        event.sequence = ++sequence_;
        entries_[next_] = event;
        next_ = (next_ + 1) % Capacity;
        if (count_ < Capacity) ++count_;
        lock_.unlock();
    }

    // Copies the newest events in chronological completion order.
    std::size_t snapshot(LargeAllocationEvent* output, std::size_t capacity) {
        lock_.lock();
        const std::size_t copied = count_ < capacity ? count_ : capacity;
        const std::size_t first = (next_ + Capacity - copied) % Capacity;
        for (std::size_t i = 0; i < copied; ++i) {
            output[i] = entries_[(first + i) % Capacity];
        }
        lock_.unlock();
        return copied;
    }

private:
    static_assert(Capacity > 0, "History must retain at least one event");
    Lock lock_ {};
    LargeAllocationEvent entries_[Capacity] {};
    std::uint64_t sequence_ = 0;
    std::size_t next_ = 0;
    std::size_t count_ = 0;
};

} // namespace allocator_profiler
