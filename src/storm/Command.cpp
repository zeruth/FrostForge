#include "storm/Command.hpp"
#include "os/Os.hpp"
#include "storm/ErrorReport.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "storm/list/TSList.hpp"
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <windows.h>

namespace {

struct CMDDEF {
    TSLink<CMDDEF> m_link;      // 0x00
    uint32_t flags;             // 0x10
    uint32_t id;                // 0x14
    char name[32];              // 0x18
    int32_t namelength;         // 0x38
    uint32_t setvalue;          // 0x3C
    uint32_t setmask;           // 0x40
    void* variableptr;          // 0x48
    uint32_t variablebytes;     // 0x50
    CMDEXTRACALLBACK callback;  // 0x58
    int32_t found;              // 0x60
    union {                     // 0x68
        uint32_t currvalue;
        uint64_t currvalue64;
        char* currvaluestr;
    };
};
static_assert(sizeof(CMDDEF) == 0x70, "CMDDEF is 0x70 bytes in the reference");
static_assert(offsetof(CMDDEF, currvalue64) == 0x68, "CMDDEF layout");

// State carried through one SCmdProcess: the flag whose value is pending, and the name it
// was matched as.
struct PROCESSING {
    CMDDEF* def;
    char name[32];
    int32_t namelength;
};
static_assert(sizeof(PROCESSING) == 0x30, "PROCESSING layout");

typedef TSList<CMDDEF, TSGetLink<CMDDEF>, 8> CMDLIST;

CMDLIST s_params;              // DAT_142cc8a68: positional parameters (CMD_PARAM_MASK)
CMDLIST s_flags;               // DAT_142cc8a80: -flags
int32_t s_hasOptionalParam;    // DAT_142e1d8dc

// The tag SMemAlloc/SMemFree are given for a CMDDEF: the list's RTTI type name.
const char s_cmdDefTag[] = ".?AV?$TSList@UCMDDEF@@V?$TSGetLink@UCMDDEF@@@@$07@@";

const char s_whitespace[] = " ,;\"\t\n\r\x1A";  // DAT_1426206e0
const char s_switchChars[] = "-/";              // DAT_1426206ec
const char s_flagSeparators[] = "=:";           // DAT_1426206dc

// PTR_s_Invalid_argument___s_1426205f8: used when the resource string is missing.
const char* const s_defaultErrors[] = {
    "Invalid argument: %s",
    "The syntax of the command is incorrect.",
    "Unable to open response file: %s",
};

CMDDEF* RawNext(const CMDDEF* def) {
    return def->m_link.m_next;
}

bool IsNode(const CMDDEF* raw) {
    uintptr_t value = reinterpret_cast<uintptr_t>(raw);
    return (value & 1) == 0 && value != 0;
}

int32_t CompareName(const CMDDEF* def, const char* name, int32_t length) {
    return (def->flags & CMD_FLAG_CASE_SENSITIVE)
        ? std::strncmp(def->name, name, length)
        : _strnicmp(def->name, name, length);
}

// After `from`, the definition with the longest name that is a prefix of `name` and longer
// than minLength. Shared by the three places the reference inlines this search.
CMDDEF* FindLongerMatch(CMDDEF* raw, const char* name, int32_t minLength) {
    int32_t length = static_cast<int32_t>(std::strlen(name));
    int32_t best = minLength;
    CMDDEF* match = nullptr;
    do {
        int32_t namelength = raw->namelength;
        if (best < namelength && namelength <= length && CompareName(raw, name, namelength) == 0) {
            best = namelength;
            match = raw;
        }
        raw = RawNext(raw);
    } while (IsNode(raw));
    return match;
}

// SStrCopy's loop as the reference inlines it: at most maxchars - 1 characters, terminated.
void CopyBounded(char* dest, const char* source, uint64_t maxchars) {
    if (maxchars == 0) {
        return;
    }
    for (;;) {
        if (--maxchars == 0) {
            *dest = '\0';
            return;
        }
        char c = *source++;
        *dest++ = c;
        if (c == '\0') {
            return;
        }
    }
}

char* DuplicateString(const char* source, int32_t line) {
    uint64_t length = std::strlen(source);
    char* copy = static_cast<char*>(SMemAlloc(length + 1, 7, __FILE__, SStrHashFnv1a(__FILE__), line, 0));
    std::memcpy(copy, source, length + 1);
    return copy;
}

}

// GetModuleHandleA(nullptr): where SCmd's error strings are loaded from.
// ref: FUN_14025cfd0
HMODULE SCmdGetResourceModule() {
    return GetModuleHandleA(nullptr);
}

