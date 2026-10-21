#include "diagnostics/profiling_policy.h"
#include <cassert>
#include <cstdint>

int main() {
    diagnostics::CallerCache<int*> cache;
    int first = 1, second = 2;
    int* result = &first;
    assert(!cache.lookup(10, result) && result == &first);
    cache.remember(10, &first);
    cache.remember(20, &second);
    assert(cache.lookup(10, result) && result == &first);
    assert(cache.lookup(20, result) && result == &second);
    cache.remember(30, nullptr);
    assert(cache.lookup(30, result) && !result);
    cache.clear();
    assert(!cache.lookup(30, result));
    diagnostics::CallerCache<int*, 1> collision;
    collision.remember(10, &first);
    collision.remember(20, &second);
    assert(!collision.lookup(10, result));
    assert(collision.lookup(20, result) && result == &second);
    diagnostics::RequestSiteSampler sampler(42), identical(42), zero_seed(0);
    unsigned samples = 0, zero_samples = 0;
    for (unsigned i = 0; i < 1000000; ++i) {
        const bool take = sampler.take();
        assert(take == identical.take());
        samples += take ? 1u : 0u;
        zero_samples += zero_seed.take() ? 1u : 0u;
    }
    assert(samples > 800 && samples < 1200 && zero_samples > 800 && zero_samples < 1200);
    using diagnostics::ProfileMode;
    assert(diagnostics::profile_mode(nullptr) == ProfileMode::light);
    assert(diagnostics::profile_mode("") == ProfileMode::light);
    assert(diagnostics::profile_mode("light") == ProfileMode::light);
    assert(diagnostics::profile_mode("full") == ProfileMode::full);
    assert(diagnostics::profile_mode("ful") == ProfileMode::invalid);
}
