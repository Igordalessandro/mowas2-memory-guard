#pragma once

// Target binary: mowas_2.exe v3.262.1 (Steam Oct 2019).
#define MOWAS2_PREFERRED_BASE       0x00400000

#define OFFSET_ENGINE_ALLOC         0x00809041
#define OFFSET_ENGINE_ALLOC_CORE    0x0010E4F0

// Preserve the original project's lobby workaround, but only for the one
// observed corrupt/sentinel request.  The old implementation changed every
// request above 1 GiB, which could silently under-allocate a legitimate block.
#define KNOWN_BAD_LOBBY_ALLOCATION  ((size_t)2147483552u) // 0x7fffffa0
#define LOBBY_ALLOCATION_REPLACEMENT ((size_t)(4u * 1024u * 1024u))

// Two reserved (not committed) virtual-address ranges are released in stages.
// The primary block provides ordinary low-memory headroom.  The final block
// stays unavailable until an allocator actually fails, avoiding the old
// behavior where the whole reserve disappeared long before the final crash.
#define PRIMARY_RESERVE_MB           48u
#define FINAL_RESERVE_MB             16u
#define RELEASE_TOTAL_FREE_MB       192u
#define RELEASE_LARGEST_FREE_MB      24u
#define TELEMETRY_INTERVAL_MS     30000u
#define MONITOR_INTERVAL_MS        5000u

// Logging
#define ENABLE_LOGGING              1
#define LOG_FILE_NAME               L"mowas2-memory-guard.log"
#define LOG_FILE_PER_RUN            0
