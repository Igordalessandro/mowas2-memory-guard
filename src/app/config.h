#pragma once

// Target binary: mowas_2.exe v3.262.1 (Steam Oct 2019).
#define MOWAS2_PREFERRED_BASE       0x00400000

#define OFFSET_ENGINE_ALLOC         0x00809041
#define OFFSET_ENGINE_ALLOC_CORE    0x0010E4F0

// Anti-crash clamp for the mp.core.start 2 GB allocation bug.
#define ALLOC_SANITY_LIMIT          (1024 * 1024 * 1024)
#define ALLOC_CLAMP_REPLACEMENT     (4 * 1024 * 1024)

// Logging
#define ENABLE_LOGGING              1
#define LOG_FILE_NAME               L"mowas2-perf.log"
#define LOG_FILE_PER_RUN            0
