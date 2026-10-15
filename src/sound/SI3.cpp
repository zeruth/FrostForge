// SI3 (WoW/Source/SoundInterface/SI3.cpp): starting and stopping the sound system for the game.

#include "sound/SI3.hpp"

#include <cstring>

#include "console/Var.hpp"
#include "db/DB2Tables.hpp"
#include "db/WowClientDB2.hpp"
#include "framescript/FrameScript.hpp"
#include "os/Os.hpp"
#include "services/ClientServices.hpp"
#include "sound/SE3.hpp"
#include "ui/FrameTime.hpp"
#include "util/Unimplemented.hpp"
#include <windows.h>

extern "C" int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);

bool CommandLineHasOption(uint32_t option);

namespace {

const char s_file[] = "D:\\BuildServer\\A\\work-git\\wow\\WoW\\Source\\SoundInterface\\SI3.cpp";

uint8_t s_initialized;      // DAT_142cfd5a0
uint8_t s_1430a99f0;        // DAT_1430a99f0
uint8_t s_restartPending;   // DAT_1430a9892
int32_t s_restartCount;     // DAT_1430a9900
uint8_t s_1430a99fc;        // DAT_1430a99fc
uint32_t s_updateTime;      // DAT_1430a9904
// Two game states the sound kits can be kept out of (set by FUN_1412fae00 and its sibling;
// both clear at the glue screens).
uint8_t s_kitState0;        // DAT_1430a99e0
uint8_t s_kitState2;        // DAT_1430a99e2

// A failed start retries with the most compatible settings: software channels, stereo, 256
// channels, 32 software ones, 44.1 kHz.
// ref: FUN_1412e6520
void InitFailed(SE3CONFIG* config) {
    SE3Log(0x39a, s_file, 0,
        " - ################################################################################################");
    SE3Log(0x39b, s_file, 0,
        " - #### Failed to init with user settings. Attempting to init using the most compatible settings...");
    SE3Log(0x39c, s_file, 0,
        " - ################################################################################################");
    config->m_useHardware = 0;
    config->m_enableReverb = 0;
    config->m_forceStereo = 1;
    config->m_09 = 1;
    config->m_maxChannels = 0x100;
    config->m_numChannels = 0x20;
    config->m_sampleRate = 44100;
    config->m_dspBufferSize = 0;
}

// LAB_14021f080
void DevicesChanged() {
    ClientSetPendingSoundReset();
}

// The game time in milliseconds of a day.
// ref: FUN_1412e1b90
int32_t GetGameTime() {
    FROST_UNIMPLEMENTED();  // FUN_1416510a0(&DAT_142d1a040): the world clock's time of day
    return 0;
}

// LAB_1412e1b80
void Slot110() {
    FROST_UNIMPLEMENTED();  // FUN_1412f8360
}

// The engine asks it with the kit's definition.
// ref: FUN_1405dce80
float One(void* def) {
    (void)def;
    return 1.0f;
}

// ref: FUN_1412e6500
void Slot170(int32_t handle) {
    (void)handle;
    FROST_UNIMPLEMENTED();  // FUN_1412f9e20, FUN_1412fcd00, FUN_1412fcbe0: the zone music and ambience
}

// The emitter grid cell of a world position: the map chunk and its sub-cell, in quarters.
// ref: FUN_1412e62d0
void EmitterGridCell(void* position, uint32_t* x, uint32_t* y) {
    (void)position;
    FROST_UNIMPLEMENTED();  // FUN_14134e2e0: the world position's chunk
    *x = 0;
    *y = 0;
}

// Once the engine is up: the async loader's sound callback, when the loader exists.
// ref: FUN_1412e6b40
void InitDone() {
    FROST_UNIMPLEMENTED();  // FUN_1418ea470(0) / FUN_1418e6720(FUN_1412e64c0, 0)
}

// Whether a sound kit's flags keep it out (SoundKit field 3, against two game states).
// ref: FUN_1412e6b70
bool FilterKit(void* kit) {
    if (kit == nullptr) {
        return false;
    }
    bool local;
    auto record = static_cast<const uint8_t*>(
        g_soundKitDB.GetRecord(*reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(kit) + 0x2c), false, &local));
    if (record == nullptr) {
        return false;
    }
    if (s_kitState2 != 0) {
        if ((DB2FieldU16(record, 3, 0, &g_soundKitDB) & 0x100) != 0) {
            return true;
        }
    } else if ((DB2FieldU16(record, 3, 0, &g_soundKitDB) & 0x8000) != 0) {
        return true;
    }
    if (s_kitState0 != 0) {
        return (DB2FieldU16(record, 3, 0, &g_soundKitDB) & 0x400) != 0;
    }
    DB2FieldU16(record, 3, 0, &g_soundKitDB);
    return false;
}

