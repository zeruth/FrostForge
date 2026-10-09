#ifndef STORM_COMMAND_HPP
#define STORM_COMMAND_HPP

#include <cstdint>

// SCmdRegisterArgument flags
#define CMD_FLAG_INVERT_OR_SIGNED  0x1        // bool: cleared unless "+"; number: signed
#define CMD_FLAG_64BIT             0x2        // number: 64-bit
#define CMD_FLAG_CASE_SENSITIVE    0x100
#define CMD_TYPE_MASK              0x30000
#define CMD_TYPE_BOOL              0x00000
#define CMD_TYPE_NUMBER            0x10000
#define CMD_TYPE_STRING            0x20000
#define CMD_PARAM_MASK             0x3000000  // positional parameters, not -flags
#define CMD_PARAM_OPTIONAL         0x1000000
#define CMD_PARAM_REQUIRED         0x2000000

struct CMDPARAMS {
    uint32_t flags;
    uint32_t id;
    const char* name;
    void* variableptr;
    uint32_t setvalue;
    uint32_t setmask;
    uint64_t currvalue;
};

struct CMDERROR {
    uint32_t errorcode;
    const char* itemstring;
    const char* errorstring;
};

typedef int32_t (*CMDEXTRACALLBACK)(CMDPARAMS* params, const char* argument);
typedef int32_t (*CMDUNHANDLEDCALLBACK)(const char* argument);
typedef void (*CMDERRORCALLBACK)(CMDERROR* error);

void SCmdRegisterArgument(uint32_t flags, uint32_t id, const char* name, void* variableptr, uint32_t variablebytes,
    uint32_t setvalue, uint32_t setmask, CMDEXTRACALLBACK callback);

uint32_t SCmdProcess(CMDUNHANDLEDCALLBACK unhandled, CMDERRORCALLBACK error, int32_t continueOnUnknown);

bool SCmdUnregisterAll();

#endif
