#pragma once

// Target binary: mowas_2.exe v3.262.1 (Steam Oct 2019).
#define MOWAS2_PREFERRED_BASE       0x00400000

#define MEMORY_GUARD_VERSION        "2.5.0-experimental.9-dev"

#define OFFSET_ENGINE_ALLOC         0x00809041
#define OFFSET_ENGINE_ALLOC_CORE    0x0010E4F0

// Preserve the original project's lobby workaround, but only for the one
// observed corrupt/sentinel request.  The old implementation changed every
// request above 1 GiB, which could silently under-allocate a legitimate block.
#define KNOWN_BAD_LOBBY_ALLOCATION  ((size_t)2147483552u) // 0x7fffffa0
#define LOBBY_ALLOCATION_REPLACEMENT ((size_t)(4u * 1024u * 1024u))

// Four reserved (not committed) virtual-address ranges are released in
// stages. The 48 MiB final block services a known large request; the separate
// 32 MiB recovery block remains protected until an allocation actually fails.
// This preserves a second contiguous chance after a large mod request consumes
// and fragments the first block.
#define PRIMARY_RESERVE_MB            8u
#define CRITICAL_RESERVE_MB           8u
#define FINAL_RESERVE_MB             48u
#define RECOVERY_RESERVE_MB          32u
#define RECOVERY_REARM_MIN_MB         8u
#define RECOVERY_REARM_SLACK_MB       8u
#define LARGE_ALLOCATION_MARGIN_MB    4u
#define RELEASE_TOTAL_FREE_MB       192u
#define RELEASE_LARGEST_FREE_MB      24u
#define CRITICAL_TOTAL_FREE_MB        96u
#define CRITICAL_LARGEST_FREE_MB       8u
// Diagnostic emergency boundary. Crossing it increases monitoring frequency,
// but the final reserve is deliberately held for a size-aware large request
// or an observed allocation failure instead of being consumed by small churn.
#define FINAL_TOTAL_FREE_MB            64u
#define FINAL_LARGEST_FREE_MB           6u
#define REARM_TOTAL_FREE_MB          768u
#define REARM_LARGEST_FREE_MB        256u
#define REARM_STABLE_MS            60000u
#define TELEMETRY_INTERVAL_MS     30000u
#define MONITOR_INTERVAL_MS        5000u
#define CRITICAL_MONITOR_INTERVAL_MS 1000u

// Logging
#define ENABLE_LOGGING              1
#define LOG_FILE_NAME               L"mowas2-memory-guard.log"
#define LOG_FILE_STEM               L"mowas2-memory-guard"
#define LOG_FILE_PER_RUN            1
#define LOG_FILE_RETAINED_RUNS      10u
#define CRASH_REPORT_FILE_NAME      L"DLL Crash Report.txt"
#define CRASH_REPORT_MAX_BYTES      900u
