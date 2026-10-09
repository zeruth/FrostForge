#include "client/Startup.hpp"
#include "client/CommandLine.hpp"
#include "client/Locale.hpp"
#include "client/LocalFileHandlers.hpp"
#include "base/Status.hpp"
#include "cas/CAS.hpp"
#include "blz/String.hpp"
#include "console/Command.hpp"
#include "console/Var.hpp"
#include "services/ClientServices.hpp"
#include "os/Os.hpp"
#include "storm/String.hpp"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include "storm/ErrorReport.hpp"
#include "storm/Log.hpp"
#include "storm/Memory.hpp"
#include "util/Unimplemented.hpp"

uint32_t g_startupFlags;
uint32_t g_startupValue;
uint8_t g_running;
uint32_t g_clientInitFlags;  // DAT_142ddd0d8
uint32_t g_clientTeardownFlags;  // DAT_142ddd0dc
CVar* g_agentUid;     // DAT_142ddfa40
CVar* g_launchAgent;  // DAT_142ddfa48

namespace {

// DAT_142ddfa20: a growable array of OsNotifyDir pointers, emptied by stage 1. Owned by the
// directory-notification code, which is not ported yet.
struct {
    void* data;
    uint64_t count;
    int64_t capacity;
} s_notifyDirs;

char s_parentProcessName[0x104];      // DAT_142ddcfc0
uint32_t s_mainThreadId;              // DAT_142ddfa18
HSLOG s_wowConnectionLog;             // DAT_142ddfa38
void (*s_wowConnectionLogCallback)(); // DAT_1430d0278
HSLOG s_defaultLogs[2];               // DAT_1430d2508

}

// The error callback stage 1 installs: shows the error in the text locale.
// ref: FUN_1402262b0
void StartupErrorCallback(uint32_t code, const char* text) {
    ErrorDisplayCode(code, text, LocaleGetText(), true);
}

// Allocates an object from the frame-layout code (Engine/Source/Frame/CLayoutFrame.cpp)
// with the name "RESIZE".
// ref: FUN_1405f64e0
void Sub1405F64E0() {
    FROST_UNIMPLEMENTED();
}

// Installs four allocator hooks into a library's function table (0x142d39c98..0x142d39cb0).
// ref: FUN_14165c0e0
void Sub14165C0E0() {
    FROST_UNIMPLEMENTED();
}

// Initializes the WowConnection layer (Source/.../WowConnection.cpp).
// ref: FUN_1415002e0
void Sub1415002E0(int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5) {
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    FROST_UNIMPLEMENTED();
}

// The WowConnection log callback, installed only when Logs/WowConnection.log opens.
// ref: 0x140226240 (reached only through the pointer)
void Sub140226240() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141566850
void LogOpenDefaultLogs() {
    static const char* const s_names[] = {
        "Logs\\DefaultLogs.log",
        "Logs\\BattleNet.log",
    };

    OsCreateDirectory("Logs", 0, 0);
    for (int32_t i = 0; i < 2; i++) {
        SLogCreate(s_names[i], 1, &s_defaultLogs[i]);
    }
}

// Stores a callback in DAT_1430a1518, read by 0x141237e40 and 0x141237f70.
// ref: FUN_141238140
void Sub141238140(void (*callback)()) {
    (void)callback;
    FROST_UNIMPLEMENTED();
}

// Callback passed to Sub141238140. Signature not yet known.
// ref: FUN_1414f8d60
void Sub1414F8D60() {
    FROST_UNIMPLEMENTED();
}

// The error reporter's exception callback. Signature not yet known.
// ref: 0x140215370 (reached only through the pointer)
void Sub140215370() {
    FROST_UNIMPLEMENTED();
}

// The error reporter's append callback. Signature not yet known.
// ref: FUN_140217f20
void Sub140217F20() {
    FROST_UNIMPLEMENTED();
}

// The error reporter's describe callback. Signature not yet known.
// ref: FUN_140217e00
void Sub140217E00() {
    FROST_UNIMPLEMENTED();
}

// The error reporter's handler callback. Signature not yet known.
// ref: FUN_1402178d0
void Sub1402178D0() {
    FROST_UNIMPLEMENTED();
}

