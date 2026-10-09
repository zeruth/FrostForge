#ifndef CLIENT_COMMAND_LINE_HPP
#define CLIENT_COMMAND_LINE_HPP

#include <cstdint>

// The client's command-line option bits (DAT_142e969c0).
enum COMMAND_LINE_OPTION : uint32_t {
    COMMAND_LINE_D3D11 = 0x1,
    COMMAND_LINE_D3D12 = 0x2,
    COMMAND_LINE_D3D11LEGACY = 0x4,
    COMMAND_LINE_FULLSCREEN = 0x8,
    COMMAND_LINE_WINDOWED = 0x10,
    COMMAND_LINE_LAUNCHERLOGIN = 0x20,
    COMMAND_LINE_ALLOWSOFTWARERENDERER = 0x40,
    COMMAND_LINE_NOSOUND = 0x80,
    COMMAND_LINE_CONSOLE = 0x100,
    COMMAND_LINE_METAL = 0x200,
    COMMAND_LINE_CASREFRESH = 0x400,
    COMMAND_LINE_ENABLEMEMORYSTACKTRACES = 0x1000,
};

bool CommandLineProcess();

const char* CommandLineGetDataDir();

const char* CommandLineGetConfigFile();
const char* CommandLineGetCascDir();

bool CommandLineHasOption(uint32_t option);

const char* CommandLineGetUid();

#endif
