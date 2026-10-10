#include "console/Command.hpp"
#include "gx/Gx.hpp"
#include "blz/Function.hpp"
#include "blz/String.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "storm/Unicode.hpp"
#include "storm/hash/TSHashTable.hpp"
#include "util/FunctionPointer.hpp"
#include "util/Unimplemented.hpp"
#include <cstdarg>
#include <cstddef>
#include <cstring>
#include <windows.h>

namespace {

struct CONSOLECOMMAND : TSHashObject<CONSOLECOMMAND>, HASHKEY_CONSTSTRI {
    blz::function_storage m_handler;  // 0x38
    const char* m_help;               // 0x58
    uint32_t m_category;              // 0x60
    uint64_t m_a5;                    // 0x68
};
static_assert(offsetof(CONSOLECOMMAND, m_str) == 0x30, "CONSOLECOMMAND layout");
static_assert(offsetof(CONSOLECOMMAND, m_handler) == 0x38, "CONSOLECOMMAND layout");
static_assert(offsetof(CONSOLECOMMAND, m_a5) == 0x68, "CONSOLECOMMAND layout");
static_assert(sizeof(CONSOLECOMMAND) == 0x70, "CONSOLECOMMAND is 0x70 bytes");

const char s_nodeTag[] = ".?AV?$TSList@UCONSOLECOMMAND@@V?$TSGetExplicitLink@UCONSOLECOMMAND@@@@$07@@";
const char s_tableTag[] = ".?AV?$TSHashTable@UCONSOLECOMMAND@@VHASHKEY_CONSTSTRI@@$07@@";

class CONSOLECOMMANDTABLE : public TSHashTable<CONSOLECOMMAND> {
    public:
    // ref: FUN_140323350
    void Delete(CONSOLECOMMAND* node) override {
        node->m_handler.m_manager(node->m_handler.m_storage, nullptr, 0);
        // FUN_140323620: out of the table, then both links
        if (node->m_table) {
            static_cast<CONSOLECOMMANDTABLE*>(node->m_table)->Unlink(node);
        }
        node->m_linktofull.Unlink();
        node->m_linktoslot.Unlink();
        SMemFree(node, s_tableTag, -2, 0);
    }

    // ref: FUN_140323390
    CONSOLECOMMAND* NewNode(TSExplicitList<CONSOLECOMMAND>* slot, uint32_t extra, uint32_t flags) override {
        auto node = static_cast<CONSOLECOMMAND*>(
            SMemAlloc(uint64_t(extra) + sizeof(CONSOLECOMMAND), 8, s_nodeTag, 0, -2, flags | SMEM_FLAG_ZEROMEMORY));
        node->m_linktoslot.m_prevlink = nullptr;
        node->m_linktoslot.m_next = nullptr;
        node->m_linktofull.m_prevlink = nullptr;
        node->m_linktofull.m_next = nullptr;
        node->m_table = nullptr;
        node->m_hashval = 0;
        node->m_str = nullptr;
        node->m_handler.m_invoke = nullptr;
        node->m_handler.m_manager = &blz::function_empty_manager;
        node->m_help = nullptr;
        slot->LinkToHead(node);
        return node;
    }
};

CONSOLECOMMANDTABLE s_commands;  // 0x142ccbd90

// DAT_14348b650: console output only runs once this is set. Its owner is not ported yet.

// DAT_142e30180 / DAT_142e30188 / DAT_142e30198 / DAT_142e301c8: the output callbacks, their
// lock, and whether output is enabled.
typedef void (*CONSOLEOUTPUT)(const char* text, uint32_t color);
CONSOLEOUTPUT* s_outputs;
uint64_t s_outputCount;
SRWLOCK s_outputLock = SRWLOCK_INIT;
uint8_t s_outputEnabled;

// DAT_142e30108 / DAT_142e30110: told about every "unknown command" style message.
void (*s_formatHook)(void* context, const char* format, va_list args);
void* s_formatHookContext;

// DAT_142e260f0 / DAT_142e2e0f0 / DAT_142e30160: the last 32 executed lines, the next slot,
// and a hook told about each one.
char s_history[32][0x400];
uint32_t s_historyNext;
void (*s_historyHook)(const char* line);

// DAT_142ccbec0 / DAT_142e2e100: recording typed lines into a buffer (for writing them to a
// file). 0 waits for a y/n overwrite answer, 1 and 2 record until "end", 3 has just ended,
// 4 is idle (the static value).
int32_t s_recordState = 4;
char s_recordBuffer[0x2000];

// DAT_142e30120: the command name ConsoleCommandParse split off.
char s_commandName[0x40];

// The invoker and manager for a handler held as a plain function pointer (10 bytes of
// storage: the pointer and a 16-bit flag).
// ref: FUN_140325780
bool InvokeHandler(const char* command, const char* arguments, void* storage) {
    return (*static_cast<CONSOLECOMMANDHANDLER*>(storage))(command, arguments) != 0;
}

// ref: FUN_1403257a0
void ManageHandler(void* dest, void* src, int32_t op) {
    if (op == 1 || op == 2) {
        if (dest) {
            std::memcpy(dest, src, 8);
            std::memcpy(static_cast<uint8_t*>(dest) + 8, static_cast<uint8_t*>(src) + 8, 2);
        }
    } else if (op == 3) {
        *static_cast<void**>(src) = dest;
    }
}

}