// Stage 1: command line, working directory, logs, the WowConnection layer. Failure skips
// straight to Sub140216880.
// ref: FUN_1402262e0
bool Sub1402262E0() {
    if (!CommandLineProcess()) {
        return false;
    }

    Sub1405F64E0();

    const char* directory = CommandLineGetDataDir();
    char executableDirectory[0x110];
    if (*directory == '\0') {
        OsGetExecutableDirectory(executableDirectory, 0x104);
        directory = executableDirectory;
    }
    OsSetCurrentDirectory(directory);

    if (s_notifyDirs.capacity >= 0) {
        SMemFree(s_notifyDirs.data, ".PEAVOsNotifyDir@@", -6, 0);
    }
    s_notifyDirs.capacity = 0;
    s_notifyDirs.data = nullptr;
    s_notifyDirs.count = 0;
    SMemFree(nullptr, ".PEAVOsNotifyDir@@", -6, 0);

    char parentName[0x118];
    OsGetParentProcessName(parentName);
    {
        char* dest = s_parentProcessName;
        const char* source = parentName;
        int64_t remaining = 0x103;
        for (;;) {
            char c = *source++;
            *dest++ = c;
            if (c == '\0') {
                break;
            }
            if (--remaining == 0) {
                *dest = '\0';
                break;
            }
        }
    }

    Sub14165C0E0();
    // The reference calls 0x1402c8f70 here: a jmp to the shared bare `ret`, i.e. a function
    // whose body is empty in this build. Not ported.
    OsCreateDirectory("Logs", 0, 0);
    // ...and 0x1402112e0("Logs/Ref.log", 0x140226230, 0): another function compiled to
    // nothing (folded into _guard_check_icall's `ret`). Not ported.

    s_mainThreadId = OsGetCurrentThreadId();
    ErrorReportSetErrorCallback(&StartupErrorCallback);

    if (SLogCreate("Logs/WowConnection.log", 0, &s_wowConnectionLog)) {
        s_wowConnectionLogCallback = &Sub140226240;
    }

    Sub1415002E0(0, 0, 1, 0, 0);
    LogOpenDefaultLogs();
    g_clientInitFlags |= 1;
    return true;
}

namespace {


// ref: FUN_140226bd0
bool PortalCallback(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    (void)userData;
    // "public-test" and "cn-test" are refused; anything else picks the portal's region and
    // may turn the profanity filter on. The locale code it reaches is not ported.
    if (!newValue) {
        return false;
    }
    FROST_UNIMPLEMENTED();
    return true;
}

// ref: FUN_140226d90
bool TextLocaleCallback(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    (void)newValue;
    (void)userData;
    FROST_UNIMPLEMENTED();
    return true;
}

// ref: FUN_140226460
bool AudioLocaleCallback(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    (void)newValue;
    (void)userData;
    FROST_UNIMPLEMENTED();
    return true;
}

// Fills textLocale, audioLocale and portal from a four-letter locale when they are empty.
// ref: FUN_140226a10
bool LocaleCallback(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    (void)userData;
    if (std::strlen(newValue) != 4) {
        return true;
    }
    FROST_UNIMPLEMENTED();
    return true;
}

// Accepts timing methods 0..3 (listing them otherwise); a change clears timingTestError
// (userData).
// ref: FUN_140226dd0
bool TimingMethodCallback(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    (void)newValue;
    (void)userData;
    FROST_UNIMPLEMENTED();
    return true;
}

// "timingInfo": prints the desired and selected timing methods and the test error.
// ref: FUN_1402264a0
int32_t TimingInfoCommand(const char* command, const char* arguments) {
    (void)command;
    (void)arguments;
    FROST_UNIMPLEMENTED();
    return 1;
}

// timeBeginPeriod(1), then sets up the game timer with a timing method.
// ref: FUN_141237d90
void TimingInitialize(int32_t method) {
    (void)method;
    FROST_UNIMPLEMENTED();
}

// Opens Logs\Movie.log and registers the movie recorder's two handlers.
// ref: FUN_1405fde10
void MovieInitialize() {
    FROST_UNIMPLEMENTED();
}

// agentUID's default: the program name Agent knows this build by.
// ref: FUN_1402278a0
const char* AgentUidDefault(const char* live) {
    int32_t type = GetBuildType();
    if (type == 0) {
        return "wow_ptr";
    }
    if (type == 1) {
        return "wow_beta";
    }
    if (type == 3) {
        return "wow_internal";
    }
    return live;
}

// OsFindFiles callback: appends each file name to the string vector.
// ref: FUN_140216420
bool CollectFileName(OSFILEINFO* info, blz::string_vector* names) {
    if (names->m_size == names->capacity()) {
        names->reserve(names->capacity() + 1);
    }
    if (blz::string* slot = names->m_data + names->m_size) {
        slot->construct(info->m_name, std::strlen(info->m_name));
    }
    names->m_size++;
    return false;
}

}

