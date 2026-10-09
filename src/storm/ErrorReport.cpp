#include "storm/ErrorReport.hpp"
#include "util/Unimplemented.hpp"
#include <windows.h>

char g_errorReportCommand[0x8000] = "no command - main not yet called";
char g_errorReportParams[0x8000] = "unknown params - main not yet called";

namespace {

uint32_t s_lastError;  // DAT_142e1c67c

}

// ref: FUN_140211570
const char* ErrorReportGetParams() {
    return g_errorReportParams;
}

// ref: FUN_14025a0e0
void SErrSetLastError(uint32_t code) {
    s_lastError = code;
    SetLastError(code);
}

// Stores the callback (DAT_142e1c948) that 0x14025a0c0 forwards errors to.
// ref: FUN_140259780
void ErrorReportSetErrorCallback(void (*callback)(uint32_t code, const char* text)) {
    (void)callback;
    FROST_UNIMPLEMENTED();
}

// Copies the build string (at most 0x7f characters) into the report's header under the
// reporter's lock (DAT_142e1c638).
// ref: FUN_1402597c0
void ErrorReportSetBuildString(const char* build) {
    (void)build;
    FROST_UNIMPLEMENTED();
}

// Called with the report writer to append sections after <ErrorDescription>.
// ref: FUN_140259790
void ErrorReportSetAppendCallback(void (*callback)()) {
    (void)callback;
    FROST_UNIMPLEMENTED();
}

// Asked for a 64-character description of the error before the code's own text is used.
// ref: FUN_140259760
void ErrorReportSetDescribeCallback(void (*callback)()) {
    (void)callback;
    FROST_UNIMPLEMENTED();
}

// Given the error code, file and line; its result decides what the reporter does next.
// ref: FUN_1402595f0
void ErrorReportSetHandlerCallback(void (*callback)()) {
    (void)callback;
    FROST_UNIMPLEMENTED();
}

// Read by the unhandled-exception path.
// ref: FUN_1402598d0
void ErrorReportSetExceptionCallback(void (*callback)()) {
    (void)callback;
    FROST_UNIMPLEMENTED();
}

// Copies `size` bytes into option slot `option` (10 slots, DAT_1426204f0), only when `size`
// matches the slot's size. Returns whether it did.
// ref: FUN_14025b6f0
bool ErrorReportSetOption(int32_t option, const void* value, int32_t size) {
    (void)option;
    (void)value;
    (void)size;
    FROST_UNIMPLEMENTED();
    return false;
}

// Formats the message for `code` (from the error resources) with `arg` and displays it.
// ref: FUN_140219af0
void ErrorDisplayCode(uint32_t code, const char* text, uint32_t arg, bool flag) {
    (void)code;
    (void)text;
    (void)arg;
    (void)flag;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14024d330
void ErrorDisplayFormatted(uint32_t code, const char* a2, int32_t a3, const char* format, ...) {
    (void)code;
    (void)a2;
    (void)a3;
    (void)format;
    FROST_UNIMPLEMENTED();
}