// Console text goes out only once Gx has a device.
bool ConsoleOutputReady() {
    return GxDevicePresent();
}

// ref: FUN_140326be0
void ConsoleWrite(uint32_t color, const char* text) {
    if (text && *text && ConsoleOutputReady() && s_outputEnabled) {
        AcquireSRWLockExclusive(&s_outputLock);
        for (CONSOLEOUTPUT* output = s_outputs; output != s_outputs + s_outputCount; output++) {
            (*output)(text, color);
        }
        ReleaseSRWLockExclusive(&s_outputLock);
    }
}

// ConsoleWrite with color 0.
// ref: FUN_140326b60
void ConsoleWriteText(const char* text) {
    if (text && *text && ConsoleOutputReady() && s_outputEnabled) {
        AcquireSRWLockExclusive(&s_outputLock);
        for (CONSOLEOUTPUT* output = s_outputs; output != s_outputs + s_outputCount; output++) {
            (*output)(text, 0);
        }
        ReleaseSRWLockExclusive(&s_outputLock);
    }
}

// Formats into 0x800 characters through Storm's printf engine (0x1402610c0), then writes.
// ref: FUN_1403232c0
void ConsoleWriteFormat(uint32_t color, const char* format, ...) {
    if (!format || *format == '\0') {
        return;
    }
    char text[0x800];
    va_list args;
    va_start(args, format);
    SStrVPrintf(text, sizeof(text), format, args);
    va_end(args);
    ConsoleWrite(color, text);
}

// ref: FUN_1403238f0
bool ConsoleCommandExists(const char* command) {
    if (!command) {
        return true;
    }
    return s_commands.Ptr(command) != nullptr;
}

// ref: FUN_1403240c0
bool ConsoleCommandRegister(const char* command, CONSOLECOMMANDHANDLER handler, uint32_t category, const char* help,
    uint64_t a5) {
    blz::function_storage function;
    if (!handler) {
        function.m_invoke = nullptr;
        function.m_manager = &blz::function_empty_manager;
    } else {
        function.m_invoke = reinterpret_cast<void*>(&InvokeHandler);
        function.m_manager = &ManageHandler;
        std::memset(function.m_storage, 0, sizeof(function.m_storage));
        std::memcpy(function.m_storage, &handler, sizeof(handler));
    }

    // the reference moves the argument into a temporary before using it
    blz::function_storage temp;
    function.m_manager(temp.m_storage, function.m_storage, 2);
    temp.m_invoke = function.m_invoke;
    temp.m_manager = function.m_manager;
    function.m_invoke = nullptr;
    function.m_manager = &blz::function_empty_manager;

    bool registered = false;
    if (std::strlen(command) < 0x40) {
        if (!s_commands.Ptr(command)) {
            uint32_t hash = SStrHashHT(command);
            CONSOLECOMMAND* node = s_commands.New(hash, 0, 0);
            node->m_str = command;
            node->m_hashval = hash;
            node->m_handler.m_manager(node->m_handler.m_storage, nullptr, 0);
            temp.m_manager(node->m_handler.m_storage, temp.m_storage, 1);
            node->m_handler.m_invoke = temp.m_invoke;
            node->m_handler.m_manager = temp.m_manager;
            node->m_a5 = a5;
            node->m_help = help;
            node->m_category = category;
            registered = true;
        } else {
            ConsoleWriteFormat(0, "Attempted to register existing command: %s\n", command);
        }
    }
    temp.m_manager(temp.m_storage, nullptr, 0);
    return registered;
}

// A line from the history: 0 is the newest.
// ref: FUN_140323920
const char* ConsoleGetHistory(int32_t age) {
    return s_history[(s_historyNext - age - 1) & 0x1F];
}

// ConsoleWriteFormat that also tells the format hook.
// ref: FUN_140323980
void ConsoleWriteFormatNotify(uint32_t color, const char* format, ...) {
    va_list args;
    va_start(args, format);
    // FUN_140323310
    if (format && *format != '\0') {
        char text[0x800];
        va_list copy;
        va_copy(copy, args);
        SStrVPrintf(text, sizeof(text), format, copy);
        va_end(copy);
        ConsoleWrite(color, text);
    }
    if (s_formatHook) {
        s_formatHook(s_formatHookContext, format, args);
    }
    va_end(args);
}