// Stage 2: the CVar system and console commands, the startup CVars, Config.wtf and any
// WTF\RunOnce*.wtf, then the game timer.
// ref: FUN_1402265e0
void Sub1402265E0() {
    CVarInitialize(true);
    // 0x1402112e0, an empty function, is called here
    ConsoleRegisterBasicCommands();

    CVarRegister("portal", "Name of Battle.net portal to use", 0x800, "", &PortalCallback, 6, false, nullptr, false);
    CVarRegister("textLocale", "Set the game locale for text", 0x800, "", &TextLocaleCallback, 4, false, nullptr, false);
    CVarRegister("audioLocale", "Set the game locale for audio content", 0x800, "", &AudioLocaleCallback, 4, false,
        nullptr, false);
    CVarRegister("locale", "Set the game locale", 0x400, "", &LocaleCallback, 5, false, nullptr, false);
    g_agentUid = CVarRegister("agentUID", "The UID provided by Battle.net to be passed to Agent", 0,
        AgentUidDefault(""), static_cast<CVARCALLBACK>(nullptr), 4, false, nullptr, false);
    CVar* timingTestError = CVarRegister("timingTestError", "Error reported by the timing validation system", 6, "0",
        static_cast<CVARCALLBACK>(nullptr), 5, false, nullptr, false);
    CVar* timingMethod = CVarRegister("timingMethod", "Desired method for game timing", 2, "0", &TimingMethodCallback,
        5, false, timingTestError, false);
    CVarRegister("enableAssetTracking", "Whether to track which assets are least recently used", 0, "1",
        static_cast<CVARCALLBACK>(nullptr), 4, false, nullptr, false);
    g_launchAgent = CVarRegister("launchAgent", "Set this to have the client start up Agent", 0, "1",
        static_cast<CVARCALLBACK>(nullptr), 4, false, nullptr, false);

    const char* config = CommandLineGetConfigFile();
    if (!config || *config == '\0') {
        config = "Config.wtf";
    }
    CVarLoadConfig(config);

    blz::string_vector runOnce = { nullptr, 0, 0 };
    char dir[0x104];
    {
        char* out = dir;
        const char* in = "WTF\\";
        for (int64_t left = 0x103;;) {
            char c = *in++;
            *out++ = c;
            if (c == '\0') {
                break;
            }
            if (--left == 0) {
                *out = '\0';
                break;
            }
        }
    }
    blz::function_storage collect;
    collect.m_invoke = reinterpret_cast<void*>(&OsFindFilesInvokePointer);
    collect.m_manager = &blz::function_trivial_manager;
    auto collector = &CollectFileName;
    std::memcpy(collect.m_storage, &collector, sizeof(collector));
    OsFindFiles(dir, "RunOnce*.wtf", &collect, &runOnce, true);

    // FUN_140215390 is the library's introsort; the order is the comparator's, which puts the
    // names in descending collation order.
    std::sort(runOnce.m_data, runOnce.m_data + runOnce.m_size, [](const blz::string& a, const blz::string& b) {
        return SStrCmpUTF8I(a.m_data, b.m_data, 0x7FFFFFFF) > 0;
    });
    for (blz::string* name = runOnce.m_data; name != runOnce.m_data + runOnce.m_size; name++) {
        CVarExecFileDirty(name->m_data);
    }
    for (blz::string* name = runOnce.m_data; name != runOnce.m_data + runOnce.m_size; name++) {
        if (!name->IsInline()) {
            SMemFree(name->m_data, ".D", -6, 0);
        }
    }
    runOnce.m_size = 0;
    runOnce.destroy();

    CVarApplyLatched(timingMethod);
    TimingInitialize(timingMethod->m_intValue);
    ConsoleCommandRegister("timingInfo", &TimingInfoCommand, 0, nullptr, 0);
    MovieInitialize();
    g_clientInitFlags |= 2;
}

