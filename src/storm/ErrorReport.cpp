#include "storm/ErrorReport.hpp"
#include "blz/String.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Unimplemented.hpp"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

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
// Diverged: the error dialog and report are not ported; the message goes to stderr so that it
// is not lost.
void ErrorDisplayFormatted(uint32_t code, const char* a2, int32_t a3, const char* format, ...) {
    (void)a2;
    (void)a3;
    char message[0x800];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    fprintf(stderr, "error 0x%08X: %s\n", code, message);
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14024d210
// Diverged: the report is not ported; the message goes to stderr so that it is not lost.
// (Its format was the fourth parameter until 2026-10-20, so the reference's 1 was read as the
// format: Lua's script-exception report faulted in vsprintf, touching address 1.)
void ErrorReportNonFatal(uint32_t code, const char* file, int32_t line, uint8_t a4, int32_t a5, uint64_t a6,
    const char* format, ...) {
    (void)a4;
    (void)a5;
    (void)a6;
    char message[0x800];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    fprintf(stderr, "error 0x%08X (%s:%d): %s\n", code, file, line, message);
    FROST_UNIMPLEMENTED();
}

namespace {

// The memory the crash dump is to include besides the stacks (DAT_142ddc620, count at
// DAT_142ddc614).
struct MEMORYRANGE {
    void* m_base;
    uint64_t m_size;
};
MEMORYRANGE s_memoryRanges[0x80];
uint32_t s_memoryRangeCount;

// The innermost context of this thread (TLS +0x2128).
thread_local ERRORCONTEXT* t_errorContext;
// The innermost memory context of this thread (TLS +0x2120).
thread_local ERRORMEMORYCONTEXT* t_errorMemoryContext;

}

// ref: FUN_140212e70
bool ErrorReportAddMemoryRange(void* base, uint64_t size) {
    uint32_t index = s_memoryRangeCount;
    if (index > 0x7f) {
        return false;
    }
    s_memoryRangeCount = index + 1;
    s_memoryRanges[index].m_base = base;
    s_memoryRanges[index].m_size = size;
    return true;
}

// ref: FUN_14025ce10
ERRORCONTEXT* ErrorContextPush(ERRORCONTEXT* context, ERRORCONTEXTCALLBACK describe) {
    context->m_describe = describe;
    context->m_previous = t_errorContext;
    t_errorContext = context;
    return context;
}

// ref: FUN_14025ce70
void ErrorContextPop(ERRORCONTEXT* context) {
    t_errorContext = context->m_previous;
}

// ref: FUN_14025cde0
ERRORMEMORYCONTEXT* ErrorMemoryContextPush(ERRORMEMORYCONTEXT* context, ERRORMEMORYCALLBACK describe) {
    context->m_describe = describe;
    context->m_previous = t_errorMemoryContext;
    t_errorMemoryContext = context;
    return context;
}

// ref: FUN_14025ce40
void ErrorMemoryContextPop(ERRORMEMORYCONTEXT* context) {
    t_errorMemoryContext = context->m_previous;
}

namespace {

const char s_crashActionTag[] = ".?AUCrashFromModuleAction@@";

// What to do when a crash comes from a module (CrashFromModuleAction, 0x60 bytes): the
// module's name, and the handler told its name.
struct CrashFromModuleAction {
    blz::inline_string<0x40> m_module;  // 0x00
    void (*m_handler)(const char* module);  // 0x58
};
static_assert(sizeof(CrashFromModuleAction) == 0x60, "CrashFromModuleAction is 0x60 bytes");

struct CRASHACTIONLIST {
    CrashFromModuleAction* m_data;
    uint64_t m_size;
    uint64_t m_capacity;            // bit 63: the storage is not owned
};
CRASHACTIONLIST s_crashActions;     // DAT_142e1c510
SRWLOCK s_crashActionLock = SRWLOCK_INIT;  // DAT_142e1c998

void SetName(blz::inline_string<0x40>* string, const char* text, uint64_t length) {
    blz::string_reserve(string->header(), length);
    std::memcpy(string->m_data, text, length);
    string->m_data[length] = '\0';
    string->m_size = length;
}

// ref: FUN_140259eb0
void CrashActionReserve(CRASHACTIONLIST* list, uint64_t count) {
    uint64_t capacity = list->m_capacity & ~blz::string::INLINE;
    if (capacity >= count) {
        return;
    }
    uint64_t grown = ((list->m_capacity >> 1) & 0x3FFFFFFFFFFFFFFFull) + capacity;
    if (grown < count) {
        grown = count;
    }
    list->m_capacity = ((list->m_capacity ^ grown) & blz::string::INLINE) ^ grown;
    auto data = static_cast<CrashFromModuleAction*>(SMemAlloc(grown * sizeof(CrashFromModuleAction), 0xb,
        s_crashActionTag, SStrHashFnv1a(s_crashActionTag), -6, 0));
    for (uint64_t i = 0; i < list->m_size; i++) {
        new (&data[i].m_module) blz::inline_string<0x40>();
        SetName(&data[i].m_module, list->m_data[i].m_module.m_data, list->m_data[i].m_module.m_size);  // FUN_1402560d0
        data[i].m_handler = list->m_data[i].m_handler;
        blz::string_free(list->m_data[i].m_module.header());
    }
    if ((list->m_capacity & blz::string::INLINE) == 0 && list->m_data != nullptr) {
        SMemFree(list->m_data, s_crashActionTag, -6, 0);
    }
    list->m_capacity &= ~blz::string::INLINE;
    list->m_data = data;
}

}

// ref: FUN_1402594e0
void ErrorReportAddModuleCrashHandler(const char* module, void (*handler)(const char* module)) {
    AcquireSRWLockExclusive(&s_crashActionLock);
    uint64_t length = *module != '\0' ? std::strlen(module) : 0;
    if (s_crashActions.m_size == (s_crashActions.m_capacity & ~blz::string::INLINE)) {
        CrashActionReserve(&s_crashActions, (s_crashActions.m_capacity & ~blz::string::INLINE) + 1);
    }
    CrashFromModuleAction* action = &s_crashActions.m_data[s_crashActions.m_size];
    new (&action->m_module) blz::inline_string<0x40>();
    SetName(&action->m_module, module, length);  // FUN_140259d60
    action->m_handler = handler;
    s_crashActions.m_size++;
    ReleaseSRWLockExclusive(&s_crashActionLock);
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1402560d0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_140259d60=GenForwardMissing")
// ---- end of link fallbacks ----

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142e1c510 = (void*)&s_crashActions;
extern "C" void* const gen_p_142e1c67c = (void*)&s_lastError;
extern "C" void* const gen_p_142e1c998 = (void*)&s_crashActionLock;
// ---- end of generated globals ----