// FUN_14026bea0: the engine asks it with the kit's definition and the play's parameters.
bool Accept(void* def, void* params) {
    (void)def;
    (void)params;
    return true;
}

// The configuration from the CVars.
// ref: FUN_1412e65a0
void Configure(SE3CONFIG* config) {
    SE3Log(0x338, s_file, 0, "=> SI3 Init");
    SE3Log(0x339, s_file, 0, "=> WoW Version %s (%s) %s", "3.4.3", "54261", "Apr 12 2024");
    SI3RegisterCVars();
    if (CommandLineHasOption(0x80)) {
        SE3Log(0x340, s_file, 0, "Sound Disabled by command line. Init Silence:");
        config->m_silent = 1;
        return;
    }
    SE3SetSlot(&config->m_onInitFailed, &InitFailed);
    SE3SetSlot(&config->m_onDevicesChanged, &DevicesChanged);
    config->m_sfxVolume = g_soundSFXVolume->m_floatValue;
    config->m_musicVolume = g_soundMusicVolume->m_floatValue;
    config->m_ambienceVolume = g_soundAmbienceVolume->m_floatValue;
    config->m_dialogVolume = g_soundDialogVolume->m_floatValue;
    config->m_masterVolume = g_soundMasterVolume->m_floatValue;
    config->m_enableSFX = g_soundEnableSFX->m_intValue != 0;
    config->m_enableMusic = g_soundEnableMusic->m_intValue != 0;
    config->m_enableAmbience = g_soundEnableAmbience->m_intValue != 0;
    config->m_enableAllSound = g_soundEnableAllSound->m_intValue != 0;
    config->m_useHardware = 0;
    config->m_enableDialog = g_soundEnableDialog->m_intValue != 0;
    config->m_maxChannels = 0x200;
    config->m_enableReverb = g_soundEnableReverb->m_intValue != 0;
    config->m_numChannels = g_soundNumChannels->m_intValue;
    config->m_mixMode2 = g_soundEnableMixMode2->m_intValue;
    config->m_dspBufferSize = g_soundDSPBufferSize->m_intValue;
    config->m_outputDriverIndex = g_soundOutputDriverIndex->m_intValue;
    config->m_outputDriverName = g_soundOutputDriverName->m_stringValue.GetString();
    SE3SetSlot(&config->m_getGameTime, &GetGameTime);
    SE3SetSlot(&config->m_slot110, &Slot110);
    SE3SetSlot(&config->m_slot130, &One);
    SE3SetSlot(&config->m_slot150, &One);
    config->m_2c = 86400000;
    FrameScript_GetTextCopy("SYSTEM_DEFAULT", config->m_defaultDriverName, 0x40, -1, 0, false);
    SE3SetSlot(&config->m_emitterGridCell, &EmitterGridCell);
    config->m_forceStereo = 0;
    config->m_09 = 1;
    SE3SetSlot(&config->m_slot170, &Slot170);
    SE3SetSlot(&config->m_onInitDone, &InitDone);
    SE3SetSlot(&config->m_slot1f0, &FilterKit);
    SE3SetSlot(&config->m_slot210, &Accept);
    if (g_soundOutputSampleRate != nullptr) {
        int32_t rate = g_soundOutputSampleRate->m_intValue;
        if (rate != 22050 && rate != 44100 && rate != 48000) {
            config->m_sampleRate = 44100;
            return;
        }
        config->m_sampleRate = rate;
    }
}

void ConfigureInvoke(SE3CONFIG* config, void* storage) {
    (void)storage;
    Configure(config);
}

// The DSP effects chains (the death knight's and the like).
// ref: FUN_14130fe30
void InitDSPEffects() {
    FROST_UNIMPLEMENTED();
}

// What a session adds once the system is up: the zone tables and the DB2 watchers.
// ref: FUN_1412e5a70
void InitSession() {
    FROST_UNIMPLEMENTED();  // FUN_141310ed0, FUN_14130e020, FUN_14130dd00, FUN_1412f8960 and three DB2 patch callbacks
}

// The engine start with the configure callback (FUN_1412e59c0's body, inlined again in
// FUN_1412e1c00).
int32_t Start(bool restart) {
    s_1430a99f0 = 0;
    blz::function_storage configure;
    configure.m_invoke = reinterpret_cast<void*>(&ConfigureInvoke);
    configure.m_manager = &blz::function_trivial_manager;
    std::memset(configure.m_storage, 0, sizeof(configure.m_storage));
    int32_t result = SE3Init(restart, &configure);
    if (result == 0) {
        if (g_soundEnableDSPEffects != nullptr && g_soundEnableDSPEffects->m_intValue != 0) {
            InitDSPEffects();
        }
        s_initialized = 1;
        SE3Log(0x2a2, s_file, 0, "=> SI3 Init Complete.");
        SE3Log(0x2a3, s_file, 0, " ");
    }
    return result;
}

