#include "diagnostics/crash_report.h"
#include "shared/logging/logger.h"

#include <windows.h>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

bool busy = false;
bool held = false;
bool testing = false;
unsigned attempts = 0;
unsigned emergency_calls = 0;
unsigned ordinary_calls = 0;
unsigned crash_calls = 0;
unsigned flush_calls = 0;
unsigned end_calls = 0;
unsigned previous_calls = 0;
unsigned long recorded_code = 0;
const void* recorded_address = nullptr;
char crash_text[256] = {};

LONG WINAPI previous_filter(EXCEPTION_POINTERS*) {
    assert(!held);
    assert(attempts == 1 && emergency_calls == 1);
    if (!busy) assert(end_calls == 1);
    ++previous_calls;
    return EXCEPTION_EXECUTE_HANDLER;
}

void reset(bool logger_busy) {
    busy = logger_busy;
    held = false;
    testing = true;
    attempts = emergency_calls = ordinary_calls = crash_calls = 0;
    flush_calls = end_calls = previous_calls = 0;
    recorded_code = 0;
    recorded_address = nullptr;
    crash_text[0] = '\0';
}

void busy_logger_never_blocks(LPTOP_LEVEL_EXCEPTION_FILTER filter,
                              EXCEPTION_POINTERS* pointers) {
    reset(true);
    assert(filter(pointers) == EXCEPTION_EXECUTE_HANDLER);
    assert(emergency_calls == 1 && previous_calls == 1);
    assert(recorded_code == EXCEPTION_ACCESS_VIOLATION);
    assert(recorded_address == pointers->ExceptionRecord->ExceptionAddress);
    assert(ordinary_calls == 0 && crash_calls == 0 && flush_calls == 0 && end_calls == 0);
}

void available_logger_releases_before_chaining(LPTOP_LEVEL_EXCEPTION_FILTER filter,
                                               EXCEPTION_POINTERS* pointers) {
    reset(false);
    assert(filter(pointers) == EXCEPTION_EXECUTE_HANDLER);
    assert(emergency_calls == 1 && previous_calls == 1);
    assert(ordinary_calls > 0 && crash_calls == 1 && flush_calls == 1 && end_calls == 1);
    assert(std::strstr(crash_text, "code=C0000005"));
    assert(!held);
}

void missing_exception_record(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
    EXCEPTION_POINTERS pointers = {};
    reset(false);
    assert(filter(&pointers) == EXCEPTION_EXECUTE_HANDLER);
    assert(recorded_code == 0 && recorded_address == nullptr);
    assert(std::strstr(crash_text, "no_exception_record"));
    assert(end_calls == 1 && previous_calls == 1);
    reset(true);
    assert(filter(nullptr) == EXCEPTION_EXECUTE_HANDLER);
    assert(recorded_code == 0 && recorded_address == nullptr);
    assert(ordinary_calls == 0 && crash_calls == 0 && end_calls == 0 && previous_calls == 1);
}

}  // namespace

// Link the real exception filter against a controlled logger. No files, game
// processes, or intentionally raised CPU exceptions are needed for these tests.
namespace logger {

void log(const char*, ...) {
    if (!testing) return;  // crash_report::install emits a setup message.
    assert(held && !busy && emergency_calls == 1);
    ++ordinary_calls;
}

void report_crash(const char* format, ...) {
    assert(held && !busy && emergency_calls == 1);
    ++crash_calls;
    va_list args;
    va_start(args, format);
    std::vsnprintf(crash_text, sizeof(crash_text), format, args);
    va_end(args);
}

void flush_now() {
    assert(held && !busy);
    ++flush_calls;
}

bool try_begin_crash_report() {
    assert(!held && attempts == 0 && emergency_calls == 0);
    ++attempts;
    held = !busy;
    return held;
}

void end_crash_report() {
    assert(held && !busy);
    ++end_calls;
    held = false;
}

void write_emergency_crash(unsigned long code, const void* address,
                           unsigned long thread, unsigned long instruction,
                           unsigned long stack) {
    assert(attempts == 1 && ordinary_calls == 0 && crash_calls == 0);
    assert(thread == GetCurrentThreadId());
    (void)instruction;
    (void)stack;
    ++emergency_calls;
    recorded_code = code;
    recorded_address = address;
}

}  // namespace logger

#include "../src/diagnostics/crash_report.cpp"

int main() {
    const auto original = SetUnhandledExceptionFilter(&previous_filter);
    assert(crash_report::install());
    const auto installed = SetUnhandledExceptionFilter(original);
    assert(installed && installed != &previous_filter);

    EXCEPTION_RECORD record = {};
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.ExceptionAddress = reinterpret_cast<void*>(0x12345678u);
    record.NumberParameters = 2;
    record.ExceptionInformation[0] = 1;
    record.ExceptionInformation[1] = 0x76543210u;
    CONTEXT context = {};
#if defined(_M_IX86) || defined(__i386__)
    context.Eip = 0x12345678u;
    context.Esp = 0x11223344u;
#endif
    EXCEPTION_POINTERS pointers = {&record, &context};
    busy_logger_never_blocks(installed, &pointers);
    available_logger_releases_before_chaining(installed, &pointers);
    missing_exception_record(installed);
    std::puts("crash report tests passed");
}
