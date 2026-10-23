#include "base/RndSeed.hpp"
#include "client/Startup.hpp"
#include "ui/OldMovie.hpp"
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
#include "os/Timer.hpp"
#include "os/Window.hpp"
#include "util/Fnv.hpp"
#include "storm/String.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include "storm/ErrorReport.hpp"
#include "storm/Log.hpp"
#include "storm/Memory.hpp"
#include "util/Unimplemented.hpp"
#include "db/DB2Tables.hpp"
#include "db/TactKeys.hpp"
#include "ui/CLayoutFrame.hpp"
#include <windows.h>

extern "C" int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);

uint32_t g_startupFlags;
uint32_t g_startupValue;
uint8_t g_running;
uint32_t g_clientInitFlags;  // DAT_142ddd0d8
uint32_t g_clientTeardownFlags;  // DAT_142ddd0dc
CVar* g_agentUid;     // DAT_142ddfa40
CVar* g_launchAgent;  // DAT_142ddfa48

// A telemetry stream's enable (vtable slot 0x88 of DAT_142e94e90 / DAT_142e94c80, flag 4 meaning
// DAT_142ccdf30's default); the streams are offline.
// ref: FUN_14057d1b0
void TelemetryStreamEnable(void* stream, uint32_t flags) {
    (void)stream;
    (void)flags;
    FROST_UNIMPLEMENTED();
}

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
uint32_t (*s_wowConnectionLogCallback)(uint64_t, uint64_t); // DAT_1430d0278
HSLOG s_defaultLogs[2];               // DAT_1430d2508

}

// The error callback stage 1 installs: shows the error in the text locale.
// ref: FUN_1402262b0
void StartupErrorCallback(uint32_t code, const char* text) {
    ErrorDisplayCode(code, text, LocaleGetText(), true);
}

// Installs four allocator hooks into a library's function table (0x142d39c98..0x142d39cb0).
// ref: FUN_14165c0e0
void Sub14165C0E0() {
    FROST_UNIMPLEMENTED();
}

extern "C" unsigned long long FUN_141502660();
extern "C" unsigned long long FUN_141502b40(unsigned long long, unsigned long long, unsigned long long,
    unsigned long long);
extern "C" unsigned long long FUN_141503670(unsigned long long);
extern "C" unsigned long long FUN_141503240(unsigned long long);

// Initializes the WowConnection layer (Source/.../WowConnection.cpp): clears the socket table and
// creates and starts the network engine (DAT_1430d0308) with up to 32 threads. Without the engine
// every WowConnection connect is a no-op, so the Login click never reached the server.
// translated: FUN_141502660, FUN_141502b40, FUN_141503670, FUN_141503240
// ref: FUN_1415002e0
void Sub1415002E0(uint64_t a1, uint64_t a2, uint32_t threads, uint8_t a4, uint64_t a5) {
    (void)a4;
    uint64_t& engine = *reinterpret_cast<uint64_t*>(0x1430d0308ull);
    if (engine != 0) {
        return;
    }

    // the socket table, a blz::chained_hash_node<unsigned __int64> map: free every node
    uint8_t* buckets = *reinterpret_cast<uint8_t**>(0x142d08298ull);
    uint64_t bucketCount = *reinterpret_cast<uint64_t*>(0x142d08290ull);
    if (buckets != nullptr && bucketCount != 0) {
        for (uint64_t i = 0; i < bucketCount; i++) {
            void** node = *reinterpret_cast<void***>(buckets + i * 8);
            while (node != nullptr) {
                void** next = static_cast<void**>(*node);
                SMemFree(node, reinterpret_cast<const char*>(0x142dadb08ull), -6, 0);
                node = next;
            }
            *reinterpret_cast<void**>(buckets + i * 8) = nullptr;
        }
    }
    *reinterpret_cast<uint64_t*>(0x142d082a0ull) = 0;
    FUN_141502660();
    *reinterpret_cast<uint32_t*>(0x1430d02f8ull) = 0;

    static const char s_file[] = "D:\\BuildServer\\A\\work-git\\wow\\WoW\\Source\\WowConnection\\WowConnection.cpp";
    uint32_t hash = 0x811c9dc5;
    for (const char* c = s_file; *c != '\0'; c++) {
        hash = (hash ^ static_cast<uint8_t>(*c)) * 0x1000193;
    }
    uint32_t count = threads < 0x21 ? threads : 0x20;

    *reinterpret_cast<uint64_t*>(0x1430d0270ull) = a1;
    void* memory = SMemAlloc(0xcc8, 3, s_file, hash, 0x7dd, 0);
    engine = FUN_141502b40(reinterpret_cast<uintptr_t>(memory), count, a2, a5);
    FUN_141503670(engine);
    FUN_141503240(engine);
}

