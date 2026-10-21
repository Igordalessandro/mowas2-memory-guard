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
}
