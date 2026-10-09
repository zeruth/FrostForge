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


#endif
