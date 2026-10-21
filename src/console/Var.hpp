#ifndef CONSOLE_VAR_HPP
#define CONSOLE_VAR_HPP

#include "base/RCString.hpp"
#include "blz/Function.hpp"
#include "blz/String.hpp"
#include <cstdint>

// CVar flags (what each bit does, as the code uses it).
#define CVAR_FLAG_1           0x1         // set on every CVar not registered with 0x400
#define CVAR_FLAG_LATCHED     0x2         // a new value waits in m_latchedValue
#define CVAR_FLAG_LOCKED      0x4         // value updates are ignored
#define CVAR_FLAG_READONLY    0x8         // the console refuses to set it
#define CVAR_FLAG_HIDDEN      0x40        // the console command does nothing
#define CVAR_FLAG_80          0x80
#define CVAR_FLAG_SECURE      0x100       // the console refuses to set it
#define CVAR_FLAG_DEFERCALL   0x200       // the callback runs after the value is stored
#define CVAR_FLAG_400         0x400
#define CVAR_FLAG_SAVE        0x800       // registering marks the CVars dirty
#define CVAR_FLAG_NOCALLLOAD  0x1000      // no callback while registering or loading
#define CVAR_FLAG_TEST        0x2000      // a change from the default is reported
#define CVAR_FLAG_PASSWORD    0x4000      // the console does not print the value
#define CVAR_FLAG_NOTDEFAULT  0x80000000  // registered with isReset

struct CVar;

// A C callback, wrapped into the CVar's blz::function by CVarRegister: return false to reject
// the new value.
typedef bool (*CVARCALLBACK)(CVar* cvar, const char* oldValue, const char* newValue, void* userData);

// Engine/Source/ConsoleBase/ConsoleVar.cpp
struct CVar {
    uint32_t m_category;               // 0x00
    uint32_t m_flags;                  // 0x04
    RCString m_stringValue;            // 0x08
    float m_floatValue;                // 0x10
    int32_t m_intValue;                // 0x14
    int64_t m_int64Value;              // 0x18
    int32_t m_modified;                // 0x20
    RCString m_defaultValue;           // 0x28
    RCString m_resetValue;             // 0x30
    RCString m_latchedValue;           // 0x38
    RCString m_help;                   // 0x40
    uint64_t m_nameHash;               // 0x48
    blz::string m_name;                // 0x50
    blz::function_storage m_callback;  // 0x78

    // unique_ptr<CVar, CVar::Deleter>
    struct Deleter {
        void operator()(CVar* cvar) const;
    };
};

// Registers a CVar, or updates the registration of one that exists. Returns null when the
// name or default is missing or a console command has the name. A new CVar's value is also
// stored as its reset value when isReset is set, and as its coded default otherwise; isReset
// clear also sets CVAR_FLAG_NOTDEFAULT. a8 sets CVAR_FLAG_80 when that flag is clear.
CVar* CVarRegister(const char* name, const char* help, uint32_t flags, const char* defaultValue,
    blz::function_storage* callback, uint32_t category, bool isReset, bool a8);

// The same with a plain C callback and its user data.
CVar* CVarRegister(const char* name, const char* help, uint32_t flags, const char* defaultValue, CVARCALLBACK callback,
    uint32_t category, bool isReset, void* userData, bool a9);

// Sets a CVar's value (and/or its reset and default values), running its callback.
bool CVarSet(CVar* cvar, const char* value, bool setValue, bool setReset, bool setDefault, bool markDirty);

// Stores a value without the callback: the string and its int, int64 and float readings.
void CVarUpdate(CVar* cvar, const char* value, bool setValue, bool setReset, bool setDefault, bool markDirty);

// Finds a CVar by the case-insensitive FNV-1a hash of its name.
CVar* CVarLookup(uint64_t nameHash);

// CVarSet with a number written as text ("%d" / "%f"), and a new coded default.
void CVarSetInt(CVar* cvar, int32_t value, bool setValue, bool setReset, bool setDefault, bool markDirty);
void CVarSetFloat(CVar* cvar, float value, bool setValue, bool setReset, bool setDefault, bool markDirty);
void CVarSetDefaultInt(CVar* cvar, int64_t value);

// The hook every value change is reported to.
void CVarSetChangedHandler(void (*handler)(CVar* cvar, const char* oldValue, const char* newValue));

// True when the CVar has a default and holds it.
bool CVarIsDefault(CVar* cvar);

// Back to the startup (reset) value / the coded default, through the callback.
void CVarResetToStartup(CVar* cvar);
void CVarResetToDefault(CVar* cvar);

// Told about each "[section]" line of a config file; returns whether the section's lines
// apply.
typedef bool (*CVARSECTIONCALLBACK)(const char* line, void* param);

bool CVarParseFile(void* file, CVARSECTIONCALLBACK sectionCallback, void* param, bool executeAll);
bool CVarExecFile(const char* name, CVARSECTIONCALLBACK sectionCallback, void* param, bool executeAll);
void CVarLoadConfig(const char* name);
void CVarExecFileDirty(const char* name);
// A saving CVar's name and value; false stops the walk.
typedef bool (*CVARENUMPROC)(const char* name, const char* value, void* param, uint32_t param2);
bool CVarEnumerate(uint32_t scope, uint32_t exclude, bool annotate, CVARENUMPROC proc, void* param, uint32_t param2);
bool CVarSaveFile(const char* name);
bool CVarSaveConfig();
bool CVarDeleteFile(const char* name);
void CVarShutdownConfig();
void CVarShutdown();

// Makes a latched CVar's pending value current; false when nothing was pending.
bool CVarApplyLatched(CVar* cvar);

// Sets up the CVar lock, optionally the directories above <cwd>\WTF, and the set /
// cvar_reset / cvar_default / cvarlist commands.
void CVarInitialize(bool createWtfDirectory);

// The console command every CVar registers under its name: no arguments prints it,
// otherwise sets it.
int32_t CVarConsoleHandler(const char* command, const char* arguments);

// Bitfield CVars: a value marked 2 in its first byte, then one character per six bits on top of
// 0x40. A value in the older form (marked 1, seven bits a character) is rewritten into it
// (FUN_140322c70); a bit is read (FUN_140320b40) and set or cleared (FUN_140320bb0), and an index
// fits while its character stays under 0xff (FUN_140320110).
void CVarConvertBitfield(CVar* cvar, int32_t marker);
bool CVarGetBit(CVar* cvar, uint32_t index);
void CVarSetBit(CVar* cvar, uint32_t index, bool on);
// The reference's shapes (FUN_140322c70, FUN_140320b40, FUN_140320bb0): the CVar's holder.
void CVarConvertBitfieldRef(CVar* const* holder, int32_t marker);
bool CVarGetBitRef(CVar* const* holder, uint32_t index);
void CVarSetBitRef(CVar* const* holder, uint32_t index, bool on);
bool CVarBitIndexValid(uint32_t index);

#endif