namespace {

// What stage 3 keeps of a CAS start (0x180 bytes): the Patch Service's answer when the client
// asks it (the strings), the status everything reports into, and a failure kind.
struct CASSTARTINFO {
    uint64_t m_00;                  // 0x000
    blz::string m_08;               // 0x008
    blz::string m_30;               // 0x030
    blz::string m_58;               // 0x058
    blz::string m_80;               // 0x080
    int32_t m_a8;                   // 0x0a8
    CStatus m_status;               // 0x0b0
    blz::string m_d8;               // 0x0d8: the text an error dialog shows
    blz::string m_100;              // 0x100
    blz::string m_128;              // 0x128
    blz::string m_150;              // 0x150
    int32_t m_failure;              // 0x178: 0x28 until something fails

    // ref: FUN_140228350
    CASSTARTINFO() : m_00(0), m_a8(0), m_failure(0x28) {}
    // FUN_140228470 is the destructor the members make.
};
static_assert(offsetof(CASSTARTINFO, m_status) == 0xb0, "CASSTARTINFO layout");
static_assert(offsetof(CASSTARTINFO, m_failure) == 0x178, "CASSTARTINFO layout");
static_assert(sizeof(CASSTARTINFO) == 0x180, "CASSTARTINFO is 0x180 bytes");

// What file reads call when one fails (DAT_142e8e2c8; read by FUN_14057dc80).
bool (*s_fileErrorHandler)();

// ref: FUN_1405819a0
void FileSetErrorHandler(bool (*handler)()) {
    s_fileErrorHandler = handler;
}

// Shows the error dialog for an error code; the dialog system is not ported.
// ref: FUN_140217000
void ErrorDisplayCode(uint32_t code, const char* text) {
    (void)code;
    (void)text;
    FROST_UNIMPLEMENTED();
}

// A failed read of game data is fatal when CAS says why: the dialog, then the process ends.
// ref: FUN_140229170
bool CasFileErrorHandler() {
    int32_t kind;
    uint32_t error = cas::CASGetErrorCode(&kind);
    if (kind != 0) {
        return false;
    }
    uint32_t code;
    if (error == 6 || error == 7) {
        code = 0x85100089;
    } else if (error == 8 || error == 0xb) {
        code = 0x8510006c;
    } else if (error == 0xa) {
        code = 0x8510006a;
    } else {
        return false;
    }
    ErrorDisplayCode(code, "");
    OsTerminateProcess();
    return false;
}

// One hot-fix status line, to the console in its severity's colour.
// ref: 0x140229250
void HotfixStatusLine(int32_t severity, const blz::string* text, void* storage) {
    (void)storage;
    uint32_t color = 0;
    if (severity == 1) {
        color = 4;
    } else if (severity == 2 || severity == 3) {
        color = 3;
    }
    ConsoleWriteFormat(color, text->m_data);
}

// ref: FUN_140229290
void HotfixSwapHandler(const CStatus* status, void* storage) {
    (void)storage;
    blz::function_storage line;
    line.m_invoke = reinterpret_cast<void*>(&HotfixStatusLine);
    line.m_manager = &blz::function_trivial_manager;
    status->ForEach(&line);
    line.m_manager(line.m_storage, nullptr, 0);
    ConsoleWriteFormat(3, status->m_maxSeverity < 2 ? "file hot fix swap complete" : "file hot fix swap failed");
}

// Re-applies the manifest's locales and writes WoW.mfil. It needs the CAS manifest-state
// query (FUN_1402cdf50) and the .mfil writer (FUN_1402cd760), not ported; nothing raises the
// event yet.
// ref: FUN_140229300
void ManifestChangeHandler(const char* name, void* storage) {
    (void)name;
    (void)storage;
    FROST_UNIMPLEMENTED();
}

// Reports a failed CAS start: a repair prompt, a patch prompt, or an error dialog naming the
// failure. The dialogs and the launcher hand-off are not ported.
// ref: FUN_140228610
void ReportCasFailure(CASSTARTINFO* info) {
    (void)info;
    FROST_UNIMPLEMENTED();
}

}