// ref: FUN_14025e140
void SCmdRegisterArgument(uint32_t flags, uint32_t id, const char* name, void* variableptr, uint32_t variablebytes,
    uint32_t setvalue, uint32_t setmask, CMDEXTRACALLBACK callback) {
    const char* source = name ? name : "";
    int64_t length = static_cast<int64_t>(std::strlen(source));

    CMDDEF* def = static_cast<CMDDEF*>(SMemAlloc(sizeof(CMDDEF), 8, s_cmdDefTag, 0, -2, SMEM_FLAG_ZEROMEMORY));
    def->m_link.m_prevlink = nullptr;
    def->m_link.m_next = nullptr;
    if ((flags & CMD_PARAM_MASK) == 0) {
        s_flags.LinkToTail(def);
    } else {
        s_params.LinkToTail(def);
    }

    CopyBounded(def->name, source, 0x20);
    def->variablebytes = variablebytes;
    def->flags = flags;
    def->setmask = setmask;
    def->callback = callback;
    def->id = id;
    def->namelength = static_cast<int32_t>(length);
    def->variableptr = variableptr;
    def->setvalue = setvalue;

    if ((flags & (CMD_TYPE_MASK | CMD_FLAG_INVERT_OR_SIGNED)) == CMD_FLAG_INVERT_OR_SIGNED) {
        def->currvalue = setvalue;  // a 32-bit store; the zeroed allocation clears the rest
    } else {
        def->currvalue64 = 0;
    }

    if ((flags & CMD_PARAM_MASK) == CMD_PARAM_OPTIONAL) {
        s_hasOptionalParam = 1;
    }
}

// ref: FUN_14025d0c0
void SCmdDeleteAll(CMDLIST* list) {
    while (CMDDEF* def = list->Head()) {
        if ((def->flags & CMD_TYPE_MASK) == CMD_TYPE_STRING && def->currvaluestr) {
            SMemFree(def->currvaluestr, __FILE__, __LINE__, 0);
            def->currvaluestr = nullptr;
        }
        def->m_link.Unlink();
        SMemFree(def, s_cmdDefTag, -2, 0);
    }
}

// ref: FUN_14025dfd0
bool SCmdUnregisterAll() {
    SCmdDeleteAll(&s_params);
    SCmdDeleteAll(&s_flags);
    s_hasOptionalParam = 0;
    return true;
}

// Builds the message for `code` (string resource 0x5201 + code, else the built-in text),
// substitutes `item` for %s, and hands both to the error callback.
// ref: FUN_14025d1a0
void SCmdReportError(CMDERRORCALLBACK callback, uint32_t code, const char* item) {
    if (code >= 4) {
        return;
    }

    wchar_t wide[256] = {};
    const uint8_t index[4] = { 0, 1, 2, 3 };
    const uint32_t ids[4] = { 0x5201, 0x5202, 0x5203, 0 };
    char text[256];
    text[0] = '\0';

    HMODULE module = SCmdGetResourceModule();
    uint8_t which = index[code];
    LoadStringW(module, ids[which], wide, 0x100);
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, 0x100, nullptr, nullptr);
    if (text[0] == '\0' && which < 3) {
        // Divergence: index 3 has no default text in the reference, whose table slot holds
        // 0 and would be copied from regardless. SCmd never raises code 3, so the guard
        // only replaces a read through a null pointer.
        CopyBounded(text, s_defaultErrors[which], 0x100);
    }

    char message[256];
    if (std::strstr(text, "%s")) {
        SStrPrintf(message, 0x100, text, item);
    } else {
        CopyBounded(message, text, 0x100);
    }

    if (message[0] != '\0') {
        size_t length = strnlen(message, 0x100);
        if (0x100 - length > 1) {
            message[length++] = '\n';
        }
        if (length < 0x100) {
            message[length] = '\0';
        }
    }

    SErrSetLastError(code);
    CMDERROR error = { code, item, message };
    callback(&error);
}

