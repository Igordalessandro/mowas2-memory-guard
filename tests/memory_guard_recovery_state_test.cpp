#include "orchestration/recovery_state.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

namespace {

using State = memory_guard::RecoveryState<std::mutex>;

void retry_ownership_and_failed_rearm() {
    State state;
    int original = 0;
    int replacement = 0;
    state.initialize(&original, 32);

    assert(state.release(7, [](State::Reservation) { return false; }) == 0);
    assert(state.snapshot().reserve.address == &original);
    assert(state.release(7, [](State::Reservation reserve) {
        assert(reserve.bytes == 32);
        return true;
    }) == 32);
    assert(!state.authorize_rearm(8));
    bool attempted = false;
    assert(!state.try_rearm([&]() -> State::Reservation {
        attempted = true;
        return {&replacement, 16};
    }));
    assert(!attempted);

    assert(state.authorize_rearm(7));
    assert(!state.try_rearm([]() -> State::Reservation { return {nullptr, 0}; }));
    assert(state.snapshot().rearm_allowed);
    assert(state.try_rearm([&]() -> State::Reservation { return {&replacement, 16}; }));
    auto snapshot = state.snapshot();
    assert(snapshot.reserve.address == &replacement);
    assert(snapshot.reserve.bytes == 16);
    assert(!snapshot.rearm_allowed);
    assert(snapshot.retry_owner == 0);
    assert(state.release(8, [](State::Reservation) { return true; }) == 16);
    assert(!state.authorize_rearm(7));
}

// Observe an actual lock attempt, allowing the regression to coordinate the
// competing release without sleeps or assuming either thread's scheduling.
struct ObservedMutex {
    std::mutex value;
    static std::mutex observer_mutex;
    static std::condition_variable observer_cv;
    static unsigned attempts;

    void lock() {
        {
            std::lock_guard<std::mutex> guard(observer_mutex);
            ++attempts;
        }
        observer_cv.notify_all();
        value.lock();
    }
    bool try_lock() { return value.try_lock(); }
    void unlock() { value.unlock(); }
};

std::mutex ObservedMutex::observer_mutex;
std::condition_variable ObservedMutex::observer_cv;
unsigned ObservedMutex::attempts = 0;

void release_cannot_lose_new_retry_owner_during_rearm() {
    using ConcurrentState = memory_guard::RecoveryState<ObservedMutex>;
    ConcurrentState state;
    int original = 0;
    int replacement = 0;
    state.initialize(&original, 32);
    assert(state.release(11, [](ConcurrentState::Reservation) { return true; }) == 32);
    assert(state.authorize_rearm(11));

    std::promise<void> reserve_entered;
    std::promise<void> finish_reserve;
    auto reserve_resume = finish_reserve.get_future();
    std::thread rearm([&] {
        assert(state.try_rearm([&]() -> ConcurrentState::Reservation {
            reserve_entered.set_value();
            reserve_resume.wait();
            return {&replacement, 24};
        }));
    });
    reserve_entered.get_future().wait();

    unsigned initial_attempts;
    {
        std::lock_guard<std::mutex> guard(ObservedMutex::observer_mutex);
        initial_attempts = ObservedMutex::attempts;
    }
    std::promise<void> release_callback_entered;
    auto release_callback = release_callback_entered.get_future();
    std::thread release([&] {
        assert(state.release(22, [&](ConcurrentState::Reservation reserve) {
            assert(reserve.address == &replacement);
            assert(reserve.bytes == 24);
            release_callback_entered.set_value();
            return true;
        }) == 24);
    });
    {
        std::unique_lock<std::mutex> guard(ObservedMutex::observer_mutex);
        ObservedMutex::observer_cv.wait(guard, [&] {
            return ObservedMutex::attempts > initial_attempts;
        });
    }
    assert(release_callback.wait_for(std::chrono::milliseconds(0)) ==
           std::future_status::timeout);
    finish_reserve.set_value();
    rearm.join();
    release.join();

    const auto snapshot = state.snapshot();
    assert(snapshot.reserve.address == nullptr);
    assert(snapshot.reserve.bytes == 0);
    assert(snapshot.retry_owner == 22);
    assert(!snapshot.rearm_allowed);
    assert(!state.authorize_rearm(11));
    assert(state.authorize_rearm(22));
}

} // namespace

int main() {
    retry_ownership_and_failed_rearm();
    release_cannot_lose_new_retry_owner_during_rearm();
}
