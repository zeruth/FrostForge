#include "client/Main.hpp"
#include "client/CrashReport.hpp"
#include "client/Locale.hpp"
#include "client/GxWindow.hpp"
#include "client/Startup.hpp"
#include "os/Os.hpp"
#include "os/Timer.hpp"
#include "os/Window.hpp"
#include "storm/ErrorReport.hpp"
#include "storm/Memory.hpp"
#include "util/Unimplemented.hpp"
#include <cstdlib>
#include <intrin.h>

namespace {

// PTR_s_WTF_Config_wtf_142617d00: the files every crash report attaches.
const char* const s_crashReportFiles[] = {
    "WTF/Config.wtf",
    "Logs/Model.log",
    "Logs/Client.log",
    "Logs/Checkout.log",
    "Logs/Connection.log",
    "Logs/Sound.log",
    "Logs/gx.log",
    "Logs/BattleNet.log",
    "Logs/PlayBuild-NGDP.log",
    "Logs/PlayBuild-NGDP.server.log",
    "Logs/mac.log",
    "Logs/Hotfix.log",
    "Logs/Ref.log",
    "Logs/Voice.log",
    "Logs/WowConnection.log",
    "Logs/Tact.log",
    ".build.info",
    ".flavor.info",
    "../.build.info",
    "Logs/ComponentRequests.log",
};

// The reference installs _guard_check_icall, a bare `ret`, as the CRT's invalid-parameter
// handler: invalid CRT arguments are ignored rather than reported.
void InvalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
}

}

// The platform-independent body of the client: the role 3.3.5's CommonMain has (the name is
// carried over for that reason). It brings the client up in stages, recording each in
// g_startupFlags, runs the main loop, and tears down what was brought up.
// ref: FUN_1402166e0
void CommonMain() {
    // The reference opens with a call to _guard_check_icall, the Control Flow Guard stub (a
    // bare `ret`). It is a compiler artifact, not ported.

    if (OsHasOversizedAddressSpace()) {
        ErrorDisplayCode(0x8510006B, nullptr, LocaleFromSystem(0), false);
    }

    OsSetCurrentThreadPriority(OS_THREAD_PRIORITY_HIGHEST);
    g_startupFlags |= STARTUP_PRE_INIT;

    for (const char* file : s_crashReportFiles) {
        CrashReportAddFile(file);
    }

    OsSetMainWindowGetter(&WindowGetHandle);
    MemoryRegisterExitHandlers();
    MemoryInstallAllocatorHooks();
    OsInitializeExceptionHandling();
    ErrorReportSetExceptionCallback(&Sub140215370);
    OsCheckSingleInstance("Blizzard Entertainment World of Warcraft");

    int32_t option = 1;
    ErrorReportSetOption(4, &option, sizeof(option));

    OsLogCpuInfo();
    ErrorReportSetBuildString("World of Warcraft: Retail Build (build 54261)");
    ErrorReportSetAppendCallback(&Sub140217F20);
    ErrorReportSetDescribeCallback(&Sub140217E00);
    ErrorReportSetHandlerCallback(&Sub1402178D0);

    if (!Sub1402262E0()) {
        Sub140216880();
        return;
    }

    g_startupFlags |= STARTUP_STAGE_1;
    Sub1402265E0();

    if (StartupServiceInitialize()) {
        g_startupFlags |= STARTUP_STAGE_2;

        if (Sub1402289B0()) {
            g_startupFlags |= STARTUP_STAGE_3;
            Sub1402295B0();
            g_startupFlags |= STARTUP_STAGE_4;
            OsWindowSystemInitialize();
            Sub14022A260();
            g_startupValue = OsGetAsyncTimeMs();
            g_running = 1;

            while (Sub1402225D0()) {
                Sub140225AB0();
            }

            Sub140216880();
            return;
        }

        Sub140227810();
    }

    Sub140226550();
    Sub140226260();
    Sub140216880();
}

// ref: FUN_1402112f0
void ClientRun(int32_t argc, char** argv) {
    (void)argc;
    (void)argv;

    _set_invalid_parameter_handler(&InvalidParameterHandler);
    CrashReportConfigure();
    CommonMain();
    std::exit(0);
}

// What wWinMain calls with UTF-8 argv: records crash-report context, then runs the client.
// ref: FUN_1402112a0
void ClientMain(int32_t argc, char** argv) {
    OsRecordThreadEntryReturnAddresses();
    OsSetMainReturnAddress(_ReturnAddress());
    CrashReportRecordCommandLine(argv, 1, 0, 0);
    ClientRun(argc, argv);
}

