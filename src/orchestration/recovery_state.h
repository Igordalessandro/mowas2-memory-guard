#pragma once

#include <cstddef>
#include <cstdint>

namespace memory_guard {

// The mutex adapter supplies lock(), try_lock(), and unlock(). Keeping the
// transition logic independent of Windows lets native tests exercise the same
// ownership and publication rules used by the injected DLL.
template <typename Mutex>
class RecoveryState {
public:
    struct Reservation {
        void* address;
        std::size_t bytes;
    };

    struct Snapshot {
        Reservation reserve;
        bool rearm_allowed;
        std::uint32_t retry_owner;
    };

    void initialize(void* address, std::size_t bytes) {
        Guard guard(mutex_, false);
        reserve_ = {address, address ? bytes : 0};
        phase_ = Phase::protected_reserve;
        retry_owner_ = 0;
    }

    Snapshot snapshot() {
        Guard guard(mutex_, false);
        return {reserve_, phase_ == Phase::rearm_allowed, retry_owner_};
    }

    // Freeing the range and publishing the retry owner are a single
    // transaction. A failed free leaves the protected reserve unchanged.
    template <typename Release>
    std::size_t release(std::uint32_t owner, Release release_range) {
        Guard guard(mutex_, false);
        if (!reserve_.address || !release_range(reserve_)) return 0;
        const std::size_t released = reserve_.bytes;
        reserve_ = {nullptr, 0};
        phase_ = Phase::awaiting_retry;
        retry_owner_ = owner;
        return released;
    }

    // Only the thread whose allocation consumed the reserve can confirm that
    // retry succeeded. Until then, monitoring must not take the space back.
    bool authorize_rearm(std::uint32_t owner) {
        Guard guard(mutex_, false);
        if (phase_ == Phase::awaiting_retry && retry_owner_ == owner) {
            phase_ = Phase::rearm_allowed;
        }
        return phase_ == Phase::rearm_allowed;
    }

    template <typename Rearm>
    bool try_rearm(Rearm reserve_range) {
        Guard guard(mutex_, true);
        if (!guard.acquired) return false;
        if (reserve_.address) return true;
        if (phase_ != Phase::rearm_allowed) return false;
        const Reservation replacement = reserve_range();
        if (!replacement.address || replacement.bytes == 0) return false;

        // Publication and metadata reset stay under the same lock as release.
        // A new release cannot have its owner/pending state cleared afterward.
        reserve_ = replacement;
        phase_ = Phase::protected_reserve;
        retry_owner_ = 0;
        return true;
    }

private:
    enum class Phase { protected_reserve, awaiting_retry, rearm_allowed };

    struct Guard {
        Mutex& mutex;
        bool acquired;
        Guard(Mutex& value, bool try_only) : mutex(value), acquired(false) {
            if (try_only) acquired = mutex.try_lock();
            else { mutex.lock(); acquired = true; }
        }
        ~Guard() { if (acquired) mutex.unlock(); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    };

    Mutex mutex_;
    Reservation reserve_ = {nullptr, 0};
    Phase phase_ = Phase::protected_reserve;
    std::uint32_t retry_owner_ = 0;
};

} // namespace memory_guard