namespace {

// Splits a line into its command name (up to 63 bytes of whole UTF-8 characters, into
// s_commandName) and its arguments (leading spaces skipped, trailing ones trimmed), and
// finds the command.
// ref: FUN_140325050
CONSOLECOMMAND* ConsoleCommandParse(const char* line, const char** command, char* arguments, uint32_t argumentsSize) {
    char* out = s_commandName;
    int32_t left = 0x3F;
    do {
        int32_t bytes;
        uint32_t ch = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(line), &bytes);
        if (ch == 0xFFFFFFFF || ch == ' ' || left < bytes) {
            break;
        }
        if (bytes > 0) {
            left -= bytes;
            for (; bytes > 0; bytes--) {
                *out++ = *line++;
            }
        }
    } while (left > 0);
    *out = '\0';

    if (command) {
        *command = s_commandName;
    }

    if (arguments) {
        int32_t bytes;
        uint32_t ch = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(line), &bytes);
        while (ch != 0xFFFFFFFF && ch == ' ') {
            line += bytes;
            ch = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(line), &bytes);
        }
        if (argumentsSize != 0) {
            char* dest = arguments;
            for (uint64_t room = argumentsSize;;) {
                if (--room == 0) {
                    *dest = '\0';
                    break;
                }
                char c = *line++;
                *dest++ = c;
                if (c == '\0') {
                    break;
                }
            }
        }
        uint64_t length = std::strlen(arguments);
        if (length != 0) {
            char* end = arguments + length;
            do {
                end--;
                if (*end != ' ') {
                    break;
                }
                *end = '\0';
            } while (end != arguments);
        }
    }

    return s_commands.Ptr(s_commandName);
}

// Runs a command's handler, after checking that its target is code in this executable.
bool CallHandler(CONSOLECOMMAND* node, const char* command, const char* arguments) {
    uint64_t* target = nullptr;
    if (node->m_handler.m_invoke) {
        node->m_handler.m_manager(node->m_handler.m_storage, &target, 3);
    }
    CheckFunctionPointer(*target);
    typedef bool (*INVOKE)(const char* command, const char* arguments, void* storage);
    return reinterpret_cast<INVOKE>(node->m_handler.m_invoke)(command, arguments, node->m_handler.m_storage);
}

}

// Executes one or more lines of console input. Each line: leading spaces dropped, cut to
// 1023 bytes; outside "SET" lines a "--" comment is echoed and cut off (a whole-line comment
// or a "--[[" block comment stops everything); optionally recorded in the history; then run
// as a command, or failing that through the "run" and "rundb" commands. While a recording is
// going on, lines are collected instead. Returns whether a handler accepted a line.
// ref: FUN_1403257d0
bool ConsoleCommandExecute(const char* text, int32_t addToHistory) {
    bool executed = false;

    if (s_recordState == 0) {
        if (*text == 'n') {
            ConsoleWrite(2, "Canceled File Creation");
            s_recordState = 4;
            return false;
        }
        if (*text == 'y') {
            ConsoleWrite(2, "Begin Typing the commands");
            s_recordState = 1;
            return false;
        }
        ConsoleWrite(3, "You must type 'y' to confirm overwrite. Process aborted!");
        s_recordState = 4;
        return false;
    }
    if (s_recordState == 1 || s_recordState == 2) {
        if (_stricmp(text, "end") != 0) {
            char line[0x400];
            SStrPrintf(line, 0x104, "%s\n", text);
            uint64_t used = std::strlen(s_recordBuffer);
            uint64_t length = std::strlen(line);
            if (0x1FFF - used != length) {
                char* end = s_recordBuffer;
                uint64_t room = 0x2000;
                while (room != 0 && *end != '\0') {
                    end++;
                    room--;
                }
                room = static_cast<uint64_t>(s_recordBuffer + sizeof(s_recordBuffer) - end);
                if (room != 0) {
                    for (const char* in = line; *in != '\0'; in++) {
                        if (room != 1) {
                            *end++ = *in;
                            room--;
                        }
                    }
                    *end = '\0';
                }
            }
            return false;
        }
        if (s_recordState != 2) {
            s_recordState = 3;
        }
    }

    blz::string buffer;
    buffer.assign(text);

    const char* cur = buffer.m_data;
    while (*cur != '\0') {
        const char* lineEnd = cur;
        for (char c = *lineEnd; c != '\0' && c != '\n' && c != '\r'; c = *++lineEnd) {
        }
        while (*cur == ' ') {
            cur++;
        }

        char line[0x400];
        uint32_t length = static_cast<uint32_t>(static_cast<int32_t>(lineEnd - cur));
        if (length >= 0x3FF) {
            length = 0x3FF;
        }
        std::memcpy(line, cur, length);
        line[length] = '\0';

        if (_strnicmp(line, "SET ", 4) != 0) {
            if (std::strstr(line, "--[[")) {
                ConsoleWriteFormat(4, "WARNING: Block comment found!\nBlock comments are not supported.");
                return false;
            }
            if (char* comment = std::strstr(line, "--")) {
                const char* start = line;
                while (*start == ' ' || *start == '\t') {
                    start++;
                }
                if (_stricmp(comment, start) == 0) {
                    ConsoleWriteFormat(0xB, "%s", comment);
                    return false;
                }
                ConsoleWriteFormat(0xB, "%s", comment);
                *comment = '\0';
            }
        }

        if (addToHistory) {
            const char* last = ConsoleGetHistory(0);
            if (!last || _stricmp(line, last) != 0) {
                char* slot = s_history[s_historyNext];
                const char* in = line;
                for (int64_t room = 0x3FF;;) {
                    char c = *in++;
                    *slot++ = c;
                    if (c == '\0') {
                        break;
                    }
                    if (--room == 0) {
                        *slot = '\0';
                        break;
                    }
                }
                s_historyNext = (s_historyNext + 1) & 0x1F;
                if (s_historyHook) {
                    s_historyHook(line);
                }
            }
        }

        char arguments[0x400];
        std::memset(arguments, 0, sizeof(arguments));
        const char* command = nullptr;
        CONSOLECOMMAND* node = ConsoleCommandParse(line, &command, arguments, sizeof(arguments));
        if (node) {
            if (CallHandler(node, command, arguments)) {
                executed = true;
            }
        } else {
            command = "";
            std::memcpy(arguments, line, std::strlen(line) + 1);
            CONSOLECOMMAND* run = s_commands.Ptr("run");
            if (run && CallHandler(run, "", arguments)) {
                executed = true;
            } else {
                CONSOLECOMMAND* rundb = s_commands.Ptr("rundb");
                if (rundb && CallHandler(rundb, command, arguments)) {
                    executed = true;
                } else {
                    ConsoleWriteFormatNotify(3, "Unknown console command (%s)", buffer.m_data);
                    return executed;
                }
            }
        }

        cur = lineEnd;
        while (*cur == '\n' || *cur == '\r') {
            cur++;
        }
    }
    return executed;
}

