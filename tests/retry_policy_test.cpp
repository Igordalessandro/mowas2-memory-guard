#include "diagnostics/retry_policy.h"

#include <cassert>
#include <initializer_list>

int main() {
    using allocator_profiler::may_retry_virtual_allocation;
    using allocator_profiler::is_resource_exhaustion_error;
    for (unsigned error : {8u, 14u, 1450u, 1455u, 1816u}) {
        assert(is_resource_exhaustion_error(error));
    }
    assert(!is_resource_exhaustion_error(87));
    assert(!is_resource_exhaustion_error(487));
    assert(may_retry_virtual_allocation(0, 4096, 0x2000, 8));
    assert(may_retry_virtual_allocation(0, 4096, 0x3000, 14));
    assert(may_retry_virtual_allocation(0, 4096, 0x1000, 8));
    assert(may_retry_virtual_allocation(0x10000, 4096, 0x2000, 8));
    assert(may_retry_virtual_allocation(0, 4096, 0x102000, 8));
    assert(!may_retry_virtual_allocation(0x10000, 4096, 0x1000, 8));
    assert(!may_retry_virtual_allocation(0, 4096, 0x2000, 87));
    assert(!may_retry_virtual_allocation(0, 4096, 0x2000, 487));
    assert(!may_retry_virtual_allocation(0, 4096, 0x2000, 1455));
    assert(!may_retry_virtual_allocation(0, 4096, 0x80000, 8));
    assert(!may_retry_virtual_allocation(0, 4096, 0x1000000, 8));
    assert(!may_retry_virtual_allocation(0, 4096, 0x402000, 8));
    assert(!may_retry_virtual_allocation(0, 4096, 0, 8));
    assert(!may_retry_virtual_allocation(0, 0, 0x2000, 8));
}