// Applies one argument to a definition: a bool's bits, a number, or a string. Then runs the
// definition's callback and copies the value to every other definition with the same id and
// type, in both lists. *consumed is how much of `argument` the value took.
// ref: FUN_14025d3a0
bool SCmdApplyValue(CMDDEF* def, const char* argument, int32_t* consumed) {
    bool set = false;
    *consumed = 0;
    uint32_t flags = def->flags;
    uint32_t type = flags & CMD_TYPE_MASK;

    if (type != CMD_TYPE_BOOL) {
        if (type == CMD_TYPE_NUMBER) {
            char* end = nullptr;
            if (!(flags & CMD_FLAG_64BIT)) {
                uint32_t value = (flags & CMD_FLAG_INVERT_OR_SIGNED)
                    ? static_cast<uint32_t>(std::strtol(argument, &end, 0))
                    : static_cast<uint32_t>(std::strtoul(argument, &end, 0));
                def->currvalue = value;
                def->variablebytes = 4;
                if (def->variableptr) {
                    *static_cast<uint32_t*>(def->variableptr) = value;
                }
            } else {
                uint64_t value = (flags & CMD_FLAG_INVERT_OR_SIGNED)
                    ? static_cast<uint64_t>(_strtoi64(argument, &end, 0))
                    : std::strtoull(argument, &end, 0);
                def->currvalue64 = value;
                def->variablebytes = 8;
                if (def->variableptr) {
                    *static_cast<uint64_t*>(def->variableptr) = value;
                }
            }
            *consumed = end ? static_cast<int32_t>(end - argument) : static_cast<int32_t>(std::strlen(argument));
        } else if (type == CMD_TYPE_STRING) {
            *consumed = static_cast<int32_t>(std::strlen(argument));
            if (def->currvaluestr) {
                SMemFree(def->currvaluestr, __FILE__, __LINE__, 0);
            }
            def->currvaluestr = DuplicateString(argument, __LINE__);
            if (def->variableptr && def->variablebytes != 0) {
                CopyBounded(static_cast<char*>(def->variableptr), argument, def->variablebytes);
            }
        } else {
            return false;
        }
    } else {
        uint32_t keep;
        if (*argument == '-') {
            *consumed = 1;
            keep = ~def->setmask;
            def->currvalue &= keep;
        } else {
            if (*argument == '+') {
                *consumed = 1;
                set = true;
            } else if (flags & CMD_FLAG_INVERT_OR_SIGNED) {
                keep = ~def->setmask;
                def->currvalue &= keep;
                goto apply;
            } else {
                set = true;
            }
            keep = ~def->setmask;
            def->currvalue = (def->currvalue & keep) | def->setvalue;
        }
    apply:
        if (uint32_t* variable = static_cast<uint32_t*>(def->variableptr)) {
            *variable &= keep;
            if (set) {
                *variable |= def->setvalue;
            }
        }
    }

    def->found = 1;
    if (def->callback) {
        CMDPARAMS params;
        params.flags = def->flags;
        params.id = def->id;
        params.name = def->name;
        params.variableptr = def->variableptr;
        params.setvalue = def->setvalue;
        params.setmask = def->setmask;
        params.currvalue = def->currvalue64;
        if (!def->callback(&params, argument)) {
            return false;
        }
    }

    for (CMDLIST* list : { &s_params, &s_flags }) {
        CMDDEF* raw = list->m_terminator.m_next;
        while (IsNode(raw)) {
            CMDDEF* other = raw;
            raw = RawNext(other);
            uint32_t otherType = other->flags & CMD_TYPE_MASK;
            if (other->id == def->id && otherType == (def->flags & CMD_TYPE_MASK) && other != def) {
                other->found = 1;
                if (otherType == CMD_TYPE_STRING) {
                    if (other->currvaluestr) {
                        SMemFree(other->currvaluestr, __FILE__, __LINE__, 0);
                    }
                    other->currvaluestr = DuplicateString(def->currvaluestr, __LINE__);
                } else {
                    other->currvalue64 = def->currvalue64;
                }
            }
        }
    }

    return true;
}

// Applies a value to the pending flag and to every later flag that matches its name with a
// longer prefix. *consumed is the most any of them took.
// ref: FUN_14025d910
bool SCmdApplyToMatches(const char* argument, PROCESSING* state, int32_t* consumed) {
    *consumed = 0;
    CMDDEF* def = state->def;
    state->def = nullptr;

    while (def) {
        int32_t used;
        if (!SCmdApplyValue(def, argument, &used)) {
            return false;
        }
        if (used < *consumed) {
            used = *consumed;
        }
        *consumed = used;

        CMDDEF* raw = RawNext(def);
        if (!IsNode(raw)) {
            return true;
        }
        def = FindLongerMatch(raw, state->name, state->namelength - 1);
    }

    return true;
}

