#pragma once

namespace logger {

bool init();

void log(const char* fmt, ...);

// Replaces one bounded field in the paste-ready DLL Crash Report.txt file.
// The complete report is rewritten after each update and never exceeds the
// configured Steam-comment-safe byte limit.
void report_memory(const char* fmt, ...);
// Pins a fresh allocation-failure memory snapshot in the compact report.
// Periodic monitoring continues in the detailed log without replacing it.
void report_failure_memory(const char* fmt, ...);
void report_allocator(const char* fmt, ...);
void report_hotspot(const char* fmt, ...);
void report_coverage(const char* fmt, ...);
void report_event(const char* fmt, ...);
void report_failure(const char* fmt, ...);
void report_crash(const char* fmt, ...);

void flush_now();

// Exception handlers must acquire this nonblocking guard before using the
// regular logger. Its critical section is recursive on the calling thread.
bool try_begin_crash_report();
void end_crash_report();
// Writes only to a separate startup-opened file; never takes the logger lock.
void write_emergency_crash(unsigned long code, const void* address,
                           unsigned long thread, unsigned long instruction,
                           unsigned long stack);

}