// Stage 3: CAS. The install's .build.info names the build; the build manifest and the local
// manifest of loose files come up, and the hot-fix and manifest-change handlers are added.
// Failure runs Sub140227810.
// ref: FUN_1402289b0
bool Sub1402289B0() {
    CASSTARTINFO info;
    const char* cascDir = CommandLineGetCascDir();
    if (cascDir != nullptr && *cascDir == '\0') {
        cascDir = nullptr;
    }
    cas::MANIFESTLOCALE locale = {};
    locale.m_region = RegionGet();
    locale.m_text = LocaleToCasIndex(LocaleGetText());
    locale.m_speech = LocaleToCasIndex(LocaleGetAudio());
    locale.m_alternate = g_overrideArchive->m_intValue != 0;

    char directory[0x104];
    OsGetWorkingDirectory(0x104, directory);
    char localRoot[0x104];
    OsPathCombine(localRoot, 0x104, directory, "");
    TextureRegisterLocalFileHandlers();
    SoundRegisterLocalFileHandlers();
    cas::CASSetLogEnabled(true);
    FileSetErrorHandler(&CasFileErrorHandler);
    ConsoleWriteText("Attempting to initialize from .build.info");

    blz::string product;
    bool started = cas::CASInitFromBuildInfo(0x76c3, "3.4.3.54261", &locale, cascDir, localRoot, &info.m_status,
        &product, false);
    if (!started) {
        // DIVERGED: the reference now asks the Patch Service, five times half a second apart:
        // http://<region>.patch.battle.net:1119/<product>/versions and /cdns (product
        // "wow_classic" when .build.info gave none), then CASInitFromPatchService
        // (FUN_1402ce6c0) with its answer. FrostLite runs offline -- the AzerothCore side will
        // serve this later -- so a missing or mismatched .build.info fails here.
        if (product.m_size == 0) {
            product.assign("wow_classic");  // PTR_s_wow_classic_142cc84c8
        }
        ConsoleWriteFormat(0, "Attempting to initialize '%s' from Patch Service", product.m_data);
        FROST_UNIMPLEMENTED();
        ReportCasFailure(&info);
        cas::CASShutdown();
        return false;
    }

    g_clientInitFlags |= 0x180;
    blz::function_storage handler;
    handler.m_invoke = reinterpret_cast<void*>(&HotfixSwapHandler);
    handler.m_manager = &blz::function_trivial_manager;
    cas::CASAddHotfixHandler(&handler, reinterpret_cast<uint64_t>(&Sub1402289B0));
    if (handler.m_manager != &blz::function_trivial_manager) {
        handler.m_manager(handler.m_storage, nullptr, 0);
    }
    handler.m_manager = &blz::function_trivial_manager;
    handler.m_invoke = reinterpret_cast<void*>(&ManifestChangeHandler);
    cas::CASAddManifestHandler(&handler, reinterpret_cast<uint64_t>(&Sub1402289B0));
    if (handler.m_manager != &blz::function_trivial_manager) {
        handler.m_manager(handler.m_storage, nullptr, 0);
    }
    return true;
}

// ref: FUN_1402295b0
void Sub1402295B0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14123b190
void Sub14123B190() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14022a260
void Sub14022A260() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141237bf0
uint32_t Sub141237BF0() {
    FROST_UNIMPLEMENTED();
    return 0;
}

// The main loop's condition: CommonMain runs Sub140225AB0 while this returns true.
// ref: FUN_1402225d0
bool Sub1402225D0() {
    FROST_UNIMPLEMENTED();
    return false;
}

// One iteration of the main loop.
// ref: FUN_140225ab0
void Sub140225AB0() {
    FROST_UNIMPLEMENTED();
}

// Unwinds stage 2 when stage 3 fails.
// ref: FUN_140227810
void Sub140227810() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140226550
void Sub140226550() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140226260
void Sub140226260() {
    FROST_UNIMPLEMENTED();
}

// Final shutdown; reads g_startupFlags to know what was brought up.
// ref: FUN_140216880
void Sub140216880() {
    FROST_UNIMPLEMENTED();
}
