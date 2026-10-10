#ifndef STORM_ERROR_REPORT_HPP
#define STORM_ERROR_REPORT_HPP

#include <cstdint>

// The error reporter's configuration, read by the crash-report writer (0x140257c60). These
// live in the Storm part of the binary (next to SMemAlloc), but their Storm names are not
// known, so they are named by what they do.

// The reporter's "command" (argv[0], absolute) and "params" (the whole command line)
// fields, 0x8000 characters each, set early in ClientMain.
extern char g_errorReportCommand[0x8000];  // 0x142cb8000
extern char g_errorReportParams[0x8000];   // 0x142cc0000

// The command line SCmd parses: g_errorReportParams.
const char* ErrorReportGetParams();

// Records the last error (DAT_142e1c67c) and passes it to SetLastError. 3.3.5's name: the
// same role.
void SErrSetLastError(uint32_t code);

void ErrorReportSetBuildString(const char* build);

void ErrorReportSetErrorCallback(void (*callback)(uint32_t code, const char* text));

void ErrorReportSetAppendCallback(void (*callback)());

void ErrorReportSetDescribeCallback(void (*callback)());

void ErrorReportSetHandlerCallback(void (*callback)());

void ErrorReportSetExceptionCallback(void (*callback)());

bool ErrorReportSetOption(int32_t option, const void* value, int32_t size);

void ErrorDisplayCode(uint32_t code, const char* text, uint32_t locale, bool flag);

// Formats the message (printf-style) and reports it under `code`.
void ErrorDisplayFormatted(uint32_t code, const char* a2, int32_t a3, const char* format, ...);

// Adds a block of memory for the crash dump to include; false once the 128 slots are used.
bool ErrorReportAddMemoryRange(void* base, uint64_t size);

// What a thread is doing, for the crash report: a stack of records, innermost first, each
// able to describe itself into a buffer. Callers put a record on their own stack, extended
// with whatever its callback reads.
struct ERRORCONTEXT;
typedef void (*ERRORCONTEXTCALLBACK)(ERRORCONTEXT* context, uint64_t size, char* buffer);
struct ERRORCONTEXT {
    ERRORCONTEXT* m_previous;       // 0x00
    ERRORCONTEXTCALLBACK m_describe;  // 0x08
};

ERRORCONTEXT* ErrorContextPush(ERRORCONTEXT* context, ERRORCONTEXTCALLBACK describe);

// Registers what to do when a crash comes from inside a module (e.g. a driver): the handler
// is told the module's name.
void ErrorReportAddModuleCrashHandler(const char* module, void (*handler)(const char* module));
void ErrorContextPop(ERRORCONTEXT* context);

// The objects whose memory a crash report should carry: a thread's stack of records, each
// reporting its ranges through the function it is handed (TLS +0x2120).
struct ERRORMEMORYCONTEXT;
typedef void (*ERRORMEMORYREPORT)(const void* base, uint64_t size);
typedef void (*ERRORMEMORYCALLBACK)(ERRORMEMORYCONTEXT* context, ERRORMEMORYREPORT report);
struct ERRORMEMORYCONTEXT {
    ERRORMEMORYCONTEXT* m_previous;     // 0x00
    ERRORMEMORYCALLBACK m_describe;     // 0x08
};
// ref: FUN_14025cde0
ERRORMEMORYCONTEXT* ErrorMemoryContextPush(ERRORMEMORYCONTEXT* context, ERRORMEMORYCALLBACK describe);
// ref: FUN_14025ce40
void ErrorMemoryContextPop(ERRORMEMORYCONTEXT* context);


#endif