// The SI3 lists the frame walks before the engine's: the sound kits waiting on their files.
// ref: FUN_141311de0
void UpdateKitLoads() {
    FROST_UNIMPLEMENTED();
}

}

extern "C" unsigned long long FUN_1419fa0c0(unsigned long long);
// The voice chat's audio on a focus change (DAT_1431f6738).
// translated: FUN_1419fa0c0
void Sub1419FA0C0(bool active) {
    __try {
        FUN_1419fa0c0(active ? 1 : 0);
    } __except (GenFault(0x1419fa0c0ull, GetExceptionInformation())) {
    }
}

// The window took or lost the focus: the engine stays audible in the background only when
// Sound_EnableSoundWhenGameIsInBG says so, and the voice chat's audio follows.
// ref: FUN_1412e5ae0
void SI3OnFocus(bool active) {
    if (active) {
        SE3SetActive(true);
        Sub1419FA0C0(true);
        return;
    }
    SE3SetActive(g_soundEnableInBackground != nullptr && g_soundEnableInBackground->m_intValue != 0);
    Sub1419FA0C0(false);
}

// ref: FUN_1412e59c0
void SI3Init(bool restart) {
    Start(restart);
}

// ref: FUN_1412e6a50
void SI3Shutdown(bool restart) {
    if (restart) {
        s_restartCount++;
    }
    SE3Log(0x2ae, s_file, 0, " ");
    SE3Log(0x2af, s_file, 0, "=> Shutting Down SI3");
    // FUN_14057ae20 over AreaTable, WeaponImpactSounds and WMOAreaTable: the patch watchers
    // FUN_1412e5a70 adds with a session (never added without one).
    FROST_UNIMPLEMENTED();  // FUN_1413118e0, FUN_141310280, FUN_1412f7e40, FUN_1412f7a50: the SI3 tables
    SE3Log(0x2c0, s_file, 0, "=> Shutting Down SI3 COMPLETE.");
    SE3Log(0x2c1, s_file, 0, " ");
    SE3Shutdown(restart);
}

// The tail of FUN_1412e1fb0 (its head is Aegis-wrapped).
void SI3Update() {
    if (g_se3ShuttingDown != 0) {
        return;
    }
    static bool s_timeStarted = false;
    if (!s_timeStarted) {
        s_updateTime = FrameTime_GetTime();
        s_timeStarted = true;
    }
    UpdateKitLoads();
    if (s_restartPending != 0) {
        SI3Shutdown(true);
        OsSleepNanoseconds(200 * 1000000ll);    // FUN_141238570(200)
        SI3Init(true);
        if (ClientServicesIsSessionUp()) {
            InitSession();
        }
        s_restartPending = 0;
    }
    s_1430a99fc = 0;
}

// ref: FUN_1412e6b30
void SI3RequestRestart() {
    s_restartPending = 1;
}

// The device list changed: when the driver in use is gone (or the default moved), the engine
// restarts on the default driver.
// ref: FUN_1412e1c00
void SI3CheckPendingReset() {
    // The device-change watchers (DAT_142cfd2b0) are told before and after; none are added.
    int32_t count = SE3GetNumOutputDrivers();
    char name[0x100];
    for (int32_t i = 0; i < count; i++) {
        SE3GetOutputDriverName(i, name, 0x100, nullptr);
        if (std::strcmp(SE3GetActiveDriverName(), name) == 0) {
            if (g_soundOutputDriverIndex == nullptr || g_soundOutputDriverIndex->m_intValue != 0) {
                return;
            }
            char first[0x100];
            SE3GetOutputDriverName(1, first, 0x100, nullptr);
            if (std::strcmp(SE3GetActiveDriverName(), first) == 0) {
                return;
            }
            break;
        }
    }
    if (g_soundOutputDriverIndex != nullptr) {
        CVarSet(g_soundOutputDriverIndex, "0", true, false, false, true);
    }
    SI3Shutdown(0 < count);
    Start(0 < count);
    if (ClientServicesIsSessionUp()) {
        InitSession();
    }
}

// The flag stays the image's: the reverb code that reads it is translated.
// ref: FUN_141312260
void SI3SetReverbEnabled(bool enabled) {
    *reinterpret_cast<uint8_t*>(0x1430aa028ull) = enabled;
}

// ref: FUN_141312590
void SI3UpdateReverb() {
    FROST_UNIMPLEMENTED();
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_1430a99e0 = (void*)&s_kitState0;
extern "C" void* const gen_p_1430a99e2 = (void*)&s_kitState2;
extern "C" void* const gen_p_1430a99f0 = (void*)&s_1430a99f0;
// ---- end of generated globals ----
