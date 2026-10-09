#include "client/ConsoleDevice.hpp"
#include "client/CommandLine.hpp"
#include "client/GxWindow.hpp"
#include "client/Locale.hpp"
#include "client/Startup.hpp"
#include "cas/CAS.hpp"
#include "console/Command.hpp"
#include "console/Var.hpp"
#include "os/Os.hpp"
#include "storm/ErrorReport.hpp"
#include "storm/Log.hpp"
#include "util/Unimplemented.hpp"
#include <windows.h>
#include <malloc.h>
#include <cstdarg>
#include <cstdlib>

// Stage 5 of CommonMain (FUN_14022a260) and what it calls. The engine subsystems it starts after
// the window and device are tagged stubs, called in the reference order, until each is ported.

// registers a shutdown hook (vector at DAT_1430a5880)
// ref: FUN_14129d420
void Sub14129D420(void (*callback)(), void* param) {
    (void)callback;
    (void)param;
    FROST_UNIMPLEMENTED();
}

// Engine/Source/ThreadPool
// ref: FUN_142359e10
void ThreadPoolInitialize(uint32_t a1) {
    (void)a1;
    FROST_UNIMPLEMENTED();
}

// the async file reader's thread settings
// ref: FUN_1405da950
void AsyncReadConfigure(int32_t threadSleep, int32_t handlerTimeout) {
    (void)threadSleep;
    (void)handlerTimeout;
    FROST_UNIMPLEMENTED();
}

// enableBGDL
// ref: FUN_1405d7c60
void AsyncSetBackgroundDownload(int32_t enabled) {
    (void)enabled;
    FROST_UNIMPLEMENTED();
}

// sortDiskReads
// ref: FUN_1405db9f0
void AsyncSetSortReads(bool sort) {
    (void)sort;
    FROST_UNIMPLEMENTED();
}

// the async preload queue
// ref: FUN_141283a80
void PreloadQueueFile(uint32_t fileDataId) {
    (void)fileDataId;
    FROST_UNIMPLEMENTED();
}

// starts the preload; true when it is under way
// ref: FUN_141283af0
bool PreloadStart(void (*callback)(void*), void* param) {
    (void)callback;
    (void)param;
    FROST_UNIMPLEMENTED();
    return true;
}

// ref: FUN_140351bf0
void Sub140351BF0() {
    FROST_UNIMPLEMENTED();
}

// DB2 hotfix callbacks
// ref: FUN_1405705d0
void Sub1405705D0() {
    FROST_UNIMPLEMENTED();
}

// the hwDetect defaults (DAT_142e303c0 hook)
// ref: FUN_14022a8e0
void HardwareDetectDefaults() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140352060
void Sub140352060() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1405da0e0
void AsyncReadShutdown() {
    FROST_UNIMPLEMENTED();
}

// Engine/Source/ThreadPool
// ref: FUN_14235a350
void ThreadPoolShutdown() {
    FROST_UNIMPLEMENTED();
}

