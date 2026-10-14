#include "model2/M2Particle.hpp"
#include "client/ClientCVars.hpp"
#include "world/WorldFrame.hpp"
#include "blz/Vector.hpp"
#include "util/Fnv.hpp"
#include "client/GraphicsSettings.hpp"
#include "client/RenderCVars.hpp"
#include "client/ConsoleDevice.hpp"
#include "client/FrameBufferCache.hpp"
#include "client/RenderSettings.hpp"
#include "threadpool/ThreadPool.hpp"
#include "client/CommandLine.hpp"
#include "client/GxWindow.hpp"
#include "client/Locale.hpp"
#include "db/DBClient.hpp"
#include "client/Startup.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/ScriptMethod.hpp"
#include "glue/GlueMgr.hpp"
#include "services/Agent.hpp"
#include "services/ClientServices.hpp"
#include "db/GlobalColor.hpp"
#include "gxu/GxuFont.hpp"
#include "ui/CSimpleTop.hpp"
#include "ui/FrameXML.hpp"
#include "cas/CAS.hpp"
#include "db/ParameterCurve.hpp"
#include "fileservices/AsyncFile.hpp"
#include "console/Command.hpp"
#include "os/Window.hpp"
#include "console/Var.hpp"
#include "os/Os.hpp"
#include "storm/ErrorReport.hpp"
#include "storm/Log.hpp"
#include "shadereffect/ShaderEffectManager.hpp"
#include "sound/SE3.hpp"
#include "sound/SI3.hpp"
#include "texture/Texture.hpp"
#include "ui/FrameTime.hpp"
#include "model2/M2Shared.hpp"
#include "util/Unimplemented.hpp"
#include <windows.h>
#include <malloc.h>
#include <cstdarg>
#include <cstdlib>
#include <windows.h>

extern "C" int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);

// Stage 5 of CommonMain (FUN_14022a260) and what it calls. The engine subsystems it starts after
// the window and device are tagged stubs, called in the reference order, until each is ported.

// the async preload queue
namespace {

uint32_t s_preloadQueued;           // DAT_1430a55f8: the files the preload asked for
uint32_t s_preloadDone;             // DAT_1430a55fc: and those that came back

// Every preload read's done, error and cancel callback: one more is back.
// ref: FUN_141283a70
void PreloadOnRead(void* userData, void* buffer, uint32_t size) {
    (void)userData;
    (void)buffer;
    (void)size;
    s_preloadDone++;
}

// The reference passes the one callback as the error callback too; C++ wants its own type.
void PreloadOnError(void* userData, void* buffer, uint32_t size, const char* error) {
    (void)error;
    PreloadOnRead(userData, buffer, size);
}

}  // namespace

// A file the preload reads ahead when it exists and is not resident yet.
// ref: FUN_141283a80
void PreloadQueueFile(uint32_t fileDataId) {
    if (!cas::CASFileExists(static_cast<int32_t>(fileDataId))) {
        return;
    }
    if (cas::CASFileGetResidency(static_cast<int32_t>(fileDataId), 0, 0) == 2) {
        return;
    }
    AsyncFileReadById(static_cast<int32_t>(fileDataId), 3, nullptr, nullptr, nullptr, nullptr, &PreloadOnRead,
        &PreloadOnError, &PreloadOnRead);
    s_preloadQueued++;
}

// starts the preload; true when it is under way
// ref: FUN_141283af0
bool PreloadStart(void (*callback)(void*), void* param) {
    (void)callback;
    (void)param;
    FROST_UNIMPLEMENTED();
    return true;
}

// DB2 hotfix callbacks
extern "C" unsigned long long FUN_1405705d0();
// translated: FUN_1405705d0
void Sub1405705D0() {
    __try {
        FUN_1405705d0();
    } __except (GenFault(0x1405705d0ull, GetExceptionInformation())) {
    }
}

// the hwDetect defaults (DAT_142e303c0 hook)
extern "C" unsigned long long FUN_14022a8e0();
// translated: FUN_14022a8e0
void HardwareDetectDefaults() {
    __try {
        FUN_14022a8e0();
    } __except (GenFault(0x14022a8e0ull, GetExceptionInformation())) {
    }
}

