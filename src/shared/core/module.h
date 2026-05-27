#pragma once

#include <cstdint>

namespace core {

struct ModuleSpec {
    const char* name;
    int         priority;
    bool      (*install_fn)(std::uintptr_t game_base);
};

void register_module(const ModuleSpec& spec);
int  install_all(std::uintptr_t game_base);

}  // namespace core

#define MOWAS_REGISTER_MODULE(tag, spec_literal)                          \
    namespace {                                                           \
        struct AutoRegister_##tag {                                       \
            AutoRegister_##tag() { ::core::register_module(spec_literal); } \
        };                                                                \
        static AutoRegister_##tag g_autoRegister_##tag;                   \
    }
