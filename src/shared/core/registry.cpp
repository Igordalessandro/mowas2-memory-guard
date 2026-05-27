#include "shared/core/module.h"
#include "shared/logging/logger.h"

#include <algorithm>
#include <vector>
#include <windows.h>

namespace core {

namespace {

struct Entry {
    ModuleSpec spec;
    bool       attempted;
    bool       installed;
};

// Heap-allocated to dodge static destruction order on DLL unload.
struct Registry {
    std::vector<Entry> entries;
};

static Registry* g_registry = nullptr;

static Registry& reg() {
    if (!g_registry) g_registry = new Registry();
    return *g_registry;
}

}  // anonymous namespace

void register_module(const ModuleSpec& spec) {
    reg().entries.push_back({spec, false, false});
}

int install_all(std::uintptr_t game_base) {
    auto& entries = reg().entries;

    std::stable_sort(entries.begin(), entries.end(),
                     [](const Entry& a, const Entry& b) {
                         return a.spec.priority < b.spec.priority;
                     });

    int ok_count = 0;
    for (auto& e : entries) {
        if (e.attempted) continue;
        if (!e.spec.install_fn) {
            e.attempted = true;
            e.installed = true;
            continue;
        }
        bool ok = false;
        __try {
            ok = e.spec.install_fn(game_base);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        e.attempted = true;
        e.installed = ok;
        if (ok) {
            ok_count++;
            logger::log("CORE : '%s' OK (priority=%d)", e.spec.name, e.spec.priority);
        } else {
            logger::log("CORE : '%s' FAILED (priority=%d, non-fatal)",
                        e.spec.name, e.spec.priority);
        }
    }
    return ok_count;
}

}  // namespace core