// Processes the text after a switch character: a run of flag names, matched greedily, each
// optionally followed by '=' or ':' and its value.
// ref: FUN_14025da20
int32_t SCmdProcessFlags(const char* argument, PROCESSING* state, CMDERRORCALLBACK error, int32_t continueOnUnknown) {
    char accum[256];
    accum[0] = '\0';

    while (*argument != '\0') {
        CMDDEF* found = nullptr;
        int64_t remaining = static_cast<int64_t>(std::strlen(argument));
        uint64_t length = std::strlen(accum);
        if (static_cast<int32_t>(length) < 1) {
            length = 1;
        }

        int32_t n;
        uint32_t i;
        do {
            n = static_cast<int32_t>(length);
            i = n - 1;
            length = i;
            if (static_cast<int32_t>(i + static_cast<int32_t>(remaining)) < 0x100) {
                CopyBounded(accum + static_cast<int32_t>(i), argument, 0x100 - i);
                found = nullptr;
                CMDDEF* head = s_flags.m_terminator.m_next;
                if (IsNode(head)) {
                    found = FindLongerMatch(head, accum, -1);
                    if (found) {
                        n = found->namelength;
                        accum[n] = '\0';
                        goto matched;
                    }
                }
            }
        } while (i != 0);

        if (error) {
            SCmdReportError(error, 0, argument);
        }
        return continueOnUnknown;

    matched:
        argument += n;
        while (*argument != '\0' && std::strchr(s_flagSeparators, *argument)) {
            argument++;
        }

        state->def = found;
        state->namelength = n;
        CopyBounded(state->name, accum, 0x20);

        if (*argument == '\0' && (found->flags & CMD_TYPE_MASK) != 0) {
            // the value is the next token
            found->found = 1;
            return 1;
        }

        int32_t used;
        if (!SCmdApplyToMatches(argument, state, &used)) {
            return 0;
        }
        argument += used;
    }

    return 1;
}

// Processes a whole command line (or a response file's contents): tokens are response files
// (@file), -flags, values for a pending flag, positional parameters, or unhandled. Returns
// whether everything up to the committed position was consumed.
// ref: FUN_14025dcc0
bool SCmdProcessString(const char** cmdline, PROCESSING* state, CMDDEF** cursor, CMDUNHANDLEDCALLBACK unhandled,
    CMDERRORCALLBACK error, int32_t continueOnUnknown) {
    const char* p = *cmdline;

    while (*p != '\0') {
        int32_t result = 0;
        int32_t quoted = 0;
        char token[256];
        SStrTokenize(&p, token, 0x100, s_whitespace, &quoted);

        if (token[0] != '\0') {
            if (token[0] == '@' && !quoted) {
                uint8_t* data = nullptr;
                uint32_t size;
                if (!OsFileReadAll(token + 1, &data, &size, 1, 7)) {
                    if (error) {
                        SCmdReportError(error, 2, token + 1);
                    }
                } else {
                    data[size] = 0;
                    const char* contents = reinterpret_cast<const char*>(data);
                    result = SCmdProcessString(&contents, state, cursor, unhandled, error, 0);
                    OsFileFree(data);
                }
            } else if (!std::strchr(s_switchChars, token[0]) || quoted) {
                CMDDEF* pending = state->def;
                if (!pending) {
                    if (*cursor) {
                        int32_t used;
                        if (!SCmdApplyValue(*cursor, token, &used)) {
                            return **cmdline == '\0';
                        }
                        *cursor = (*cursor)->m_link.Next();
                        *cmdline = p;
                        continue;
                    }
                    if (!unhandled) {
                        if (error) {
                            SCmdReportError(error, 0, token);
                        }
                        return **cmdline == '\0';
                    }
                    result = unhandled(token);
                } else {
                    state->def = nullptr;
                    do {
                        int32_t used;
                        if (!SCmdApplyValue(pending, token, &used)) {
                            return **cmdline == '\0';
                        }
                        CMDDEF* raw = RawNext(pending);
                        if (!IsNode(raw)) {
                            break;
                        }
                        pending = FindLongerMatch(raw, state->name, state->namelength - 1);
                    } while (pending);
                    result = 1;
                }
            } else {
                state->def = nullptr;
                result = SCmdProcessFlags(token + 1, state, error, continueOnUnknown);
            }

            if (result == 0) {
                return **cmdline == '\0';
            }
        }

        *cmdline = p;
    }

    return **cmdline == '\0';
}

// ref: FUN_14025e000
uint32_t SCmdProcess(CMDUNHANDLEDCALLBACK unhandled, CMDERRORCALLBACK error, int32_t continueOnUnknown) {
    const char* cmdline = ErrorReportGetParams();
    SStrTokenize(&cmdline, nullptr, 0, s_whitespace, nullptr);  // the program name

    PROCESSING state = {};
    CMDDEF* cursor = s_params.Head();

    if (!SCmdProcessString(&cmdline, &state, &cursor, unhandled, error, continueOnUnknown)) {
        return 0;
    }

    // every required positional parameter must have been consumed
    uint32_t ok = 1;
    for (CMDDEF* node = cursor; IsNode(node) && ok; node = RawNext(node)) {
        if ((node->flags & CMD_PARAM_MASK) == CMD_PARAM_REQUIRED) {
            ok = 0;
        }
    }

    if (error && ok == 0) {
        SCmdReportError(error, 1, "");
    }
    return ok;
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142cc8a68 = (void*)&s_params;
extern "C" void* const gen_p_142cc8a80 = (void*)&s_flags;
// ---- end of generated globals ----
