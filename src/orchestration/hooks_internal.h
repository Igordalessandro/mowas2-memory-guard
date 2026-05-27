#pragma once

#include "app/config.h"

#include <windows.h>
#include <cstdint>
#include <cstddef>

extern uintptr_t g_gameBase;

typedef void* (__cdecl *PFN_EngineAlloc)(size_t size);

extern PFN_EngineAlloc g_origEngineAlloc;
extern PFN_EngineAlloc g_origEngineAllocCore;

void* hooked_engineAlloc(size_t size);
void* hooked_engineAllocCore(size_t size);