// registers a product tag
// ref: FUN_141c994e0
void Sub141C994E0(const char* tag) {
    (void)tag;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14068c520
void Sub14068C520() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412a11c0
void Sub1412A11C0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14023caf0
void Sub14023CAF0() {
    FROST_UNIMPLEMENTED();
}

// client objects made with the device
// ref: FUN_14022a020
void OnDeviceCreatedClient(void* param) {
    (void)param;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14022a030
void OnDeviceDestroyedClient(void* param) {
    (void)param;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14124c270
void Sub14124C270() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14129d9f0
void Sub14129D9F0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141280a70
void Sub141280A70() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1402f9e30
void Sub1402F9E30() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412a2bf0
void Sub1412A2BF0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14129fcb0
void Sub14129FCB0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412834a0
void Sub1412834A0(uint32_t a1) {
    (void)a1;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141297eb0
void Sub141297EB0(uint32_t a1) {
    (void)a1;
    FROST_UNIMPLEMENTED();
}

// told the window size
// ref: FUN_14068b4c0
void Sub14068B4C0(const uint64_t* size) {
    (void)size;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14126ea20
void Sub14126EA20() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140326a30
void Sub140326A30() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141930f00
void Sub141930F00() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141280060
void Sub141280060(int32_t a1) {
    (void)a1;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140229b60
bool OnM2ForceAdditiveParticleSortChanged(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    (void)newValue;
    (void)userData;
    FROST_UNIMPLEMENTED();
    return true;
}

// ref: FUN_1412a1d50
void Sub1412A1D50() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412a3990
void Sub1412A3990() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14134ed50
void Sub14134ED50(void (*callback)()) {
    (void)callback;
    FROST_UNIMPLEMENTED();
}

// the world's loading callbacks
// ref: FUN_1413527a0
void Sub1413527A0(void* const* functions) {
    (void)functions;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140327010
void Sub140327010(void (*callback)(uint32_t)) {
    (void)callback;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14022ad90
void Sub14022AD90(uint32_t a1) {
    (void)a1;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140328760
bool Sub140328760() {
    FROST_UNIMPLEMENTED();
    return false;
}

// ref: FUN_140329220
bool Sub140329220() {
    FROST_UNIMPLEMENTED();
    return false;
}

// M2 setup
// ref: FUN_1411974e0
void Sub1411974E0(uint32_t flags, int32_t a2, int32_t a3) {
    (void)flags;
    (void)a2;
    (void)a3;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14125f550
void Sub14125F550() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1405dcf20
void Sub1405DCF20() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14134ec50
void Sub14134EC50() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14023dbd0
void Sub14023DBD0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412a99a0
void Sub1412A99A0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1402d0b60
void Sub1402D0B60(void (*callback)()) {
    (void)callback;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1402d0f60
void Sub1402D0F60() {
    FROST_UNIMPLEMENTED();
}

// whether the console is enabled
// ref: FUN_140326070
bool Sub140326070() {
    FROST_UNIMPLEMENTED();
    return false;
}

// ref: FUN_14192fa70
void* Sub14192FA70() {
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_1406a8850
bool Sub1406A8850(void* a1) {
    (void)a1;
    FROST_UNIMPLEMENTED();
    return false;
}

// ref: FUN_140229ce0
int32_t CommandConsoleEnable(const char* command, const char* arguments) {
    (void)command;
    (void)arguments;
    FROST_UNIMPLEMENTED();
    return 1;
}

// ref: FUN_140229d20
int32_t CommandSetGraphicsQuality(const char* command, const char* arguments) {
    (void)command;
    (void)arguments;
    FROST_UNIMPLEMENTED();
    return 1;
}

// ref: FUN_140229d80
int32_t CommandSetGraphicsQualityRaid(const char* command, const char* arguments) {
    (void)command;
    (void)arguments;
    FROST_UNIMPLEMENTED();
    return 1;
}

// Gx
// ref: FUN_14228b8c0
void GxFrameSetPhase(uint8_t phase) {
    (void)phase;
    FROST_UNIMPLEMENTED();
}

// Gx
// ref: FUN_14228b3a0
void GxFrameBegin() {
    FROST_UNIMPLEMENTED();
}

// Gx
// ref: FUN_14228c1a0
void GxFrameWorkFlush(uint8_t a1) {
    (void)a1;
    FROST_UNIMPLEMENTED();
}

// Gx: the frame-rate limit wait
// ref: FUN_14228aa60
bool GxFrameLimit(uint32_t timeout) {
    (void)timeout;
    FROST_UNIMPLEMENTED();
    return false;
}

// the world clock
// ref: FUN_14065fce0
void Sub14065FCE0(uint32_t elapsed, uint32_t now) {
    (void)elapsed;
    (void)now;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14065fd40
void Sub14065FD40() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1405db0f0
void AsyncReadPoll() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14135a290
void Sub14135A290() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140222a60
void Sub140222A60() {
    FROST_UNIMPLEMENTED();
}

// CAS
// ref: FUN_1402cf8d0
void Sub1402CF8D0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14021f090
void Sub14021F090() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1415642a0
void Sub1415642A0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14230b040
void Sub14230B040() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412e1c00
void Sub1412E1C00() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140216570
uint64_t Sub140216570() {
    FROST_UNIMPLEMENTED();
    return 0;
}

// ref: FUN_1412cab50
void Sub1412CAB50(uint64_t a1) {
    (void)a1;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412e1fb0
void Sub1412E1FB0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1402f4d00
void Sub1402F4D00(uint64_t a1) {
    (void)a1;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14021c040
void Sub14021C040() {
    FROST_UNIMPLEMENTED();
}

// Gx
// ref: FUN_14228c670
void Sub14228C670() {
    FROST_UNIMPLEMENTED();
}

// the window
// ref: FUN_1414f8e20
void Sub1414F8E20() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141350fd0
void Sub141350FD0(bool a1) {
    (void)a1;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14192fcb0
void Sub14192FCB0(void* a1) {
    (void)a1;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1413528e0
void Sub1413528E0(bool a1) {
    (void)a1;
    FROST_UNIMPLEMENTED();
}

// glue
// ref: FUN_1412ee600
void Sub1412EE600() {
    FROST_UNIMPLEMENTED();
}

// glue
// ref: FUN_1412ea4a0
void Sub1412EA4A0() {
    FROST_UNIMPLEMENTED();
}

// glue
// ref: FUN_1412e8790
void Sub1412E8790() {
    FROST_UNIMPLEMENTED();
}

// glue
// ref: FUN_14167c520
void Sub14167C520() {
    FROST_UNIMPLEMENTED();
}

// glue
// ref: FUN_141687ed0
void Sub141687ED0() {
    FROST_UNIMPLEMENTED();
}

// glue
// ref: FUN_141691b00
void Sub141691B00() {
    FROST_UNIMPLEMENTED();
}

// the UI update
// ref: FUN_1405f0be0
void Sub1405F0BE0(void* a1, float elapsed) {
    (void)a1;
    (void)elapsed;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141525060
void Sub141525060(void* a1, float elapsed) {
    (void)a1;
    (void)elapsed;
    FROST_UNIMPLEMENTED();
}

// game state 2
// ref: FUN_140234df0
void Sub140234DF0() {
    FROST_UNIMPLEMENTED();
}

// game state 3
// ref: FUN_140234ed0
void Sub140234ED0() {
    FROST_UNIMPLEMENTED();
}

// game state 4
// ref: FUN_140235040
void Sub140235040() {
    FROST_UNIMPLEMENTED();
}

// game state 5
// ref: FUN_140234080
void Sub140234080() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140570630
void Sub140570630() {
    FROST_UNIMPLEMENTED();
}

// Gx
// ref: FUN_14228cb00
void Sub14228CB00() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1402372d0
void ClientOnMouseCapture(uint32_t x, uint32_t y) {
    (void)x;
    (void)y;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1405efc40
void UiInputChar(void* input, uint32_t character) {
    (void)input;
    (void)character;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1405f04b0
void UiInputIme(void* input, const void* event) {
    (void)input;
    (void)event;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140237660
void ClientOnFocus(uint32_t active) {
    (void)active;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1402382b0
void ClientKeyBinding(uint32_t key, uint32_t down, char* handled) {
    (void)key;
    (void)down;
    (void)handled;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140326920
bool ConsoleIsToggleKey(uint32_t key) {
    (void)key;
    FROST_UNIMPLEMENTED();
    return false;
}

// ref: FUN_141237fa0
void OsWindowFocus(void* window) {
    (void)window;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1405f0680
void UiInputKeyDown(void* input, const void* event) {
    (void)input;
    (void)event;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1405f0890
void UiInputKeyDownBound(void* input, const void* event) {
    (void)input;
    (void)event;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140237b00
void ClientOnKeyUp(const uint32_t* params, uint32_t time) {
    (void)params;
    (void)time;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14152cf90
uint64_t Sub14152CF90(uint32_t a1) {
    (void)a1;
    FROST_UNIMPLEMENTED();
    return 0;
}

// ref: FUN_1405effd0
void UiInputGamepadDown(void* input, const void* event) {
    (void)input;
    (void)event;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1405f01e0
void UiInputGamepadUp(void* input, const void* event) {
    (void)input;
    (void)event;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1405f0270
void UiInputGamepadStick(void* input, const void* event) {
    (void)input;
    (void)event;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14123ab20
void OsGetClientRect(int32_t* rect) {
    (void)rect;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14228ac80
void GxSetMouseCursorActive(uint32_t active) {
    (void)active;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1405f12f0
void UiInputMouseDown(void* input, const void* event) {
    (void)input;
    (void)event;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1405f1530
void UiInputMouseMove(void* input, const void* event, int32_t a3) {
    (void)input;
    (void)event;
    (void)a3;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1405f1ab0
void UiInputMouseWheel(void* input, const void* event) {
    (void)input;
    (void)event;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1405f1860
void UiInputMouseDelta(void* input, const void* event) {
    (void)input;
    (void)event;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140237c80
void ClientOnMouseUp(uint32_t button, uint32_t x, uint32_t y, uint32_t a4, uint32_t time, bool a6) {
    (void)button;
    (void)x;
    (void)y;
    (void)a4;
    (void)time;
    (void)a6;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1405eff80
void UiInputDisplayChanged() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14065fb00
void Sub14065FB00() {
    FROST_UNIMPLEMENTED();
}


// The callbacks stage 5 hands out by address; their bodies come with their subsystems.
// ref: FUN_140216870
void Sub140216870(uint8_t a1) {
    (void)a1;
    FROST_UNIMPLEMENTED();  // DAT_142ddcf99, then Gx FUN_142288660
}

// ref: FUN_140229bd0
void OnPreloadDone(void* param) {
    (void)param;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140229b10
void HotfixListener140229B10(int32_t event) {
    (void)event;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140229bf0
void HotfixListener140229BF0(int32_t event) {
    (void)event;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140229ca0
void OnModelHook140229CA0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14022db40
void LoadingCallback14022DB40() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14022dec0
void LoadingCallback14022DEC0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14022f7c0
void LoadingCallback14022F7C0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14022e190
void LoadingCallback14022E190() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14022e010
void LoadingCallback14022E010() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14022f660
void LoadingCallback14022F660() {
    FROST_UNIMPLEMENTED();
}

namespace {

CVar* s_asyncThreadSleep;           // DAT_142de0280
CVar* s_asyncHandlerTimeout;        // DAT_142de0288
CVar* s_enableBackgroundDownload;   // DAT_142de0290
CVar* s_sortDiskReads;              // DAT_142de0298
CVar* s_m2UseThreads;               // DAT_142de0268
CVar* s_m2ForceAdditiveParticleSort;  // DAT_142de0270
CVar* s_m2UseInstancing;            // DAT_142de0278
uint8_t s_clientStarted;            // DAT_142de02a0
uint8_t s_deviceCallbacksAdded;     // DAT_142de02a8
HSLOG s_hotfixLog;                  // DAT_142de02b0
int32_t s_splitMode;                // DAT_142dde820

// The window title: GlobalStrings 1 ("World of Warcraft").
// ref: FUN_1402164b0
void GetWindowTitle(blz::string* title) {
    const char* text = "World of Warcraft";  // FUN_14057b640(GlobalStrings, 1) when the table is loaded
    title->assign(text);
}

// ref: FUN_14022a240
void OnShutdownHook() {
    Sub140216870(0);
}

// ref: FUN_140229ad0
bool OnAsyncReadOptionChanged(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    (void)userData;
    int radix = 10;
    if (newValue != nullptr && newValue[0] == '0' && ((newValue[1] + 0xa8u) & 0xdf) == 0) {
        radix = 0x10;
    }
    std::strtol(newValue, nullptr, radix);
    return true;
}

// ref: FUN_140229c00
bool OnEnableBackgroundDownloadChanged(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    (void)userData;
    int radix = 10;
    if (newValue != nullptr && newValue[0] == '0' && ((newValue[1] + 0xa8u) & 0xdf) == 0) {
        radix = 0x10;
    }
    AsyncSetBackgroundDownload(static_cast<int32_t>(std::strtol(newValue, nullptr, radix)));
    return true;
}

// ref: FUN_140229c50
bool OnSortDiskReadsChanged(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    (void)userData;
    int radix = 10;
    if (newValue != nullptr && newValue[0] == '0' && ((newValue[1] + 0xa8u) & 0xdf) == 0) {
        radix = 0x10;
    }
    AsyncSetSortReads(std::strtol(newValue, nullptr, radix) != 0);
    return true;
}

// The async reader reports a fatal read error (code 10 from the main thread).
// ref: FUN_140229cb0
bool OnAsyncReadError(int32_t thread, int32_t error) {
    if (thread == 0 && error == 10) {
        ErrorDisplayCode(0x8510006a, "", LocaleFromSystem(0), false);  // FUN_140217000
        OsTerminateProcess();
    }
    return false;
}

void (*s_asyncErrorHandler)();      // DAT_142e96e60

// ref: FUN_1405db9c0
void* AsyncSetErrorHandler(void* handler) {
    void* previous = reinterpret_cast<void*>(s_asyncErrorHandler);
    s_asyncErrorHandler = reinterpret_cast<void (*)()>(handler);
    return previous;
}

void* s_asyncHooks[2];              // DAT_142e96d38, DAT_142e96dc8

// ref: FUN_1405d8df0
void AsyncSetHooks(void* a1, void* a2) {
    s_asyncHooks[0] = a1;
    s_asyncHooks[1] = a2;
}

// ref: FUN_140219ae0
void SetSplitMode(int32_t mode) {
    s_splitMode = mode;
}

// The stage-5 gate: queue the preload list (fdid 0x1849e6, a run of file data ids) and the
// locale's files, then start the preload.
// ref: FUN_14022afd0
bool StartupPreload() {
    cas::CASFILE* file = nullptr;
    if (!cas::CASFileOpenById(0x1849e6, &file, 0)) {
        return false;
    }
    uint64_t size = cas::CASFileGetSize(file);
    uint32_t count = static_cast<uint32_t>(size >> 2) & 0x3fffffff;
    auto ids = static_cast<uint32_t*>(_alloca((static_cast<size_t>(static_cast<uint32_t>(size)) + 0xf) & ~static_cast<size_t>(0xf)));
    bool read = cas::CASFileRead(file, ids, static_cast<uint32_t>(size));
    cas::CASFileClose(&file);
    if (!read) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        PreloadQueueFile(ids[i]);
    }
    // The locale's own files come from a DB2 (DAT_142e94dc0, walked with the locale's bit,
    // FUN_1405d5790); that table is not loaded yet, as when the reference runs without it.
    return PreloadStart(OnPreloadDone, nullptr);
}

// ref: FUN_14022b170
void HotfixLog(const char* format, ...) {
    va_list args;
    va_start(args, format);
    SLogWriteV(s_hotfixLog, format, args);
    va_end(args);
}

// ref: FUN_14022b140
void HotfixLogRecord(int32_t action, const char* table, int32_t record, const char* status) {
    SLogWrite(s_hotfixLog, "\t%d Table %s RecID %d %s", action, table, record, status);
}

void (*s_dbErrorLog)(const char*, ...);       // DAT_142e8e2c0
void (*s_dbHotfixLog)(const char*, ...);      // DAT_142e8e218
void (*s_dbHotfixRecordLog)(int32_t, const char*, int32_t, const char*);  // DAT_142e8e220

// ref: FUN_140581990
void DbSetErrorLog(void (*log)(const char*, ...)) {
    s_dbErrorLog = log;
}

// ref: FUN_1405742b0
void DbSetHotfixLog(void (*log)(const char*, ...)) {
    s_dbHotfixLog = log;
}

// ref: FUN_1405742c0
void DbSetHotfixRecordLog(void (*log)(int32_t, const char*, int32_t, const char*)) {
    s_dbHotfixRecordLog = log;
}

uint8_t s_hotfixesClosed;           // DAT_142e8e0e9

// ref: FUN_1405742d0
void DbCloseHotfixes() {
    s_hotfixesClosed = 1;
    FROST_UNIMPLEMENTED();  // the four DB2 hotfix hooks Sub1405705D0 installs (DAT_14309ec10 and the rest)
}

uint32_t s_consoleEnabledFromCommandLine;  // DAT_142e301b4

// ref: FUN_140326080
void ConsoleSetEnabledFromCommandLine(bool enabled) {
    s_consoleEnabledFromCommandLine = enabled;
}

// The splash window stage 1 shows while loading (stage 1 does not make it yet).
HWND s_splashWindow;                // DAT_142dde7f8
void* s_splashImage;                // DAT_142dde810
HCURSOR s_splashCursor;             // DAT_142dde818

// ref: FUN_140219a00
void SplashDestroy() {
    DestroyWindow(s_splashWindow);
    if (s_splashCursor != nullptr) {
        DestroyCursor(s_splashCursor);
        s_splashCursor = nullptr;
    }
    s_splashWindow = nullptr;
    s_splashImage = nullptr;
}

}

// Stage 5: the async reader and preload, the hotfix log, the console device (window and
// device), then the engine's subsystems. False when the client cannot start.
// ref: FUN_14022a260
bool Sub14022A260() {
    if (CommandLineHasOption(0x40)) {
        GxSetAllowSoftwareGpu(true);
    }
    Sub14129D420(&OnShutdownHook, nullptr);
    ThreadPoolInitialize(1);
    s_asyncThreadSleep = CVarRegister("asyncThreadSleep", "Engine option: Async read thread sleep", 1, "0",
        OnAsyncReadOptionChanged, 0, false, nullptr, false);
    s_asyncHandlerTimeout = CVarRegister("asyncHandlerTimeout", "Engine option: Async read main thread timeout", 1,
        "100", OnAsyncReadOptionChanged, 0, false, nullptr, false);
    s_enableBackgroundDownload = CVarRegister("enableBGDL", "Background Download (on async net thread) Enabled", 1, "1",
        OnEnableBackgroundDownloadChanged, 0, false, nullptr, false);
    s_sortDiskReads = CVarRegister("sortDiskReads", "Sort async disk reads to minimize seeks (requires restart)", 1, "0",
        OnSortDiskReadsChanged, 0, false, nullptr, false);
    AsyncReadConfigure(s_asyncThreadSleep->m_intValue, s_asyncHandlerTimeout->m_intValue);
    AsyncSetErrorHandler(reinterpret_cast<void*>(&OnAsyncReadError));
    AsyncSetHooks(nullptr, nullptr);
    SetSplitMode(2);
    if (!StartupPreload()) {
        ThreadPoolShutdown();
        AsyncReadShutdown();
        ClientRequestQuit();
        return false;
    }
    g_clientInitFlags |= 0x400;
    SplashDestroy();
    g_clientTeardownFlags |= 0x400;
    OsCreateDirectory("Logs", false, false);
    SLogCreate("Logs\\Hotfix.log", 0, &s_hotfixLog);
    HotfixLog("---- Startup ----");
    DbSetErrorLog(HotfixLog);
    DbSetHotfixLog(HotfixLog);
    DbSetHotfixRecordLog(HotfixLogRecord);
    Sub140351BF0();
    HotfixAddInitialListener(&HotfixListener140229B10);
    HotfixAddInitialListener(&HotfixListener140229BF0);
    Sub1405705D0();
    ConsoleSetEnabledFromCommandLine(CommandLineHasOption(0x100));
    blz::string title;
    GetWindowTitle(&title);
    ConsoleDeviceSetHardwareDetectHook(&HardwareDetectDefaults);
    if (!ConsoleDeviceInitialize(title.m_data)) {
        DbCloseHotfixes();
        Sub140352060();
        HotfixLog("---- Shutdown ----");
        SLogClose(s_hotfixLog);
        AsyncReadShutdown();
        ThreadPoolShutdown();
        ClientRequestQuit();
        return false;
    }
    g_clientInitFlags |= 0x800;
    Sub141C994E0("Wrath");
    Sub141C994E0("WOTLKC");
    Sub14068C520();
    Sub1412A11C0();
    Sub14023CAF0();
    GxAddDeviceCreatedCallback(OnDeviceCreatedClient, nullptr);
    GxAddDeviceDestroyedCallback(OnDeviceDestroyedClient, nullptr);
    s_deviceCallbacksAdded = 1;
    Sub14124C270();
    Sub14129D9F0();
    Sub141280A70();
    Sub1402F9E30();
    Sub1412A2BF0();
    Sub14129FCB0();
    Sub1412834A0(0xbaa0c);
    Sub141297EB0(0xb27ad);
    GXSIZE resolution;
    WindowGetResolution(&resolution);
    uint64_t size = static_cast<uint64_t>(resolution.m_height) << 32 | resolution.m_width;
    Sub14068B4C0(&size);
    Sub14126EA20();
    Sub140326A30();
    Sub141930F00();
    Sub141280060(1);
    s_m2UseThreads = CVarRegister("M2UseThreads", "multithread model animations", 0, "1",
        static_cast<CVARCALLBACK>(nullptr), 1, false, nullptr, false);
    s_m2ForceAdditiveParticleSort = CVarRegister("M2ForceAdditiveParticleSort",
        "force all particles to sort as though they were additive", 0, "0", OnM2ForceAdditiveParticleSortChanged, 1,
        false, nullptr, false);
    s_m2UseInstancing = CVarRegister("M2UseInstancing", "use hardware instancing", 0, "1",
        static_cast<CVARCALLBACK>(nullptr), 1, false, nullptr, false);
    uint32_t m2Flags = s_m2UseThreads->m_intValue == 0 ? 0x200 : 0x204;
    if (s_m2ForceAdditiveParticleSort->m_intValue != 0) {
        m2Flags |= 0x80;
    }
    if (s_m2UseInstancing->m_intValue != 0) {
        m2Flags |= 0x100;
    }
    Sub1412A1D50();
    Sub1412A3990();
    Sub14134ED50(&OnModelHook140229CA0);
    static void* const s_loadingCallbacks[] = { reinterpret_cast<void*>(&LoadingCallback14022DB40),
        reinterpret_cast<void*>(&LoadingCallback14022DEC0), reinterpret_cast<void*>(&LoadingCallback14022F7C0),
        reinterpret_cast<void*>(&LoadingCallback14022E190), reinterpret_cast<void*>(&LoadingCallback14022E010),
        reinterpret_cast<void*>(&LoadingCallback14022F660) };
    Sub1413527A0(s_loadingCallbacks);
    Sub140327010(Sub14022AD90);
    if (Sub140328760() || Sub140329220()) {
        for (uint32_t i = 0; i < 3; i++) {
            Sub14022AD90(i);
        }
    }
    Sub1411974E0(m2Flags | 0x53, 0, 0);
    Sub14125F550();
    Sub1405DCF20();
    Sub14134EC50();
    Sub14023DBD0();
    g_clientInitFlags |= 0x1000;
    Sub1412A99A0();
    Sub1402D0B60(nullptr);
    Sub1402D0F60();
    s_clientStarted = 1;
    if (!Sub140326070() && !Sub1406A8850(Sub14192FA70())) {
        ConsoleCommandRegister("enable", CommandConsoleEnable, 2,
            "Enable the console with \"/console enable\" in the chat window. This is the same as launching the client with -console",
            0);
    }
    ConsoleCommandRegister("SetGraphicsQuality", CommandSetGraphicsQuality, 1, nullptr, 0);
    ConsoleCommandRegister("SetGraphicsQualityRAID", CommandSetGraphicsQualityRaid, 1, nullptr, 0);
    return true;
}