namespace {

// "help [command]": lists the commands, or describes one.
// ref: FUN_140324310
int32_t HelpCommand(const char* command, const char* arguments) {
    (void)command;
    (void)arguments;
    FROST_UNIMPLEMENTED();
    return 1;
}

// SStrPrintf into a char[128]; another copy of the same template body as 0x1402544f0.
// ref: FUN_140211780
void FormatBuffer128(char* dest, const char* format, ...) {
    va_list args;
    va_start(args, format);
    SStrVPrintf(dest, 0x80, format, args);
    va_end(args);
}

// "ver": the build and the compiler it was built with.
// ref: FUN_142434840
int32_t VersionCommand(const char* command, const char* arguments) {
    (void)command;
    (void)arguments;
    char compiler[0x88];
    FormatBuffer128(compiler, "MSVC %d.%02d.%d.%d", 0x13, 0x26, 0x816D, 0);
    ConsoleWriteFormat(0, "WoW [Release x64] [Build 54261 (3.4.3) (Apr 12 2024 14:26:02)] [%s]", compiler);
    return 1;
}

// "quit": asks the client to quit.
// ref: FUN_140326df0
int32_t QuitCommand(const char* command, const char* arguments) {
    (void)command;
    (void)arguments;
    ClientRequestQuit();
    return 1;
}

}

// ref: FUN_140323940
void ConsoleRegisterHelpCommand() {
    ConsoleCommandRegister("help", &HelpCommand, 2, "Provides help information about a command.", 0);
}

// ref: FUN_1424348a0
void ConsoleRegisterVersionCommand() {
    ConsoleCommandRegister("ver", &VersionCommand, 5, nullptr, 0);
}

// ref: FUN_140326e20
void ConsoleRegisterBasicCommands() {
    ConsoleRegisterHelpCommand();
    ConsoleRegisterVersionCommand();
    // 0x1402112e0, an empty function, is called here
    ConsoleCommandRegister("quit", &QuitCommand, 5, nullptr, 0);
}

// ref: FUN_1403242a0
void ConsoleCommandUnregister(const char* command) {
    if (command) {
        if (CONSOLECOMMAND* node = s_commands.Ptr(command)) {
            s_commands.Unlink(node);
            s_commands.Delete(node);
        }
    }
}

namespace {

int32_t s_consoleHotKey;  // DAT_142ccbed0

}

// ref: FUN_140326b30
void ConsoleSetHotKey(int32_t key) {
    s_consoleHotKey = key;
}

int32_t ConsoleGetHotKey() {
    return s_consoleHotKey;
}
