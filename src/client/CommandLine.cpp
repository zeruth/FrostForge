#include "client/CommandLine.hpp"
#include "os/Os.hpp"
#include "storm/Command.hpp"
#include "util/Unimplemented.hpp"

namespace {

uint32_t s_options;       // DAT_142e969c0
char s_dataDir[0x110];    // DAT_142e969d0 (registered as 0x104)
char s_cascDir[0x110];    // DAT_142e96ae0 (registered as 0x104)
char s_uid[0x40];         // DAT_142e96bf0
char s_config[0x104];     // DAT_142e96c30

const char s_usage[] =
    "World of Warcraft usage:\n"
    "[-d3d11legacy]  Force Gx to use Direct X 11 Legacy backend\n"
    "[-d3d11]        Force Gx to use Direct X 11 backend\n"
    "[-d3d12]        Force Gx to use Direct X 12 backend\n"
    "[-allowSoftwareRenderer] Allow game to run on the Software Renderer (performance will be bad)\n"
    "[-enableMemoryStackTraces] Enable stack trace capture for memory allocations on startup\n"
    "[-fullscreen]   Force game to launch in windowed fullscreen mode\n"
    "[-windowed]     Force game to launch in windowed mode\n"
    "[-nosound]      Disable sound\n"
    "[-console]      Enable in-game console (default key '~')\n"
    "[-version | -v] Print version number (major.minor.patch.build_number)\n"
    "[-help | -h]    Prints this help output\n"
    "[-gluescript]  Lua code run at the glue screens\n"
    "[-gamescript]  Lua code run once in-game\n"
    "[-gluescriptFile]  File containing Lua code run at the glue screens\n"
    "[-gamescriptFile]  File containing Lua code run once in-game\n";

}

// SCmd's error callback for the client command line.
// ref: FUN_1405d6b40
void CommandLineError(CMDERROR* error) {
    (void)error;
    FROST_UNIMPLEMENTED();
}


// Registers the client's command-line options, parses the command line against them, and
// unregisters them again. -help prints the usage, -version the build; both stop startup.
// ref: FUN_1405d6c40
bool CommandLineProcess() {
    int32_t help = 0;
    SCmdRegisterArgument(0, 0xFFFFFFFF, "help", &help, 4, 1, 1, nullptr);
    SCmdRegisterArgument(0, 0xFFFFFFFF, "h", &help, 4, 1, 1, nullptr);

    int32_t version = 0;
    SCmdRegisterArgument(0, 0xFFFFFFFF, "version", &version, 4, 1, 1, nullptr);
    SCmdRegisterArgument(0, 0xFFFFFFFF, "v", &version, 4, 1, 1, nullptr);

    SCmdRegisterArgument(0, 0xFFFFFFFF, "d3d11", &s_options, 4, COMMAND_LINE_D3D11, COMMAND_LINE_D3D11, nullptr);
    SCmdRegisterArgument(0, 0xFFFFFFFF, "d3d12", &s_options, 4, COMMAND_LINE_D3D12, COMMAND_LINE_D3D12, nullptr);
    SCmdRegisterArgument(0, 0xFFFFFFFF, "metal", &s_options, 4, COMMAND_LINE_METAL, COMMAND_LINE_METAL, nullptr);
    SCmdRegisterArgument(0, 0xFFFFFFFF, "allowsoftwarerenderer", &s_options, 4, COMMAND_LINE_ALLOWSOFTWARERENDERER,
        COMMAND_LINE_ALLOWSOFTWARERENDERER, nullptr);
    SCmdRegisterArgument(0, 0xFFFFFFFF, "fullscreen", &s_options, 4, COMMAND_LINE_FULLSCREEN, COMMAND_LINE_FULLSCREEN, nullptr);
    SCmdRegisterArgument(0, 0xFFFFFFFF, "windowed", &s_options, 4, COMMAND_LINE_WINDOWED, COMMAND_LINE_WINDOWED, nullptr);
    SCmdRegisterArgument(0, 0xFFFFFFFF, "launcherlogin", &s_options, 4, COMMAND_LINE_LAUNCHERLOGIN,
        COMMAND_LINE_LAUNCHERLOGIN, nullptr);
    SCmdRegisterArgument(0, 0xFFFFFFFF, "d3d11legacy", &s_options, 4, COMMAND_LINE_D3D11LEGACY, COMMAND_LINE_D3D11LEGACY,
        nullptr);
    SCmdRegisterArgument(0, 0xFFFFFFFF, "nosound", &s_options, 4, COMMAND_LINE_NOSOUND, COMMAND_LINE_NOSOUND, nullptr);
    SCmdRegisterArgument(0, 0xFFFFFFFF, "console", &s_options, 4, COMMAND_LINE_CONSOLE, COMMAND_LINE_CONSOLE, nullptr);
    SCmdRegisterArgument(0, 0xFFFFFFFF, "casrefresh", &s_options, 4, COMMAND_LINE_CASREFRESH, COMMAND_LINE_CASREFRESH,
        nullptr);
    SCmdRegisterArgument(0, 0xFFFFFFFF, "enableMemoryStackTraces", &s_options, 4, COMMAND_LINE_ENABLEMEMORYSTACKTRACES,
        COMMAND_LINE_ENABLEMEMORYSTACKTRACES, nullptr);

    SCmdRegisterArgument(CMD_TYPE_STRING, 0xFFFFFFFF, "datadir", s_dataDir, 0x104, 0, 0, nullptr);
    SCmdRegisterArgument(CMD_TYPE_STRING, 0xFFFFFFFF, "cascdir", s_cascDir, 0x104, 0, 0, nullptr);
    SCmdRegisterArgument(CMD_TYPE_STRING, 0xFFFFFFFF, "uid", s_uid, 0x40, 0, 0, nullptr);
    SCmdRegisterArgument(CMD_TYPE_STRING, 0xFFFFFFFF, "config", s_config, 0x104, 0, 0, nullptr);

    uint32_t processed = SCmdProcess(nullptr, &CommandLineError, 1);
    SCmdUnregisterAll();

    if (help == 0 && processed != 0) {
        if (version != 0) {
            OsMessageBox(nullptr, 0x10001, "54261", "WoW Version");
            return false;
        }
        return true;
    }

    OsMessageBox(nullptr, processed ? 0x10001 : 0x40001, s_usage, "Command Line Help");
    return false;
}

// The -datadir value: stage 1 changes into it, or into the executable's directory when
// it is empty.
// ref: FUN_1405d6c30
const char* CommandLineGetDataDir() {
    return s_dataDir;
}

// The -cascdir value: the CAS data folder (empty: the default).
// ref: FUN_1405d6c10
const char* CommandLineGetCascDir() {
    return s_cascDir;
}

// The -config value: the config file stage 2 loads instead of Config.wtf.
// ref: FUN_1405d6c20
const char* CommandLineGetConfigFile() {
    return s_config;
}

// ref: FUN_1405d6bf0
bool CommandLineHasOption(uint32_t option) {
    return (s_options & option) != 0;
}

// The -uid value: the product uid Agent knows this install by.
// ref: FUN_1405d6c00
const char* CommandLineGetUid() {
    return s_uid;
}
