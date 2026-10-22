#include "diagnostics/large_allocation_history.h"

#include <atomic>
#include <cassert>
#include <thread>

namespace {
struct TestLock {
    std::atomic_flag held = ATOMIC_FLAG_INIT;
    void lock() { while (held.test_and_set(std::memory_order_acquire)) {} }
    void unlock() { held.clear(std::memory_order_release); }
};
using History = allocator_profiler::LargeAllocationHistory<TestLock>;
using Event = allocator_profiler::LargeAllocationEvent;

Event make_event(std::uint32_t identity, std::size_t size = History::minimum_bytes) {
    Event event {};
    event.tick = event.thread_id = event.caller_rva = event.engine_rva = identity;
    event.requested = size;
    event.engine_requested = size + identity;
    event.result = event.heap = identity;
    event.request_flags = event.flags = identity;
    event.kind = allocator_profiler::LargeAllocationKind::heap;
    return event;
}

void assert_intact(const Event& event) {
    assert(event.sequence != 0);
    assert(event.tick == event.thread_id && event.tick == event.caller_rva);
    assert(event.tick == event.engine_rva && event.tick == event.result);
    assert(event.tick == event.heap && event.tick == event.request_flags);
    assert(event.tick == event.flags);
    assert(event.engine_requested == event.requested + event.tick);
}
}

int main() {
    History history;
    Event snapshot[32] {};
    assert(history.snapshot(snapshot, 32) == 0);
    history.record(make_event(1));
    for (unsigned i = 0; i < 100000; ++i) history.record(make_event(i, 4096));
    assert(history.snapshot(snapshot, 32) == 1);
    assert(snapshot[0].tick == 1 && snapshot[0].sequence == 1);
    for (std::uint32_t i = 2; i <= 70; ++i) history.record(make_event(i));
    assert(history.snapshot(snapshot, 32) == 32);
    assert(snapshot[0].tick == 39 && snapshot[31].tick == 70);
    assert(history.snapshot(snapshot, 16) == 16);
    assert(snapshot[0].tick == 55 && snapshot[15].tick == 70);

    // Small ordinary traffic stays excluded, but a real failure must retain
    // its exact native/engine sizes even below the large-request threshold.
    History failures;
    Event small_failure = make_event(7, 699211);
    small_failure.engine_requested = 699192;
    small_failure.initial_failure = true;
    failures.record(small_failure);
    for (unsigned i = 0; i < 100000; ++i) failures.record(make_event(i, 48));
    assert(failures.snapshot(snapshot, 32) == 1);
    assert(snapshot[0].requested == 699211 && snapshot[0].engine_requested == 699192);
    assert(snapshot[0].initial_failure);

    History concurrent;
    std::atomic<unsigned> complete {0};
    std::thread writers[4];
    for (unsigned worker = 0; worker < 4; ++worker) {
        writers[worker] = std::thread([&concurrent, &complete, worker] {
            for (unsigned i = 1; i <= 10000; ++i) {
                concurrent.record(make_event(worker * 10000u + i));
            }
            complete.fetch_add(1, std::memory_order_release);
        });
    }
    while (complete.load(std::memory_order_acquire) != 4) {
        const std::size_t count = concurrent.snapshot(snapshot, 32);
        for (std::size_t i = 0; i < count; ++i) {
            assert_intact(snapshot[i]);
            if (i) assert(snapshot[i].sequence == snapshot[i - 1].sequence + 1);
        }
    }
    for (std::thread& writer : writers) writer.join();
    assert(concurrent.snapshot(snapshot, 32) == 32);
    assert(snapshot[31].sequence == 40000);
    for (const Event& event : snapshot) assert_intact(event);
}
