#pragma once

namespace logger {

bool init();

void log(const char* fmt, ...);

// Replaces one bounded field in the paste-ready DLL Crash Report.txt file.
// The complete report is rewritten after each update and never exceeds the
// configured Steam-comment-safe byte limit.
void report_memory(const char* fmt, ...);
void report_allocator(const char* fmt, ...);
void report_hotspot(const char* fmt, ...);
void report_event(const char* fmt, ...);
void report_crash(const char* fmt, ...);

void flush_now();

}