// The WowConnection log callback, installed only when Logs/WowConnection.log opens.
// It hands its two arguments to the log's writer (FUN_14025b2e0 on DAT_142ddfa38) and returns
// the first when no log is open.
extern "C" unsigned long long FUN_140226240(unsigned long long, unsigned long long);
// translated: FUN_140226240
uint32_t Sub140226240(uint64_t a1, uint64_t a2) {
    __try {
        return static_cast<uint32_t>(FUN_140226240(a1, a2));
    } __except (GenFault(0x140226240ull, GetExceptionInformation())) {
        return static_cast<uint32_t>(a1);
    }
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

    CLayoutFrame::CreateResizeAllocator();

    // DIVERGED: with no -datadir the reference moves to its executable's directory
    // (OsGetExecutableDirectory), which is the install it ships in. FrostLite's executable lives
    // in the build tree, so it keeps the working directory it was started in, the install. (The
    // stubbed OsPathStripFilename hid this: the move was to the exe's own path and failed. Its
    // translation strips the name, the move succeeded, and the client found no Config.wtf and no
    // .build.info, 2026-10-18.)
    const char* directory = CommandLineGetDataDir();
    if (*directory != '\0') {
        OsSetCurrentDirectory(directory);
    }

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
extern "C" unsigned long long FUN_140226dd0(unsigned long long, unsigned long long, unsigned long long, unsigned long long);
// translated: FUN_140226dd0
bool TimingMethodCallback(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    __try {
        return (FUN_140226dd0((unsigned long long)cvar, (unsigned long long)oldValue, (unsigned long long)newValue, (unsigned long long)userData) & 0xff) != 0;
    } __except (GenFault(0x140226dd0ull, GetExceptionInformation())) {
        return false;
    }
}

// "timingInfo": prints the desired and selected timing methods and the test error.
// ref: FUN_1402264a0
int32_t TimingInfoCommand(const char* command, const char* arguments) {
    (void)command;
    (void)arguments;
    int32_t desired = CVarLookup(t2util::ConstFnvHashStringI("timingMethod"))->m_intValue;
    int32_t selected = OsTimerGetMethod();
    int32_t error = CVarLookup(t2util::ConstFnvHashStringI("timingTestError"))->m_intValue;
    ConsoleWriteFormat(0, "Timing method desired: %d - %s", desired, OsTimerMethodName(desired));
    ConsoleWriteFormat(0, "Timing method selected: %d - %s", selected, OsTimerMethodName(selected));
    ConsoleWriteFormat(0, "Timing test error: %d", error);
    return 1;
}


// Opens Logs\Movie.log and registers the movie player's audio device handlers (ui/OldMovie).
void MovieInitialize() {
    MoviePlayerInitialize();
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
    OsTimerInitialize(timingMethod->m_intValue);
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
extern "C" unsigned long long FUN_140217000(unsigned long long, unsigned long long);
// translated: FUN_140217000
void ErrorDisplayCode(uint32_t code, const char* text) {
    __try {
        FUN_140217000((unsigned long long)code, (unsigned long long)text);
    } __except (GenFault(0x140217000ull, GetExceptionInformation())) {
    }
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
extern "C" unsigned long long FUN_140229300(unsigned long long, unsigned long long);
// translated: FUN_140229300
void ManifestChangeHandler(const char* name, void* storage) {
    __try {
        FUN_140229300((unsigned long long)name, (unsigned long long)storage);
    } __except (GenFault(0x140229300ull, GetExceptionInformation())) {
    }
}

// Reports a failed CAS start: a repair prompt, a patch prompt, or an error dialog naming the
// failure. The dialogs and the launcher hand-off are not ported.
extern "C" unsigned long long FUN_140228610(unsigned long long);
// translated: FUN_140228610
void ReportCasFailure(CASSTARTINFO* info) {
    __try {
        FUN_140228610((unsigned long long)info);
    } __except (GenFault(0x140228610ull, GetExceptionInformation())) {
    }
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

namespace {

// The product's registry key under Blizzard Entertainment (0x142cc84d0, 0x80 bytes), with
// "\PTR" or "\Beta" appended for those builds. Each call appends again, as in the reference.
char s_registryProduct[0x80] = "World of Warcraft";

void AppendBounded(char* buffer, uint64_t size, const char* text) {
    char* end = buffer;
    uint64_t room = size;
    while (room != 0 && *end != '\0') {
        end++;
        room--;
    }
    room = static_cast<uint64_t>(buffer + size - end);
    if (room != 0) {
        for (; *text != '\0'; text++) {
            if (room != 1) {
                *end++ = *text;
                room--;
            }
        }
        *end = '\0';
    }
}

// ref: FUN_140229450
const char* RegistryProductKey() {
    int32_t type = GetBuildType();
    if (type == 0) {
        AppendBounded(s_registryProduct, sizeof(s_registryProduct), "\\");
        AppendBounded(s_registryProduct, sizeof(s_registryProduct), "PTR");
    } else if (type == 1) {
        AppendBounded(s_registryProduct, sizeof(s_registryProduct), "\\");
        AppendBounded(s_registryProduct, sizeof(s_registryProduct), "Beta");
    }
    return s_registryProduct;
}

// The command line the error reporter keeps (its executable path first).
// ref: FUN_140211580
void GetCommandLineCopy(char* dest, uint64_t size) {
    const char* source = g_errorReportCommand;
    if (size == 0) {
        return;
    }
    for (;;) {
        if (--size == 0) {
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

// Two splits of the unit interval (0x142dde820..0x142dde83c): {0.3, 0.6} normally,
// {0, 0.3} when `low` is set, with the widths between them.
struct SPLITS {
    uint32_t m_low;                 // 0x20
    float m_24;                     // 0x24
    float m_first;                  // 0x28: 0 (the reference writes it as an int)
    float m_second;                 // 0x2c
    float m_start;                  // 0x30
    float m_middle;                 // 0x34
    float m_end;                    // 0x38
    float m_rest;                   // 0x3c
};
SPLITS s_splits;

// ref: FUN_140219a60
void SetSplits(int32_t low) {
    s_splits.m_first = 0.0f;
    s_splits.m_start = 0.3f;   // DAT_142618d5c
    s_splits.m_end = 0.6f;     // DAT_142618d60
    if (low != 0) {
        s_splits.m_start = 0.0f;
        s_splits.m_end = 0.3f;
    }
    s_splits.m_second = s_splits.m_start;
    s_splits.m_low = low != 0;
    s_splits.m_middle = s_splits.m_end - s_splits.m_start;
    s_splits.m_rest = 1.0f - s_splits.m_end;  // DAT_142618d64
}

// The telemetry encryption setup: offline (docs/stubs.md).
// ref: FUN_140229940
void TelemetryEncryptionHandler(int32_t a1) {
    (void)a1;
    FROST_UNIMPLEMENTED();
}

// The initial hotfix listener stage 1 installs: on event 3 (the initial hotfixes are in)
// FUN_141359ec0 walks the table at DAT_142cfdc08 and sets DAT_1430ab6f4, which
// ClientServicesStatusCallback waits for before the character list goes to the glue; event 4
// clears it again (FUN_141359fd0). Both run translated. They were stubs here under telemetry
// names until 2026-10-20, and their ref tags took the translated callers with them, so the flag
// was never set.
extern "C" unsigned long long FUN_141359ec0(...);
extern "C" unsigned long long FUN_141359fd0(...);

// ref: 0x140229920
void InitialHotfixDataListener(int32_t event) {
    if (event == 3) {
        FUN_141359ec0();
    } else if (event == 4) {
        FUN_141359fd0();
    }
}

uint8_t s_telemetryStreamA;  // DAT_142e94e90
uint8_t s_telemetryStreamB;  // DAT_142e94c80

// "simd": masks the CPU's SIMD features with the value and installs the math routines for
// what is left: the feature detection (cpuid) and the dispatch table (FUN_1412be6f0, vtable
// 0x142728920 over DAT_1430a7cc0) are the reference's, which the translated math reads.
extern "C" unsigned long long FUN_140229860(unsigned long long, unsigned long long, unsigned long long);
// translated: FUN_140229860
bool SimdCallback(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)userData;
    __try {
        return (FUN_140229860((unsigned long long)cvar, (unsigned long long)oldValue, (unsigned long long)newValue) & 0xff) != 0;
    } __except (GenFault(0x140229860ull, GetExceptionInformation())) {
        return true;
    }
}

// ref: FUN_140229800
bool ProcessAffinityCallback(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    (void)userData;
    uint32_t limit = 0xFFFFFFFFu >> ((0x20u - OsGetCpuInfo()->m_logical) & 0x1f);
    uint32_t mask = static_cast<uint32_t>(std::atoi(newValue));
    if (limit < mask) {
        ConsoleWriteFormat(0, "Specified mask %08x is greater than the maximum allowable value of %08x", mask, limit);
        return false;
    }
    return true;
}

CRndSeed s_seedA;  // DAT_142ddcf50
CRndSeed s_seedB;  // DAT_142ddcf80

CVar* s_streamingCameraRadius;          // DAT_1430a55d8
CVar* s_streamingCameraMaxRadius;       // DAT_1430a55e0
CVar* s_streamingCameraLookAheadTime;   // DAT_1430a55e8

// ref: FUN_141282bc0
void StreamingCameraRegisterCVars() {
    s_streamingCameraRadius = CVarRegister("streamingCameraRadius", "Base radius of the streaming camera.", 1, "100",
        static_cast<CVARCALLBACK>(nullptr), 1, false, nullptr, false);
    s_streamingCameraMaxRadius = CVarRegister("streamingCameraMaxRadius", "Max radius of the streaming camera.", 1,
        "250", static_cast<CVARCALLBACK>(nullptr), 1, false, nullptr, false);
    s_streamingCameraLookAheadTime = CVarRegister("streamingCameraLookAheadTime", "Look ahead time for streaming.", 1,
        "2000", static_cast<CVARCALLBACK>(nullptr), 1, false, nullptr, false);
}

extern "C" unsigned long long FUN_141487ed0();
// The Battle.net client (WoW/Source/WowBattleNet/IBattleNet.cpp): the bgs-sdk core and its TLS
// layer, the realm-list CVars, the login and account services. Not ported: it runs translated,
// and connects only where the portal CVar points (docs/login-roadmap.md, Phase 2).
// translated: FUN_141487ed0
void BattleNetInitialize() {
    __try {
        FUN_141487ed0();
    } __except (GenFault(0x141487ed0ull, GetExceptionInformation())) {
    }
}

// cleared while the system sleeps.
uint8_t s_systemAwake = 1;  // 0x142cc8000

// ref: FUN_140214990
void SetSystemAwake(uint8_t awake) {
    s_systemAwake = awake;
}

// ref: 0x1402297a0
void OnSuspend() {
    SetSystemAwake(0);
}

// On resume a session that was up is ended (reason 1), then the client learns of it through
// OS event 0x1a.
// ref: FUN_1402297b0
void OnResume() {
    if (ClientServicesExists()) {
        auto services = static_cast<ClientServices*>(ClientServicesGet());
        if (services->m_sessionUp) {
            services->Disconnect(1);
        }
    }
    SetSystemAwake(1);
    OsQueueEvent(0x1a, 0, 0, 0);
}

extern "C" unsigned long long FUN_141487dc0();
// The Battle.net client's end: its log line, the Aurora and login services torn down, the realm
// connection's status and auth-data callbacks cleared. Runs translated, like
// BattleNetInitialize.
// translated: FUN_141487dc0
void BattleNetShutdown() {
    __try {
        FUN_141487dc0();
    } __except (GenFault(0x141487dc0ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141282a80();
// An object of 0x148 bytes (DAT_1430a55d0) freed. Not ported: it runs translated.
// translated: FUN_141282a80
void Sub141282A80() {
    __try {
        FUN_141282a80();
    } __except (GenFault(0x141282a80ull, GetExceptionInformation())) {
    }
}

}

// Stage 4: process-wide settings (install path, SIMD, CPU affinity), random seeds, the
// streaming camera, Battle.net, and the power hooks.
// ref: FUN_1402295b0
void Sub1402295B0() {
    const char* product = RegistryProductKey();
    char path[0x618];
    GetCommandLineCopy(path, sizeof(path));
    if (path[0] != '\0') {
        char* slash = std::strrchr(path, '\\');
        if (slash != nullptr) {
            slash[1] = '\0';
            // DIVERGED: the reference writes this directory as InstallPath under
            // HKEY_*\Software\Blizzard Entertainment\<product> (FUN_14025cad0). FrostLite shares
            // the machine with a real install and leaves the registry alone.
            (void)product;
        }
    }
    SetSplits(0);
    TactKeysSetup(&TelemetryEncryptionHandler);
    HotfixAddInitialListener(&InitialHotfixDataListener);
    TelemetryStreamEnable(&s_telemetryStreamA, 4);
    TelemetryStreamEnable(&s_telemetryStreamB, 4);
    CVarRegister("simd", "Enable SIMD features (e.g. SSE)", 0, "-1", &SimdCallback, 5, false, nullptr, false);
    CVar* affinity = CVarRegister("processAffinityMask",
        "Sets which core(s) WoW may execute on - changes require restart to take effect", 2, "0",
        &ProcessAffinityCallback, 0, false, nullptr, false);
    CVar* videoOptionsVersion = CVarLookup(t2util::ConstFnvHashStringI("videoOptionsVersion"));
    if (videoOptionsVersion == nullptr || videoOptionsVersion->m_intValue < 3) {
        char value[8];
        SStrPrintf(value, sizeof(value), "%u", 0);
        CVarSet(affinity, value, true, false, false, true);
        CVarApplyLatched(affinity);
    }
    if (affinity->m_intValue != 0) {
        OsSetProcessAffinity(static_cast<uint32_t>(affinity->m_intValue));
    }
    RndSeedInit(&s_seedA, OsGetAsyncTimeMs());
    RndSeedInit(&s_seedB, OsGetAsyncTimeMs());
    StreamingCameraRegisterCVars();
    BattleNetInitialize();
    g_clientInitFlags |= 0x200;
    OsAddSuspendHandler(&OnSuspend);
    OsAddResumeHandler(&OnResume);
}


// Stage 2's end: the timer resolution back, the console commands dropped, the settings written
// to Config.wtf (and the game pad's addon bindings) unless this is a kiosk, and the CVars let go.
// ref: FUN_140226550
void Sub140226550() {
    OsTimerShutdown();
    ConsoleCommandTableClear();
    if (!Sub1406A8850(Sub14192FA70())) {
        CVarSaveConfig();
        GamePadSaveAddonConfig();
    }
    CVarShutdownConfig();
    g_clientTeardownFlags |= 2;
}

// The console command table emptied.
// ref: FUN_140323870
void ConsoleCommandTableClear() {
    // Not run, for the reason CVarShutdown gives: the commands are still reachable from the
    // parts of the client that are never torn down.
    FROST_UNIMPLEMENTED();
}

// WTF\GamePadConfig_Addons.json written when the game pad has addon bindings.
// ref: FUN_14152ad60
void GamePadSaveAddonConfig() {
    FROST_UNIMPLEMENTED();  // no game pad support is ported
}

// Stage 4's end.
// ref: FUN_14022a040
void Sub14022A040() {
    FROST_UNIMPLEMENTED();
}

// The window system's end.
// ref: FUN_14123abd0
void Sub14123ABD0() {
    FROST_UNIMPLEMENTED();
}

// Stage 4's settings undone: the power hooks out, the Battle.net client ended, the startup
// DB2s unloaded.
// ref: FUN_1402293f0
void Sub1402293F0() {
    OsRemoveSuspendHandler(&OnSuspend);
    OsRemoveResumeHandler(&OnResume);
    Sub141282A80();
    BattleNetShutdown();
    g_startup_StringsDB.Unload(false);
    g_startupFilesDB.Unload(false);
    g_clientTeardownFlags |= 0x200;
}

// ref: FUN_1402285e0
void Sub1402285E0() {
    FROST_UNIMPLEMENTED();
}

extern "C" unsigned long long FUN_140226260();
// translated: FUN_140226260
void Sub140226260() {
    __try {
        FUN_140226260();
    } __except (GenFault(0x140226260ull, GetExceptionInformation())) {
    }
}

// Final shutdown: the quit's release if nothing did it yet, the window system's end if the loop
// ran, and if the client got as far as the main loop, the RunOnce files deleted (they have been
// applied, and Config.wtf now holds their values) and each stage torn down from the last.
// ref: FUN_140216880
void Sub140216880() {
    ClientReleaseOnQuit();
    if (g_running != 0) {
        Sub14022A040();
        Sub14123ABD0();
    }
    if ((g_startupFlags & 2) == 0) {
        return;
    }
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
    std::sort(runOnce.m_data, runOnce.m_data + runOnce.m_size, [](const blz::string& a, const blz::string& b) {
        return SStrCmpUTF8I(a.m_data, b.m_data, 0x7FFFFFFF) > 0;
    });
    for (blz::string* name = runOnce.m_data; name != runOnce.m_data + runOnce.m_size; name++) {
        CVarDeleteFile(name->m_data);
    }
    for (blz::string* name = runOnce.m_data; name != runOnce.m_data + runOnce.m_size; name++) {
        if (!name->IsInline()) {
            SMemFree(name->m_data, ".D", -6, 0);
        }
    }
    runOnce.m_size = 0;
    runOnce.destroy();
    Sub1402293F0();
    Sub1402285E0();
    Sub140227810();
    Sub140226550();
    Sub140226260();
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:GenFault=GenFaultMissing")
#pragma comment(linker, "/alternatename:FUN_140217000=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_140226240=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_140226260=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_140226dd0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_140228610=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_140229300=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_140229860=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141282a80=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141359ec0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141359fd0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141487dc0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141487ed0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141502660=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141502b40=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141503240=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141503670=GenForwardMissing")
// ---- end of link fallbacks ----

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_140211580 = (void*)&GetCommandLineCopy;
extern "C" void* const gen_f_140214990 = (void*)&SetSystemAwake;
extern "C" void* const gen_p_142ddcf50 = (void*)&s_seedA;
extern "C" void* const gen_p_142ddcf80 = (void*)&s_seedB;
extern "C" void* const gen_p_142ddcfc0 = (void*)&s_parentProcessName;
extern "C" void* const gen_p_142ddfa38 = (void*)&s_wowConnectionLog;
extern "C" void* const gen_p_1430a55d8 = (void*)&s_streamingCameraRadius;
extern "C" void* const gen_p_1430a55e8 = (void*)&s_streamingCameraLookAheadTime;
extern "C" void* const gen_p_1430d0278 = (void*)&s_wowConnectionLogCallback;
extern "C" void* const gen_p_1430d2508 = (void*)&s_defaultLogs;
// ---- end of generated globals ----