// registers a product tag
// ref: FUN_141c994e0
void Sub141C994E0(const char* tag) {
    (void)tag;
    FROST_UNIMPLEMENTED();
}

void Sub14068C520() {
    GxuFontRegisterCallbacks();
}

extern "C" unsigned long long FUN_1412a11c0();
// translated: FUN_1412a11c0
void Sub1412A11C0() {
    __try {
        FUN_1412a11c0();
    } __except (GenFault(0x1412a11c0ull, GetExceptionInformation())) {
    }
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

extern "C" unsigned long long FUN_1402f9e30();
// translated: FUN_1402f9e30
void Sub1402F9E30() {
    __try {
        FUN_1402f9e30();
    } __except (GenFault(0x1402f9e30ull, GetExceptionInformation())) {
    }
}


extern "C" unsigned long long FUN_1412834a0(unsigned long long);
// translated: FUN_1412834a0
void Sub1412834A0(uint32_t a1) {
    __try {
        FUN_1412834a0((unsigned long long)a1);
    } __except (GenFault(0x1412834a0ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141297eb0(unsigned long long);
// translated: FUN_141297eb0
void Sub141297EB0(uint32_t a1) {
    __try {
        FUN_141297eb0((unsigned long long)a1);
    } __except (GenFault(0x141297eb0ull, GetExceptionInformation())) {
    }
}

// told the window size
void Sub14068B4C0(const uint64_t* size) {
    GxuFontInitialize(reinterpret_cast<const uint32_t*>(size));
}

extern "C" unsigned long long FUN_14126ea20();
// translated: FUN_14126ea20
void Sub14126EA20() {
    __try {
        FUN_14126ea20();
    } __except (GenFault(0x14126ea20ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_140326a30();
// translated: FUN_140326a30
void Sub140326A30() {
    __try {
        FUN_140326a30();
    } __except (GenFault(0x140326a30ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141930f00();
// translated: FUN_141930f00
void Sub141930F00() {
    __try {
        FUN_141930f00();
    } __except (GenFault(0x141930f00ull, GetExceptionInformation())) {
    }
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

extern "C" unsigned long long FUN_1412a1d50();
// translated: FUN_1412a1d50
void Sub1412A1D50() {
    __try {
        FUN_1412a1d50();
    } __except (GenFault(0x1412a1d50ull, GetExceptionInformation())) {
    }
}

// The world's startup: its CVars, then its subsystems.
// ref: FUN_14134ed50
void Sub14134ED50(void (*callback)()) {
    (void)callback;
    GraphicsRegisterCVars();
    AmbientOcclusionRegisterCVars();
    // FUN_1413b9e40 (the world's callbacks), the world option defaults (DAT_1430ab9c0..), and
    // the subsystems around the particles' (liquids, shadows, the sky, ...) are not ported.
    FROST_UNIMPLEMENTED();
    ParticleSystemInitialize();
}

// the world's loading callbacks
// ref: FUN_1413527a0
void Sub1413527A0(void* const* functions) {
    (void)functions;
    FROST_UNIMPLEMENTED();
}

namespace {

const char s_optionPageTag[] = ".P6AXW4OPTION_PAGE__@Z";
// What each options page's Defaults button runs (DAT_142e30208).
blz::vector<void (*)(uint32_t), s_optionPageTag> s_optionPageResets;  // DAT_142e30208

}

// ref: FUN_140327010
void Sub140327010(void (*callback)(uint32_t)) {
    s_optionPageResets.push_back(callback);
}

namespace {

void ResetCVar(const char* name) {
    if (CVar* cvar = CVarLookup(t2util::ConstFnvHashStringI(name))) {
        CVarResetToDefault(cvar);
    }
}

// The advanced page's settings (PTR_s_textureFilteringMode_142619af0).
const char* const s_advancedOptions[] = { "textureFilteringMode", "MSAAQuality", "FFXAntiAliasingMode", "MSAAAlphaTest",
    "ResampleQuality", "ResampleSharpness", "gxAdapter", "physicsLevel", "maxFPS", "maxFPSBk", "targetFPS", "gamma",
    "brightness", "contrast", "clipcursor", "shadowRt", "RAIDshadowRt", "SSAOType" };

}

// An options page back to its defaults: the graphics page (0) its quality settings, every
// quality group and the render scale; the advanced page (1) its settings, and the graphics API
// the best this machine has; the interface page (2) the UI scale. The first start does all
// three, with the defaults the hardware detection chose.
// ref: FUN_14022ad90
void Sub14022AD90(uint32_t page) {
    if (page == 0) {
        for (const char* name : { "windowResizeLock", "graphicsQuality", "RAIDgraphicsQuality" }) {
            ResetCVar(name);
        }
        GraphicsResetQualityGroups(true);
        GraphicsResetQualityGroups(false);
        ResetCVar("RenderScale");
    } else if (page == 1) {
        for (const char* name : s_advancedOptions) {
            ResetCVar(name);
        }
        CVar* api = CVarLookup(t2util::ConstFnvHashStringI("gxapi"));
        if (api == nullptr) {
            return;
        }
        CVarSet(api, GxApiName(GxGetBestApi()), true, false, false, true);
    } else if (page == 2) {
        ResetCVar("uiScale");
        ResetCVar("useUiScale");
    }
}



extern "C" unsigned long long FUN_14125f550();
// translated: FUN_14125f550
void Sub14125F550() {
    __try {
        FUN_14125f550();
    } __except (GenFault(0x14125f550ull, GetExceptionInformation())) {
    }
}

// ---- Sub14023DBD0's callees ----------------------------------------------------------------

static uint8_t s_flag142de2990;      // DAT_142de2990
static CWorldScene* s_worldScene;   // DAT_142ddcf58: the world's scene
static uint32_t* s_worldParams;      // DAT_142ddcf60: its view's flags

void Sub140238BC0() {
    ErrorDisplayInitialize();
}

// The reloadUI and TakeScreenshot console commands.
extern "C" unsigned long long FUN_14024d040();
// translated: FUN_14024d040
void Sub14024D040() {
    __try {
        FUN_14024d040();
    } __except (GenFault(0x14024d040ull, GetExceptionInformation())) {
    }
}

// Opens Logs/Client.log ("Client Initialize.").
extern "C" unsigned long long FUN_1402165b0();
// translated: FUN_1402165b0
void Sub1402165B0() {
    __try {
        FUN_1402165b0();
    } __except (GenFault(0x1402165b0ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_14022e810();
// translated: FUN_14022e810
void Sub14022E810() {
    __try {
        FUN_14022e810();
    } __except (GenFault(0x14022e810ull, GetExceptionInformation())) {
    }
}

// Binds the client's script API into a new Lua state (and releases it with null).
// ref: FUN_14023da20
void ClientScriptStateHook(lua_State* L) {
    if (L != nullptr) {
        ScriptMethod::CreateTrampoline(L);
        return;
    }
    ScriptMethod::ReleaseTrampoline();
}

// ref: FUN_14230b600
void Sub14230B600() {
    ParameterCurveInitialize();
}

// ref: FUN_1412e59c0
void Sub1412E59C0(uint32_t value) {
    SI3Init(value != 0);
}

extern "C" unsigned long long FUN_14192c8c0();
// translated: FUN_14192c8c0
void Sub14192C8C0() {
    __try {
        FUN_14192c8c0();
    } __except (GenFault(0x14192c8c0ull, GetExceptionInformation())) {
    }
}

// The ImpactModelCollision* CVars.
extern "C" unsigned long long FUN_141897e90();
// translated: FUN_141897e90
void Sub141897E90() {
    __try {
        FUN_141897e90();
    } __except (GenFault(0x141897e90ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141658730();
// translated: FUN_141658730
void Sub141658730() {
    __try {
        FUN_141658730();
    } __except (GenFault(0x141658730ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141671c80(unsigned long long, unsigned long long);
// A realm message's payload into the state at 0x14311bf50 (not ported: both run translated).
// translated: FUN_141671c80
void Sub141671C80(const void* message) {
    __try {
        FUN_141671c80(0x14311bf50ull, reinterpret_cast<uintptr_t>(message));
    } __except (GenFault(0x141671c80ull, GetExceptionInformation())) {
    }
}

// The realm connection's message callback.
// ref: 0x141672070 (reached only through the pointer)
void Sub141672070(const void* message) {
    Sub141671C80(message);
}

// ref: FUN_14156d0e0
void Sub14156D0E0(void* services) {
    (void)services;
    FROST_UNIMPLEMENTED();
}

extern "C" unsigned long long FUN_140330ba0();
// translated: FUN_140330ba0
void Sub140330BA0() {
    __try {
        FUN_140330ba0();
    } __except (GenFault(0x140330ba0ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_14033fa30();
// translated: FUN_14033fa30
void Sub14033FA30() {
    __try {
        FUN_14033fa30();
    } __except (GenFault(0x14033fa30ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_14165a560();
// translated: FUN_14165a560
void Sub14165A560() {
    __try {
        FUN_14165a560();
    } __except (GenFault(0x14165a560ull, GetExceptionInformation())) {
    }
}

// The cursor freelook CVars.
extern "C" unsigned long long FUN_1415242c0();
// translated: FUN_1415242c0
void Sub1415242C0() {
    __try {
        FUN_1415242c0();
    } __except (GenFault(0x1415242c0ull, GetExceptionInformation())) {
    }
}

// ref: FUN_14024a690
void Sub14024A690() {
    s_flag142de2990 = 1;
}

extern "C" unsigned long long FUN_141caf660();
// translated: FUN_141caf660
void Sub141CAF660() {
    __try {
        FUN_141caf660();
    } __except (GenFault(0x141caf660ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141745f00();
// translated: FUN_141745f00
void Sub141745F00() {
    __try {
        FUN_141745f00();
    } __except (GenFault(0x141745f00ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141719bf0();
// translated: FUN_141719bf0
void Sub141719BF0() {
    __try {
        FUN_141719bf0();
    } __except (GenFault(0x141719bf0ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141974d80();
// translated: FUN_141974d80
void Sub141974D80() {
    __try {
        FUN_141974d80();
    } __except (GenFault(0x141974d80ull, GetExceptionInformation())) {
    }
}

// The startup worker thread (blz::thread over FUN_14023deb0, with an auto-reset event).
void StartWorkerThread() {
    FROST_UNIMPLEMENTED();
}

// The end of stage 5: every client subsystem past the device, then the glue.
// ref: FUN_14023dbd0
void Sub14023DBD0() {
    // FUN_14029fae0: anti-tamper (Aegis), not ported
    MessageInitialize();
    Sub140238BC0();
    Sub14024D040();
    ClientRegisterCVars();
    GxuFontSetColorNameLookup(GlobalColorLookup);
    Sub1402165B0();
    Sub14022E810();
    FrameScript_SetStateHook(ClientScriptStateHook);
    FrameScript_Initialize(false);
    Sub14230B600();
    Sub1412E59C0(0);
    Sub14192C8C0();
    FrameXML_RegisterDefault();
    FrameXML_RegisterCVars();
    Sub141897E90();
    // three script-function slots (0x14309e1e0, 0x14309e1a0, 0x14309f2e8) get their handlers
    Sub141658730();
    RealmConnectionSetServerTimeCallback(&ClientServicesSetServerTime);
    RealmConnectionSetMessageCallback(&Sub141672070);
    Sub14156D0E0(ClientServicesGet());
    Sub140330BA0();
    Sub14033FA30();
    FrameTime_Initialize();
    Sub14165A560();
    s_worldScene = WorldSceneCreate(8, 6, false);
    s_worldParams = WorldSceneGetParams(s_worldScene);
    Sub1415242C0();
    Sub14024A690();
    Sub141CAF660();
    GlueMgrStartup();
    Sub141745F00();
    Sub141719BF0();
    Sub141974D80();
    // FUN_14026bea0 (Sub14026BEA0) returns true and does nothing else
    StartWorkerThread();
}

extern "C" unsigned long long FUN_1412a99a0();
// translated: FUN_1412a99a0
void Sub1412A99A0() {
    __try {
        FUN_1412a99a0();
    } __except (GenFault(0x1412a99a0ull, GetExceptionInformation())) {
    }
}

// ref: FUN_1402d0b60
void Sub1402D0B60(void (*callback)()) {
    (void)callback;
    FROST_UNIMPLEMENTED();
}

extern "C" unsigned long long FUN_1402d0f60();
// translated: FUN_1402d0f60
void Sub1402D0F60() {
    __try {
        FUN_1402d0f60();
    } __except (GenFault(0x1402d0f60ull, GetExceptionInformation())) {
    }
}


// Kiosk mode (0x24 bytes): on or off, the session's length and how much of it has passed, its
// stage (1 running, 2 over), and two warnings, each a time left and whether it is still due
// (5 minutes and 1 minute).
struct KIOSKSTATE {
    uint8_t m_enabled;              // 0x00
    uint8_t m_pad01[7];
    float m_length;                 // 0x08
    float m_elapsed;                // 0x0C
    uint8_t m_stage;                // 0x10
    uint8_t m_pad11[3];
    struct {
        float m_left;
        uint8_t m_due;
        uint8_t m_pad[3];
    } m_warnings[2];                // 0x14
};
static_assert(sizeof(KIOSKSTATE) == 0x24, "KIOSKSTATE is 0x24 bytes");

KIOSKSTATE s_kiosk = { 0, {}, 0.0f, 0.0f, 0, {}, { { 300.0f, 0, {} }, { 60.0f, 0, {} } } };  // DAT_142d2a4e0

// ref: FUN_14192fa70
void* Sub14192FA70() {
    return &s_kiosk;
}

// Whether kiosk mode is on.
// ref: FUN_1406a8850
bool Sub1406A8850(void* a1) {
    return static_cast<KIOSKSTATE*>(a1)->m_enabled != 0;
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

extern "C" unsigned long long FUN_14135a290();
// translated: FUN_14135a290
void Sub14135A290() {
    __try {
        FUN_14135a290();
    } __except (GenFault(0x14135a290ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_140222a60();
// translated: FUN_140222a60
void Sub140222A60() {
    __try {
        FUN_140222a60();
    } __except (GenFault(0x140222a60ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_14021f090();
// translated: FUN_14021f090
void Sub14021F090() {
    __try {
        FUN_14021f090();
    } __except (GenFault(0x14021f090ull, GetExceptionInformation())) {
    }
}

void Sub1415642A0() {
    AgentUpdate();
}

// ref: FUN_14230b040
void Sub14230B040() {
    ParameterCurveUpdate();
}

// ref: FUN_1412e1c00
void Sub1412E1C00() {
    SI3CheckPendingReset();
}

// ref: FUN_1412cab50
void Sub1412CAB50(uint64_t a1) {
    (void)a1;
    SE3Update();
}

// ref: FUN_1412e1fb0
void Sub1412E1FB0() {
    SI3Update();
}

extern "C" unsigned long long FUN_1402f4d00(unsigned long long);
// translated: FUN_1402f4d00
void Sub1402F4D00(uint64_t a1) {
    __try {
        FUN_1402f4d00((unsigned long long)a1);
    } __except (GenFault(0x1402f4d00ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_14021c040();
// translated: FUN_14021c040
void Sub14021C040() {
    __try {
        FUN_14021c040();
    } __except (GenFault(0x14021c040ull, GetExceptionInformation())) {
    }
}

// Gx
// ref: FUN_14228c670
void Sub14228C670() {
    GxFrameUpdate();
}

// ref: FUN_14192fcb0
void Sub14192FCB0(void* a1) {
    (void)a1;
    FROST_UNIMPLEMENTED();
}

namespace {

uint8_t s_flag142cfdb74;            // DAT_142cfdb74
uint8_t s_compositorEnabled;        // DAT_142de194c
uint32_t s_compositorMode;          // DAT_142de1970: the screenshot size override, {u16 w, u16 h}
uint8_t s_captureFlip;              // DAT_142e96f72
uint8_t s_captureRead;              // DAT_142e96f50
uint8_t s_flag142e96e01;            // DAT_142e96e01
uint8_t s_flag142e96e02;            // DAT_142e96e02
uint8_t s_flag142e96e03;            // DAT_142e96e03
uint8_t s_flag14323f218;            // DAT_14323f218

}

// ref: FUN_1413528e0
void Sub1413528E0(bool a1) {
    s_flag142cfdb74 = a1;
}

// glue
extern "C" unsigned long long FUN_1412ee600();
// translated: FUN_1412ee600
void Sub1412EE600() {
    __try {
        FUN_1412ee600();
    } __except (GenFault(0x1412ee600ull, GetExceptionInformation())) {
    }
}

// glue
extern "C" unsigned long long FUN_1412ea4a0();
// translated: FUN_1412ea4a0
void Sub1412EA4A0() {
    __try {
        FUN_1412ea4a0();
    } __except (GenFault(0x1412ea4a0ull, GetExceptionInformation())) {
    }
}

// glue
extern "C" unsigned long long FUN_1412e8790();
// translated: FUN_1412e8790
void Sub1412E8790() {
    __try {
        FUN_1412e8790();
    } __except (GenFault(0x1412e8790ull, GetExceptionInformation())) {
    }
}

// glue
extern "C" unsigned long long FUN_14167c520();
// translated: FUN_14167c520
void Sub14167C520() {
    __try {
        FUN_14167c520();
    } __except (GenFault(0x14167c520ull, GetExceptionInformation())) {
    }
}

// glue
extern "C" unsigned long long FUN_141687ed0();
// translated: FUN_141687ed0
void Sub141687ED0() {
    __try {
        FUN_141687ed0();
    } __except (GenFault(0x141687ed0ull, GetExceptionInformation())) {
    }
}

// glue
extern "C" unsigned long long FUN_141691b00();
// translated: FUN_141691b00
void Sub141691B00() {
    __try {
        FUN_141691b00();
    } __except (GenFault(0x141691b00ull, GetExceptionInformation())) {
    }
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

// ref: FUN_1402372d0
void ClientOnMouseCapture(uint32_t x, uint32_t y) {
    (void)x;
    (void)y;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1405f04b0
void UiInputIme(void* input, const void* event) {
    (void)input;
    (void)event;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141237fa0
void OsWindowFocus(void* window) {
    (void)window;
    FROST_UNIMPLEMENTED();
}

namespace {

uint8_t s_gamepadInBackground;  // DAT_1430d13da

}

// The gamepad hears that the window lost (true) or took the focus.
// ref: FUN_14152d7d0
void GamepadSetInBackground(bool background) {
    s_gamepadInBackground = background ? 1 : 0;
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

extern "C" unsigned long long FUN_14228ac80(unsigned long long);
// translated: FUN_14228ac80
void GxSetMouseCursorActive(uint32_t active) {
    __try {
        FUN_14228ac80((unsigned long long)active);
    } __except (GenFault(0x14228ac80ull, GetExceptionInformation())) {
    }
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

// A render settings validator that only runs FUN_140216870 and accepts.
// ref: FUN_14022a240
bool OnRenderSettingsValidate(const RENDERSETTINGS* current, RENDERSETTINGS* pending, void* param) {
    (void)current;
    (void)pending;
    (void)param;
    Sub140216870(0);
    return true;
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

// The async reader's error handler: a full disk (tact's error 10) is fatal.
// ref: FUN_140229cb0
bool OnAsyncReadError(int32_t kind, uint32_t code, const char* text, CAsyncObject* object) {
    (void)text;
    (void)object;
    if (kind == 0 && code == 10) {
        ErrorDisplayCode(0x8510006a, "", LocaleFromSystem(0), false);  // FUN_140217000
        OsTerminateProcess();
    }
    return false;
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
// Whether the console may open now (the game's check, set by FUN_140326b40), and whether it is
// open.
bool (*s_consoleMayToggle)();  // DAT_142e301b8
uint8_t s_consoleShown;  // DAT_142e301c9

}  // namespace

// The console's hot key while the console is enabled and may open: it opens or closes the
// console (the IME follows it), and the key is the console's.
// ref: FUN_140326920
bool ConsoleIsToggleKey(uint32_t key) {
    if (s_consoleEnabledFromCommandLine == 0 || static_cast<int32_t>(key) != ConsoleGetHotKey()
        || s_consoleMayToggle == nullptr || !s_consoleMayToggle()) {
        return false;
    }
    bool shown = s_consoleShown == 0;
    if ((s_consoleShown != 0) != shown) {
        s_consoleShown = shown ? 1 : 0;
        OsImeEnable(!shown);
    }
    return true;
}

namespace {

// ref: FUN_140326080
void ConsoleSetEnabledFromCommandLine(bool enabled) {
    s_consoleEnabledFromCommandLine = enabled;
}

// Whether the command line enabled the console.
// ref: FUN_140326070
bool ConsoleIsEnabled() {
    return s_consoleEnabledFromCommandLine != 0;
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
    RenderSettingsAddValidator(OnRenderSettingsValidate, nullptr);
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
    ClientDBInitialize();
    HotfixAddInitialListener(&HotfixListener140229B10);
    HotfixAddInitialListener(&HotfixListener140229BF0);
    Sub1405705D0();
    ConsoleSetEnabledFromCommandLine(CommandLineHasOption(0x100));
    blz::string title;
    GetWindowTitle(&title);
    ConsoleDeviceSetHardwareDetectHook(&HardwareDetectDefaults);
    if (!ConsoleDeviceInitialize(title.m_data)) {
        DbCloseHotfixes();
        ClientDBShutdown();
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
    FrameRateRegisterCVars();
    GxAddDeviceCreatedCallback(OnDeviceCreatedClient, nullptr);
    GxAddDeviceDestroyedCallback(OnDeviceDestroyedClient, nullptr);
    s_deviceCallbacksAdded = 1;
    ParticleSystemCreatePools();
    RenderSettingsInitialize();
    TextureInitialize();
    Sub1402F9E30();
    ResampleRegisterCVars();
    FrameBufferCacheInitialize();
    Sub1412834A0(0xbaa0c);
    Sub141297EB0(0xb27ad);
    GXSIZE resolution;
    WindowGetResolution(&resolution);
    uint64_t size = static_cast<uint64_t>(resolution.m_height) << 32 | resolution.m_width;
    Sub14068B4C0(&size);
    Sub14126EA20();
    Sub140326A30();
    Sub141930F00();
    TextureSetAtlasEnabled(true);
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
    ShaderEffectManagerInitialize();
    Sub14134ED50(&OnModelHook140229CA0);
    static void* const s_loadingCallbacks[] = { reinterpret_cast<void*>(&LoadingCallback14022DB40),
        reinterpret_cast<void*>(&LoadingCallback14022DEC0), reinterpret_cast<void*>(&LoadingCallback14022F7C0),
        reinterpret_cast<void*>(&LoadingCallback14022E190), reinterpret_cast<void*>(&LoadingCallback14022E010),
        reinterpret_cast<void*>(&LoadingCallback14022F660) };
    Sub1413527A0(s_loadingCallbacks);
    Sub140327010(Sub14022AD90);
    if (ConsoleDeviceHwDetected() || ConsoleDeviceApiWasReset()) {
        for (uint32_t i = 0; i < 3; i++) {
            Sub14022AD90(i);
        }
    }
    M2Initialize(m2Flags | 0x53, 0, 0);
    Sub14125F550();
    FullScreenEffectsRegisterCVars();
    WorldInitialize();
    Sub14023DBD0();
    g_clientInitFlags |= 0x1000;
    Sub1412A99A0();
    Sub1402D0B60(nullptr);
    Sub1402D0F60();
    s_clientStarted = 1;
    if (!ConsoleIsEnabled() && !Sub1406A8850(Sub14192FA70())) {
        ConsoleCommandRegister("enable", CommandConsoleEnable, 2,
            "Enable the console with \"/console enable\" in the chat window. This is the same as launching the client with -console",
            0);
    }
    ConsoleCommandRegister("SetGraphicsQuality", CommandSetGraphicsQuality, 1, nullptr, 0);
    ConsoleCommandRegister("SetGraphicsQualityRAID", CommandSetGraphicsQualityRaid, 1, nullptr, 0);
    return true;
}

// ---- the frame's callees, ported one by one -------------------------------------------------

// 1 .. 0x4000 each, or 0 for the window's size.
// ref: FUN_14023a3f0
void ClientSetScreenshotSize(const uint16_t* size) {
    if (size[0] == 0 || size[1] == 0) {
        s_compositorMode = 0;
        return;
    }
    uint32_t width = size[0] > 0x4000 ? 0x4000 : size[0];
    uint32_t height = size[1] > 0x4000 ? 0x4000 : size[1];
    s_compositorMode = (height << 16) | width;
}

// ref: FUN_14134f980
bool Sub14134F980() {
    return s_flag142cfdb74 != 0;
}

// A compositor (stereo) path's flags (DAT_142de194c, DAT_142de1970); neither is set yet.
// ref: FUN_140239e90
bool Sub140239E90() {
    return s_compositorEnabled != 0 && s_compositorMode != 0;
}

// ref: FUN_140239e10
uint32_t* Sub140239E10(uint32_t* out, const uint32_t* size) {
    (void)size;
    FROST_UNIMPLEMENTED();
    *out = *size;
    return out;
}

// Screenshot / video capture flags (DAT_142e96f72, DAT_142e96f50).
// ref: FUN_1405dc320
bool Sub1405DC320() {
    return s_captureFlip != 0;
}

// ref: FUN_1405dbde0
bool Sub1405DBDE0() {
    return s_captureRead != 0;
}

// ref: FUN_14026bea0
bool Sub14026BEA0() {
    return true;
}

extern "C" unsigned long long FUN_1413544d0();
// translated: FUN_1413544d0
void Sub1413544D0() {
    __try {
        FUN_1413544d0();
    } __except (GenFault(0x1413544d0ull, GetExceptionInformation())) {
    }
}

// The UI's frame: its time step, then FUN_1417d8ab0, FUN_1417ce430 and FUN_141b1b1b0.
extern "C" unsigned long long FUN_14167ba90(unsigned long long, unsigned long long);
// translated: FUN_14167ba90
void Sub14167BA90(uint64_t a1, uint64_t a2) {
    __try {
        FUN_14167ba90((unsigned long long)a1, (unsigned long long)a2);
    } __except (GenFault(0x14167ba90ull, GetExceptionInformation())) {
    }
}

// The UI root's render (slot 0xf8 of DAT_142e979d8).
void UiRender(void* ui) {
    static_cast<CSimpleTop*>(ui)->Render();
}

void Sub1405F27E0(bool a1) {
    UiTrimUnneededTextures(a1);
}

void Sub140238D50() {
    ErrorDisplayRender();
}

// The loading screen's draw.
// ref: FUN_14022ee80
void Sub14022EE80(GxRenderContext* context, GXRENDERTARGETS* targets, bool a3) {
    (void)context;
    (void)targets;
    (void)a3;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14192ffc0
void Sub14192FFC0(GxRenderContext* context, GXRENDERTARGETS* targets) {
    (void)context;
    (void)targets;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141350fc0
void Sub141350FC0() {
    if (s_flag142e96e02 == 0 && s_flag142e96e03 != 0) {
        InterlockedExchange8(reinterpret_cast<volatile char*>(&s_flag142e96e01), 1);
        s_flag142e96e03 = 0;
    }
}

// ref: FUN_141c9cbd0
bool Sub141C9CBD0() {
    return s_flag14323f218 != 0;
}

// ref: FUN_140239ec0
void Sub140239EC0(FRAMEBUFFER* frame) {
    (void)frame;
    FROST_UNIMPLEMENTED();
}

void Sub14068BC30() {
    GxuFontUpdate();
}

void Sub14068AAE0() {
    GxuFontReleaseIdleFonts();
}

// The frame stats' bookkeeping and the dynamic render scale.
// ref: FUN_14023d170
void Sub14023D170(GXFRAMESTATS* stats) {
    (void)stats;
    FROST_UNIMPLEMENTED();
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142ddcf58 = (void*)&s_worldScene;
extern "C" void* const gen_p_142de194c = (void*)&s_compositorEnabled;
extern "C" void* const gen_p_142e301b8 = (void*)&s_consoleMayToggle;
extern "C" void* const gen_p_142e301c9 = (void*)&s_consoleShown;
extern "C" void* const gen_p_142e8e0e9 = (void*)&s_hotfixesClosed;
extern "C" void* const gen_p_142e8e218 = (void*)&s_dbHotfixLog;
extern "C" void* const gen_p_142e8e220 = (void*)&s_dbHotfixRecordLog;
// ---- end of generated globals ----
