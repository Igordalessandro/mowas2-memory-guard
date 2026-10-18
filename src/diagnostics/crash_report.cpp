#include "diagnostics/crash_report.h"

#include "shared/logging/logger.h"

#include <windows.h>

namespace crash_report {
namespace {

LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
volatile LONG g_handling_exception = 0;

LONG WINAPI report_unhandled_exception(EXCEPTION_POINTERS* pointers) {
    if (InterlockedCompareExchange(&g_handling_exception, 1, 0) == 0) {
        const EXCEPTION_RECORD* record = pointers ? pointers->ExceptionRecord : nullptr;
        const CONTEXT* context = pointers ? pointers->ContextRecord : nullptr;

        logger::log("============================================================");
        if (record) {
            const unsigned long instruction = context
#if defined(_M_IX86) || defined(__i386__)
                ? static_cast<unsigned long>(context->Eip)
#else
                ? 0ul
#endif
                : 0ul;
            const unsigned long stack = context
#if defined(_M_IX86) || defined(__i386__)
                ? static_cast<unsigned long>(context->Esp)
#else
                ? 0ul
#endif
                : 0ul;
            logger::report_crash(
                "crash=unhandled code=%08lX address=%p thread=%lu ip=%08lX sp=%08lX params=%lu",
                static_cast<unsigned long>(record->ExceptionCode),
                record->ExceptionAddress,
                static_cast<unsigned long>(GetCurrentThreadId()),
                instruction, stack,
                static_cast<unsigned long>(record->NumberParameters));
            logger::log(
                "DLL CRASH REPORT : UNHANDLED EXCEPTION code=0x%08lX address=%p flags=0x%08lX thread=%lu parameters=%lu",
                static_cast<unsigned long>(record->ExceptionCode),
                record->ExceptionAddress,
                static_cast<unsigned long>(record->ExceptionFlags),
                static_cast<unsigned long>(GetCurrentThreadId()),
                static_cast<unsigned long>(record->NumberParameters));
            for (DWORD i = 0; i < record->NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; ++i) {
                logger::log("DLL CRASH REPORT : exception_parameter[%lu]=0x%08lX",
                            static_cast<unsigned long>(i),
                            static_cast<unsigned long>(record->ExceptionInformation[i]));
            }
        } else {
            logger::report_crash("crash=unhandled no_exception_record thread=%lu",
                                 static_cast<unsigned long>(GetCurrentThreadId()));
            logger::log("DLL CRASH REPORT : UNHANDLED EXCEPTION without an exception record thread=%lu",
                        static_cast<unsigned long>(GetCurrentThreadId()));
        }

#if defined(_M_IX86) || defined(__i386__)
        if (context) {
            logger::log(
                "DLL CRASH REPORT : registers EIP=%08lX ESP=%08lX EBP=%08lX EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX",
                static_cast<unsigned long>(context->Eip),
                static_cast<unsigned long>(context->Esp),
                static_cast<unsigned long>(context->Ebp),
                static_cast<unsigned long>(context->Eax),
                static_cast<unsigned long>(context->Ebx),
                static_cast<unsigned long>(context->Ecx),
                static_cast<unsigned long>(context->Edx),
                static_cast<unsigned long>(context->Esi),
                static_cast<unsigned long>(context->Edi));
        }
#else
        (void)context;
#endif

        logger::log("DLL CRASH REPORT : paste DLL Crash Report.txt and describe the last action, active mods, and whether a dialog appeared");
        logger::log("============================================================");
        logger::flush_now();
        InterlockedExchange(&g_handling_exception, 0);
    }

    const LPTOP_LEVEL_EXCEPTION_FILTER previous = g_previous_filter;
    if (previous && previous != &report_unhandled_exception) {
        return previous(pointers);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

bool install() {
    g_previous_filter = SetUnhandledExceptionFilter(&report_unhandled_exception);
    logger::log("DLL CRASH REPORT : unhandled-exception recorder installed; previous_filter=%p",
                reinterpret_cast<void*>(g_previous_filter));
    return true;
}

}  // namespace crash_report
