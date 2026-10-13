// SE3 (Engine/Source/SoundEngine/SE3.cpp): the FMOD system's life, Logs/Sound.log, the output
// drivers, and the frame's update.

#include "sound/SE3.hpp"

#include "sound/SE3Internal.hpp"
#include "sound/SE3Kit.hpp"

#include <cfloat>
#include <cstring>
#include <new>
#include <float.h>

#include "base/ObjectAlloc.hpp"
#include "console/Var.hpp"
#include "fmod.hpp"
#include "fmod_errors.h"
#include "os/Os.hpp"
#include "os/Timer.hpp"
#include "storm/Log.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "ui/FrameTime.hpp"

SE3CONFIG g_se3Config;
uint8_t s_1430a9538;
TObjectAllocHeap* g_soundKitDefHeap;
TObjectAllocHeap* g_soundKitHeap;
FMOD::System* g_fmodSystem;
int32_t g_fmodOutputType = FMOD_OUTPUTTYPE_MAX;
volatile uint8_t g_se3ShuttingDown;

namespace {

const char s_file[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\SoundEngine\\SE3.cpp";

HSLOG s_log;                        // DAT_1430a9710
uint32_t s_logFlags;                // DAT_1430a9718
int32_t s_errorCount;               // DAT_1430a971c
char s_driverName[0x40];            // DAT_1430a9480 the configured default driver's name
char s_activeDriverName[0x104];     // DAT_1430a9580 the driver in use
uint8_t s_initialized;              // DAT_142cfca08
float s_globalVolume = 1.0f;        // DAT_142cfca04
int32_t s_1430a9720;                // DAT_1430a9720
CVar* s_variationCap;               // DAT_1430a94c8
CVar* s_positionalLowPass;          // DAT_1430a94d0
uint32_t s_lastUpdateTime;          // DAT_1430a9684
int32_t s_cleanupTimer;             // DAT_1430a968c
int32_t s_rangeTimer;               // DAT_1430a9690

// The inline bounded copy: at most size - 1 characters, always terminated.
void CopyString(char* dest, const char* source, uint64_t size) {
    if (size == 0) {
        return;
    }
    uint64_t i = 0;
    for (; i + 1 < size && source[i] != '\0'; i++) {
        dest[i] = source[i];
    }
    dest[i] = '\0';
}

// The speaker mode names (PTR_DAT_142728fa0).
const char* const s_speakerModes[] = {
    "RAW", "MONO", "STEREO", "QUAD", "SURROUND", "5.1", "7.1", "PROLOGIC", "DOLBY5.1", "MYEARS",
};

// The handlers' invokers: the function kept in the storage, called with the arguments
// (LAB_1412d7640, LAB_140215340).
void InvokeCacheEvent(int32_t event, void* object, void* storage) {
    (*static_cast<void (**)(int32_t, void*)>(storage))(event, object);
}

int32_t InvokeDoneLoading(void* object, int32_t result, void* storage) {
    return (*static_cast<int32_t (**)(void*, int32_t)>(storage))(object, result);
}

int32_t CallTime(blz::function_storage* slot) {
    using INVOKE = int32_t (*)(void* storage);
    return reinterpret_cast<INVOKE>(slot->m_invoke)(slot->m_storage);
}

void CallConfig(blz::function_storage* slot, SE3CONFIG* config) {
    using INVOKE = void (*)(SE3CONFIG* config, void* storage);
    reinterpret_cast<INVOKE>(slot->m_invoke)(config, slot->m_storage);
}

void CallVoid(blz::function_storage* slot) {
    using INVOKE = void (*)(void* storage);
    reinterpret_cast<INVOKE>(slot->m_invoke)(slot->m_storage);
}

void ResetSlot(blz::function_storage* slot) {
    slot->m_manager(slot->m_storage, nullptr, 0);
    slot->m_invoke = nullptr;
    slot->m_manager = &blz::function_empty_manager;
}

// FMOD's allocator callbacks (Storm, category 0x16).
// ref: FUN_1412c8e40
void* F_CALLBACK FmodAlloc(unsigned int size, FMOD_MEMORY_TYPE type, const char* source) {
    (void)type;
    (void)source;
    return SMemAlloc(size, 0x16, s_file, SStrHashFnv1a(s_file), 0xad, 0);
}

// ref: FUN_1412c8ec0
void* F_CALLBACK FmodRealloc(void* ptr, unsigned int size, FMOD_MEMORY_TYPE type, const char* source) {
    (void)type;
    (void)source;
    return SMemReAlloc(ptr, size, 0x16, s_file, SStrHashFnv1a(s_file), 0xcd, 0);
}

// LAB_1412c8ea0
void F_CALLBACK FmodFree(void* ptr, FMOD_MEMORY_TYPE type, const char* source) {
    (void)type;
    (void)source;
    SMemFree(ptr, s_file, -2, 0);
}

// The FMOD system's callback: a change to the device list re-reads it and tells the game.
// ref: FUN_1412c8a30
FMOD_RESULT F_CALLBACK SystemCallback(FMOD_SYSTEM* system, FMOD_SYSTEM_CALLBACKTYPE type, void* data1, void* data2) {
    (void)system;
    (void)data1;
    (void)data2;
    if (type == FMOD_SYSTEM_CALLBACKTYPE_DEVICELISTCHANGED) {
        if (g_fmodSystem != nullptr) {
            int32_t count;
            g_fmodSystem->getNumDrivers(&count);
        }
        s_1430a9538 = 0;
        if (g_se3Config.m_onDevicesChanged.m_invoke != nullptr) {
            CallVoid(&g_se3Config.m_onDevicesChanged);
        }
    }
    return FMOD_OK;
}

// Whether a driver's name is `name`.
// ref: FUN_1412c8d40
bool DriverNameIs(const char* name, int32_t index) {
    char driverName[0x200];
    int32_t driver = index < 0 ? 0 : index;
    if (g_fmodSystem != nullptr) {
        if (driver == 0) {
            CopyString(driverName, s_driverName, 0x200);
            if (driverName[0] == '\0') {
                CopyString(driverName, "System Default", 0x200);
            }
        } else {
            g_fmodSystem->getDriverInfo(driver - 1, driverName, 0x200, nullptr);
        }
    }
    return _stricmp(driverName, name) == 0;
}

// Picks the output driver: the configured index when its name still matches, else the driver
// with the configured name, else the system default.
// ref: FUN_1412d3220
int32_t SetOutputDriver(SE3CONFIG* config) {
    int32_t index = config->m_outputDriverIndex;
    const char* name = config->m_outputDriverName;
    int32_t count = 0;
    if (g_fmodSystem != nullptr) {
        g_fmodSystem->getNumDrivers(&count);
        if (count > 0) {
            count++;
        }
    }
    int32_t chosen = index;
    if (index < 0 || count <= index || !DriverNameIs(name, index)) {
        chosen = 0;
        for (int32_t i = 0; i < count; i++) {
            if (i != index && DriverNameIs(name, i)) {
                chosen = i;
                break;
            }
        }
    }
    int32_t driver = chosen != 0 ? chosen - 1 : 0;
    int32_t result = g_fmodSystem->getDriverInfo(driver, s_activeDriverName, 0x104, nullptr);
    if (result != FMOD_OK) {
        SE3Log(0x1460, s_file, result, "Failed to get driver name for driver index %d", driver);
        if (driver == 0) {
            return result;
        }
        SE3Log(0x1465, s_file, result, "Falling back to default sound driver", driver);
        result = g_fmodSystem->getDriverInfo(0, s_activeDriverName, 0x104, nullptr);
        driver = 0;
        if (result != FMOD_OK) {
            return result;
        }
    }
    if (chosen != 0) {
        SE3Log(0x1477, s_file, 0, "** Using User Specified Driver [%d] %s", chosen, s_activeDriverName);
    } else {
        SE3Log(0x1473, s_file, 0, "** Using Driver [%d] System Default (%s)", chosen, s_activeDriverName);
    }
    result = g_fmodSystem->setDriver(driver);
    if (result != FMOD_OK) {
        SE3Log(0x147d, s_file, result, "Error setting output driver index %d", driver, s_activeDriverName);
        if (driver != 0) {
            SE3Log(0x1482, s_file, result, "Falling back to default sound driver", driver, s_activeDriverName);
            result = g_fmodSystem->setDriver(0);
        }
    }
    return result;
}

void ReleaseSystem() {
    if (g_fmodSystem != nullptr) {
        g_fmodSystem->close();
        g_fmodSystem->release();
        g_fmodSystem = nullptr;
    }
    g_fmodOutputType = FMOD_OUTPUTTYPE_MAX;
}

void LogBanner(int32_t line) {
    SE3Log(line, s_file, 0,
        " - ###########################################################################################");
}

}


void SE3SetSlot(blz::function_storage* slot, void* invoke, void* function) {
    slot->m_manager(slot->m_storage, nullptr, 0);
    slot->m_invoke = invoke;
    slot->m_manager = &blz::function_trivial_manager;
    std::memcpy(slot->m_storage, &function, sizeof(function));
}

// ref: FUN_1412d9970
void SE3Log(int32_t line, const char* file, int32_t result, const char* format, ...) {
    if (s_log == nullptr) {
        return;
    }
    if (s_errorCount < 200) {
        if (result != FMOD_OK) {
            SLogWrite(s_log, " -######## FMOD ERROR! (err %d) %s (%s: %d)", result,
                FMOD_ErrorString(static_cast<FMOD_RESULT>(result)), file, line);
            s_errorCount++;
        }
        if (format != nullptr && *format != '\0') {
            va_list args;
            va_start(args, format);
            SLogWriteV(s_log, format, args);
            va_end(args);
        }
    }
    if (s_errorCount == 200) {
        SLogWrite(s_log, "%s", " -######## TOO MANY ERRORS. NO FURTHER ERRORS WILL BE LOGGED.");
        s_errorCount++;
    }
}

// ref: FUN_1412d9900
void SE3LogOpen() {
    OsCreateDirectory("Logs", false, false);
    SLogCreate("Logs\\Sound.log", s_logFlags, &s_log);
    s_logFlags |= 4;
    OsDeleteFile("Logs\\SESound.log");
}

// ref: FUN_1412d9950
void SE3LogClose() {
    SLogFlush(s_log);
    SLogClose(s_log);
}

// ref: FUN_1412d2260
void SE3ConfigDefaults(SE3CONFIG* config) {
    config->m_silent = 0;
    config->m_enableSFX = 1;
    config->m_enableMusic = 1;
    config->m_enableAmbience = 1;
    config->m_enableAllSound = 1;
    config->m_enableDialog = 1;
    config->m_useHardware = 0;
    config->m_outputDriverName = "System Default";
    config->m_mixMode2 = 0;
    config->m_outputDriverIndex = 0;
    config->m_enableReverb = 0;
    config->m_forceStereo = 0;
    config->m_09 = 0;
    config->m_masterVolume = 1.0f;
    config->m_sfxVolume = 1.0f;
    config->m_musicVolume = 1.0f;
    config->m_ambienceVolume = 1.0f;
    config->m_dialogVolume = 1.0f;
    config->m_volume20 = 1.0f;
    config->m_maxChannels = 0x80;
    config->m_numChannels = 4;
    config->m_2c = 0;
    config->m_sampleRate = 44100;
    config->m_dspBufferSize = 4;
    config->m_emitterParam88 = 4;
    config->m_emitterParam8c = 3;
    blz::function_storage* slots[] = {
        &config->m_getTime, &config->m_onInitFailed, &config->m_onDevicesChanged, &config->m_getGameTime,
        &config->m_slot110, &config->m_slot130, &config->m_slot150, &config->m_slot170, &config->m_emitterGridCell,
        &config->m_onInitDone, &config->m_slot1d0, &config->m_slot1f0, &config->m_slot210, &config->m_slot230, &config->m_slot250,
        &config->m_slot270, &config->m_slot290,
    };
    for (blz::function_storage* slot : slots) {
        if (slot->m_manager == nullptr) {
            slot->m_manager = &blz::function_empty_manager;
        }
        ResetSlot(slot);
    }
    CopyString(config->m_defaultDriverName, "System Default", 0x40);
}

// ref: FUN_1412c9520
int32_t SE3GetNumOutputDrivers() {
    int32_t count = 0;
    if (g_fmodSystem == nullptr) {
        return 0;
    }
    g_fmodSystem->getNumDrivers(&count);
    if (count > 0) {
        return count + 1;
    }
    return count;
}

// ref: FUN_1412c9580
bool SE3GetOutputDriverName(int32_t index, char* name, int32_t size, bool* isDefault) {
    int32_t driver = index < 0 ? 0 : index;
    if (isDefault != nullptr) {
        *isDefault = false;
    }
    if (g_fmodSystem == nullptr) {
        return false;
    }
    if (driver == 0) {
        if (isDefault != nullptr) {
            *isDefault = true;
        }
        CopyString(name, s_driverName, size);
        if (*name == '\0' && size != 0) {
            CopyString(name, "System Default", size);
        }
        return true;
    }
    g_fmodSystem->getDriverInfo(driver - 1, name, size, nullptr);
    return true;
}

// ref: FUN_1412c9450
const char* SE3GetActiveDriverName() {
    return s_activeDriverName;
}

// ref: FUN_1412c85f0
FMOD::ChannelGroup* SE3CreateChannelGroup(const char* name) {
    FMOD::ChannelGroup* group = nullptr;
    g_fmodSystem->createChannelGroup(name, &group);
    return group;
}

// ref: FUN_1412d3ad0
void SE3SetReverbProperties(const void* properties) {
    if (g_fmodSystem == nullptr) {
        return;
    }
    g_fmodSystem->setReverbProperties(static_cast<const FMOD_REVERB_PROPERTIES*>(properties));
}

// ref: DAT_142cfca08
bool SE3IsInitialized() {
    return s_initialized != 0;
}

// ref: FUN_1412ca150
float SE3GlobalVolume() {
    return s_globalVolume;
}

int32_t SE3VariationCap() {
    return s_variationCap != nullptr ? s_variationCap->m_intValue : 0x20;
}

bool SE3PositionalLowPassEnabled() {
    return s_positionalLowPass != nullptr && s_positionalLowPass->m_intValue != 0;
}

// ---- start --------------------------------------------------------------------------------

namespace {

// The engine's systems once FMOD is up: the CVars it owns, the object heaps, the buses.
// ref: FUN_1412cb430
void InitSystems(bool restart) {
    (void)restart;
    s_initialized = 1;
    s_1430a9720 = 0;
    g_se3Period = g_se3Config.m_2c;
    if (s_variationCap == nullptr) {
        // FUN_1412d4780: 1 to 64
        s_variationCap = CVarRegister("SoundPerf_VariationCap",
            "Limit sound kit variations to cut down on memory usage and disk thrashing on 32-bit machines", 0, "32",
            [](CVar*, const char*, const char* value, void*) -> bool {
                long cap = std::strtol(value, nullptr,
                    value != nullptr && value[0] == '0' && ((value[1] + 0xa8) & 0xdf) == 0 ? 16 : 10);
                return static_cast<uint64_t>(cap - 1) < 0x40;
            },
            7, false, nullptr, false);
    }
    if (s_positionalLowPass == nullptr) {
        s_positionalLowPass = CVarRegister("Sound_EnablePositionalLowPassFilter",
            "Environmental effect to make sounds duller behind you or far away", 0, "0", nullptr, 7, false, nullptr,
            false);
    }
    g_soundKitDefHeap = new (SMemAlloc(sizeof(TObjectAllocHeap), 0x16, s_file, SStrHashFnv1a(s_file), 0x1139, 0))
        TObjectAllocHeap("SoundKitDefObjectHeap", 0xb0, 0x400, 0x16);
    g_soundKitHeap = new (SMemAlloc(sizeof(TObjectAllocHeap), 0x16, s_file, SStrHashFnv1a(s_file), 0x113a, 0))
        TObjectAllocHeap("SoundKitObjectHeap", 0x3f0, 0x100, 0x16);
    FMOD::Debug_SetLevel(FMOD_DEBUG_LEVEL_NONE);
    int32_t result = g_fmodSystem->setCallback(&SystemCallback);
    SE3Log(0xf00, s_file, result, " - Setting device callback");
    SE3Log(0xf03, s_file, 0, " - Sound Bus Init");
    SE3BusInit(&g_se3Config);
    SE3SuppressorsInit();
    blz::function_storage handler;
    handler.m_invoke = reinterpret_cast<void*>(&InvokeCacheEvent);
    handler.m_manager = &blz::function_trivial_manager;
    void* soundEvent = reinterpret_cast<void*>(&SE3OnCacheEvent);
    std::memcpy(handler.m_storage, &soundEvent, sizeof(soundEvent));
    SE3InstallSoundEndedHandler(&handler);
    handler.m_invoke = reinterpret_cast<void*>(&InvokeDoneLoading);
    handler.m_manager = &blz::function_trivial_manager;
    void* doneLoading = reinterpret_cast<void*>(&SE3OnDoneLoading);
    std::memcpy(handler.m_storage, &doneLoading, sizeof(doneLoading));
    SE3InstallDoneLoadingHandler(&handler);
    if (g_se3Config.m_onInitDone.m_invoke != nullptr) {
        CallVoid(&g_se3Config.m_onInitDone);
    }
}

// The silent fallback: a NOSOUND output, so everything still runs.
// ref: FUN_1412cb770
void InitSilent() {
    g_fmodOutputType = FMOD_OUTPUTTYPE_NOSOUND;
    FMOD::System_Create(&g_fmodSystem);
    g_fmodSystem->setOutput(FMOD_OUTPUTTYPE_NOSOUND);
    SE3Log(0x1123, s_file, 0, " - ##############################");
    SE3Log(0x1124, s_file, 0, " - ### Sound System: SILENT MODE");
    SE3Log(0x1125, s_file, 0, " - ##############################");
    InitSystems(false);
}

// ref: FUN_1412cb800
int32_t InitFmod(bool restart) {
    g_se3ShuttingDown = 0;
    int32_t result = FMOD::Memory_Initialize(nullptr, 0, &FmodAlloc, &FmodRealloc, &FmodFree);
    SE3Log(0xf44, s_file, result, " - FMOD Memory Init");
    if ((result & ~0x20) != 0) {
        return 3;
    }
    if (g_se3Config.m_silent != 0) {
        InitSilent();
        return 0;
    }
    CopyString(s_driverName, g_se3Config.m_defaultDriverName, sizeof(s_driverName));
    g_fmodSystem = nullptr;
    g_fmodOutputType = FMOD_OUTPUTTYPE_NOSOUND;
    result = FMOD::System_Create(&g_fmodSystem);
    SE3Log(0xf60, s_file, result, " - FMOD System Create");
    if (result != FMOD_OK) {
        return 3;
    }
    result = g_fmodSystem->setOutput(FMOD_OUTPUTTYPE_AUTODETECT);
    SE3Log(0xf6a, s_file, result, " - Setting Output Type");
    if (result != FMOD_OK) {
        SE3Log(0xf6e, s_file, result, " - Failed to set output type.");
        return 3;
    }
    int32_t drivers = SE3GetNumOutputDrivers();
    SE3Log(0xf76, s_file, 0, " - %d Output drivers detected", drivers);
    if (drivers == 0) {
        SE3Log(0xf7a, s_file, 0, " - SOUND INIT ERROR: NO SOUND OUTPUT DEVICES DETECTED", 0);
        return 0x14;
    }
    for (int32_t i = 0; i < drivers; i++) {
        char name[0x100];
        if (g_fmodSystem != nullptr) {
            if (i == 0) {
                CopyString(name, s_driverName, sizeof(name));
                if (name[0] == '\0') {
                    CopyString(name, "System Default", sizeof(name));
                }
            } else {
                g_fmodSystem->getDriverInfo(i - 1, name, 0x100, nullptr);
            }
        }
        SE3Log(0xf82, s_file, 0, " --- [%d] %s", i, name);
    }
    result = SetOutputDriver(&g_se3Config);
    int32_t buffers = g_se3Config.m_dspBufferSize;
    if (result != FMOD_OK) {
        SE3Log(0xf8a, s_file, result, " - Failed to set output driver.");
        return 3;
    }
    if (g_se3Config.m_dspBufferSize == 0) {
        SE3Log(0xf98, s_file, 0, " - DSPBufferSize = AUTO DETECT");
    } else {
        SE3Log(0xf94, s_file, 0,
            " - DSPBufferSize = %d [Valid values are 0 = AUTO DETECT (default), 4 through 10]", g_se3Config.m_dspBufferSize);
    }
    FMOD_CAPS caps = 0;
    FMOD_SPEAKERMODE speakerMode;
    int32_t driver = g_se3Config.m_outputDriverIndex != 0 ? g_se3Config.m_outputDriverIndex - 1 : 0;
    if (g_fmodSystem->getDriverCaps(driver, &caps, nullptr, &speakerMode) != FMOD_OK) {
        speakerMode = FMOD_SPEAKERMODE_STEREO;
    }
    bool custom = true;
    if ((caps & FMOD_CAPS_HARDWARE_EMULATED) == 0) {
        if (buffers == 0) {
            buffers = 4;
            custom = false;
        }
    } else {
        LogBanner(0xfa2);
        SE3Log(0xfa3, s_file, 0, " - # NOTICE:   Your Sound Driver is running in EMULATED mode.");
        SE3Log(0xfa4, s_file, 0, " - #           This may lead to poor sound performance.");
        if (buffers == 0) {
            SE3Log(0xfa9, s_file, 0, " - #           Using Max DSP Buffer size (%d) to avoid stuttering.", 10);
            buffers = 10;
        }
        SE3Log(0xfae, s_file, 0,
            " - #           Also in EMULATED mode some speaker configurations like 5.1 may not be available.");
        LogBanner(0xfaf);
    }
    if (custom) {
        LogBanner(0xfb4);
        SE3Log(0xfb5, s_file, 0, " - # NOTICE:   You are using a custom DSP Buffer size");
        SE3Log(0xfb6, s_file, 0, " - #           Higher settings may reduce sound stuttering and crackling,");
        SE3Log(0xfb7, s_file, 0, " - #           but may affect sound timing.");
        SE3Log(0xfb8, s_file, 0, " - #           Do not change this setting from default unless you really need to.");
        LogBanner(0xfb9);
        if (static_cast<uint32_t>(buffers - 4) > 6) {
            LogBanner(0xfc3);
            SE3Log(0xfc4, s_file, 0,
                " - # NOTICE:    DSPBufferSize out of range!! Valid range is %d to %d, or %d for AUTO DETECT", 4, 10, 0);
            SE3Log(0xfc5, s_file, 0, " - #            Using default DSPBufferSize");
            LogBanner(0xfc6);
            buffers = 4;
        }
    }
    result = g_fmodSystem->setDSPBufferSize(0x400, buffers);
    SE3Log(0xfcb, s_file, result, " - Setting DSP Buffer Size");
    if (result != FMOD_OK) {
        LogBanner(0xfce);
        SE3Log(0xfcf, s_file, 0, " - # Driver failed to accept buffer size setting");
        LogBanner(0xfd0);
        return 3;
    }
    if (g_se3Config.m_forceStereo != 0) {
        speakerMode = FMOD_SPEAKERMODE_STEREO;
    }
    result = g_fmodSystem->setSpeakerMode(speakerMode);
    const char* modeName = static_cast<uint32_t>(speakerMode) < 10 ? s_speakerModes[speakerMode] : "";
    SE3Log(0xfe7, s_file, result, " - Using Control Panel Speaker Mode %s", modeName);
    if (result != FMOD_OK) {
        SE3Log(0xfeb, s_file, 0, " - Failed to set speaker mode", modeName);
        return 3;
    }
    FMOD_SOUND_FORMAT format = FMOD_SOUND_FORMAT_PCM16;
    if (g_se3Config.m_mixMode2 != 0) {
        SE3Log(0xff6, s_file, 0,
            " -###########################################################################################");
        SE3Log(0xff7, s_file, 0, " -# NOTICE:   Using Mix Mode 2.");
        if (g_se3Config.m_mixMode2 == 1) {
            SE3Log(0xffc, s_file, 0, " -# %d PCMFLOAT", 1);
            format = FMOD_SOUND_FORMAT_PCMFLOAT;
        } else if (g_se3Config.m_mixMode2 == 2) {
            SE3Log(0x1001, s_file, 0, " -# %d PCM24", 2);
            format = FMOD_SOUND_FORMAT_PCM24;
        } else if (g_se3Config.m_mixMode2 == 3) {
            SE3Log(0x1005, s_file, 0, " -# %d PCM32", 3);
            format = FMOD_SOUND_FORMAT_PCM32;
        } else {
            SE3Log(0x100a, s_file, 0, " -# %d PCM16", 0);
        }
        SE3Log(0x100e, s_file, 0,
            " -###########################################################################################");
    }
    result = g_fmodSystem->setSoftwareFormat(g_se3Config.m_sampleRate, format, 0, 0, FMOD_DSP_RESAMPLER_LINEAR);
    SE3Log(0x1012, s_file, 0, " - Setting Software Format (%d, %d, %d).", g_se3Config.m_sampleRate, format, 1);
    if (result != FMOD_OK) {
        LogBanner(0x1015);
        SE3Log(0x1016, s_file, 0, " - # Failed to set software format, driver doesn't support selected settings.");
        LogBanner(0x1017);
        return 3;
    }
    int32_t channels = g_se3Config.m_numChannels;
    if (static_cast<uint32_t>(channels - 0x14) > 0x6c) {
        channels = channels < 0x14 ? 0x14 : 0x80;
    }
    SE3Log(0x1021, s_file, 0, " - Requested %d channels.", channels);
    bool hardwareDone = false;
    if (g_se3Config.m_useHardware != 0) {
        SE3Log(0x1026, s_file, 0, " - Initializing Hardware Channels");
        int32_t available = 0;
        result = g_fmodSystem->getHardwareChannels(&available);
        if (result == FMOD_OK) {
            SE3Log(0x1033, s_file, 0, " - Sound Driver Reports: %d Hardware Channels Available.", available);
            if (available == 0) {
                LogBanner(0x1039);
                SE3Log(0x103a, s_file, 0, " - # NOTICE:   No Hardware Channels Available.");
                SE3Log(0x103b, s_file, 0,
                    " - #           Microsoft dropped support for hardware channels in Windows Vista and Windows 7.");
                SE3Log(0x103c, s_file, 0,
                    " - #           If you are not using Windows Vista or Windows 7 and you are still seeing this, ");
                SE3Log(0x103d, s_file, 0,
                    " - #           it means your audio hardware/driver doesn't support hardware channels.");
                LogBanner(0x103e);
            } else if (available < 0x14) {
                LogBanner(0x1044);
                SE3Log(0x1045, s_file, 0, " - # NOTICE:   Insufficient Hardware Channels Available.");
                SE3Log(0x1046, s_file, 0,
                    " - #           World of Warcraft Requires At Least %d Hardware Channels to Function in Hardware Mode.",
                    0x14);
                LogBanner(0x1047);
            } else {
                if (available < channels) {
                    LogBanner(0x104d);
                    SE3Log(0x104e, s_file, 0, " - # NOTICE:");
                    SE3Log(0x104f, s_file, 0, " - # Requested %d hardware channels, but only %d are available.",
                        channels, available);
                    SE3Log(0x1050, s_file, 0, " - # Using the maximum available hardware channels (%d).", available);
                    LogBanner(0x1051);
                    channels = available;
                }
                if (g_fmodSystem->setHardwareChannels(channels) == FMOD_OK) {
                    if (g_fmodSystem->setSoftwareChannels(0x10) == FMOD_OK) {
                        hardwareDone = true;
                    } else {
                        SE3Log(0x1067, s_file, 0,
                            "- ###########################################################################################");
                        SE3Log(0x1068, s_file, 0, "- # Failed to set software channels");
                        SE3Log(0x1069, s_file, 0,
                            "- ###########################################################################################");
                    }
                } else {
                    LogBanner(0x105b);
                    SE3Log(0x105c, s_file, 0, " - # Failed to set hardware channels");
                    LogBanner(0x105d);
                }
            }
        } else {
            LogBanner(0x102d);
            SE3Log(0x102e, s_file, 0, " - # Sound driver failed to report hardware information");
            LogBanner(0x102f);
            SE3Log(0x1033, s_file, 0, " - Sound Driver Reports: %d Hardware Channels Available.", available);
        }
    }
    if (!hardwareDone) {
        SE3Log(0x1074, s_file, 0, " - Initializing Software Channels");
        if (channels > 0x40) {
            SE3Log(0x1079, s_file, 0,
                " -###########################################################################################");
            SE3Log(0x107a, s_file, 0, " -# NOTICE:   You are using a very high number of channels.");
            SE3Log(0x107b, s_file, 0, " -#           This may lead to poor sound performance.");
            SE3Log(0x107c, s_file, 0,
                " -###########################################################################################");
        }
        int32_t hardware = g_fmodSystem->setHardwareChannels(0);
        if (hardware != FMOD_OK) {
            LogBanner(0x1082);
            SE3Log(0x1083, s_file, 0, " - # Failed to set hardware channels");
            LogBanner(0x1084);
        }
        if (g_fmodSystem->setSoftwareChannels(channels) != FMOD_OK) {
            LogBanner(0x108b);
            SE3Log(0x108c, s_file, 0, " - # Failed to set software channels");
            LogBanner(0x108d);
            return 3;
        }
        if (hardware != FMOD_OK) {
            return 3;
        }
    }
    FMOD_ADVANCEDSETTINGS advanced = {};
    advanced.cbsize = sizeof(advanced);
    advanced.vol0virtualvol = 0.0001f;
    result = g_fmodSystem->setAdvancedSettings(&advanced);
    SE3Log(0x10c8, s_file, result, " - FMOD advanced settings");
    result = g_fmodSystem->init(g_se3Config.m_maxChannels, FMOD_INIT_VOL0_BECOMES_VIRTUAL | FMOD_INIT_3D_RIGHTHANDED, nullptr);
    SE3Log(0x10cf, s_file, result, " - Init FMOD Game System");
    if (result == FMOD_ERR_OUTPUT_FORMAT) {
        int32_t output = g_fmodSystem->setOutput(FMOD_OUTPUTTYPE_DSOUND);
        SE3Log(0x10d6, s_file, output,
            " - Setting output to FMOD_OUTPUTTYPE_DSOUND instead of FMOD_OUTPUTTYPE_AUTODETECT");
        result = g_fmodSystem->init(g_se3Config.m_maxChannels, FMOD_INIT_VOL0_BECOMES_VIRTUAL | FMOD_INIT_3D_RIGHTHANDED,
            nullptr);
        SE3Log(0x10d9, s_file, result, " - Attempting Init FMOD Game System again");
    }
    if (result == FMOD_ERR_OUTPUT_CREATEBUFFER) {
        LogBanner(0x10e9);
        SE3Log(0x10ea, s_file, 0, " - # NOTICE:   FMOD System Init failed!");
        SE3Log(0x10eb, s_file, 0, " - #           Control Panel Speaker Mode %s FAILED to initialize!", modeName);
        SE3Log(0x10ec, s_file, 0, " - #           Reverting to STEREO");
        SE3Log(0x10ed, s_file, 0, " - #           This most commonly happens when SURROUND is selected.");
        SE3Log(0x10ee, s_file, 0, " - #           SURROUND is actually 5.0 (no subwoofer).");
        SE3Log(0x10ef, s_file, 0,
            " - #           Many sound drivers do not support SURROUND even though you can select it in the control panel");
        SE3Log(0x10f0, s_file, 0, " - #           You probably wanted 5.1, not SURROUND.");
        SE3Log(0x10f1, s_file, 0,
            " - #           Some speaker modes will fail to initialize if the sound driver is running in EMULATED mode.");
        LogBanner(0x10f2);
    } else if (result == FMOD_OK) {
        SE3Log(0x10f9, s_file, 0, " - Ok");
        FMOD_OUTPUTTYPE output;
        g_fmodSystem->getOutput(&output);
        g_fmodOutputType = output;
        result = g_fmodSystem->set3DSettings(1.0f, 1.0f, 4.0f);
        SE3Log(0x1102, s_file, result, " - Setting Rolloff Scale");
        result = g_fmodSystem->set3DRolloffCallback(&SE3Rolloff);
        SE3Log(0x1105, s_file, result, " - Setting Rolloff Callback");
        if (!restart) {
            SE3EmitterManagerInit(&g_se3Config.m_emitterGridCell, g_se3Config.m_emitterParam88, g_se3Config.m_emitterParam8c);
        }
        InitSystems(restart);
        return 0;
    }
    SE3Log(0x10f6, s_file, result, " - Failed to init FMOD Game System.");
    return 3;
}

}

// ref: FUN_1412cb260
int32_t SE3Init(bool restart, blz::function_storage* configure) {
    SE3ConfigDefaults(&g_se3Config);
    SE3LogOpen();
    SE3Log(0x8f4, s_file, 0, " - SE3 Init...");
    if (configure->m_invoke != nullptr) {
        CallConfig(configure, &g_se3Config);
    }
    int32_t result = InitFmod(restart);
    if (result == 0) {
        SE3Log(0x8ff, s_file, 0, " - SE3 Init Success.");
    } else {
        SE3Log(0x903, s_file, 0, " - SE3 Init Failed.");
        bool retried = false;
        if (g_se3Config.m_onInitFailed.m_invoke != nullptr) {
            CallConfig(&g_se3Config.m_onInitFailed, &g_se3Config);
            ReleaseSystem();
            result = InitFmod(restart);
            retried = result == 0;
        }
        if (!retried) {
            ReleaseSystem();
            InitSilent();
        }
    }
    // FUN_1402db190 / FUN_1402db330: the CAS update handlers (LAB_1412d7650, LAB_1412d7660) that
    // reload sounds a hot-fix changed; the in-place update is not ported (as for textures).
    configure->m_manager(configure->m_storage, nullptr, 0);
    return result;
}

// ---- stop ---------------------------------------------------------------------------------

// ref: FUN_1412d41d0
void SE3Shutdown(bool restart) {
    SE3Log(0x98f, s_file, 0, " - Shutting Down SE3");
    g_se3ShuttingDown = 1;
    // FUN_1402db4d0 / FUN_1402db6e0: the CAS update handlers (never added, see SE3Init).
    SE3StopGroup(0xfe, true);
    SE3SuppressorsShutdown();
    SE3CacheUpdate(0, true, restart);
    SE3ReleaseFinishedInstances();
    SE3ReleaseKitDefs();
    SE3BusShutdown();
    ReleaseSystem();
    SE3FreeEngineLists();
    SE3CombinableShutdown();
    if (!restart) {
        SE3EmitterManagerShutdown();
    }
    SE3Log(0x9fd, s_file, 0, " - Shutting Down SE3 COMPLETE.");
    SE3LogClose();
}

// ---- the frame ----------------------------------------------------------------------------

// ref: FUN_1412cab50
void SE3Update() {
    if (g_se3ShuttingDown != 0) {
        return;
    }
    static bool s_timeStarted = false;
    if (!s_timeStarted) {
        s_lastUpdateTime = OsGetAsyncTimeMs();
        s_timeStarted = true;
    }
    uint32_t now = g_se3Config.m_getTime.m_invoke != nullptr ? static_cast<uint32_t>(CallTime(&g_se3Config.m_getTime))
                                                              : FrameTime_GetTime();
    int32_t elapsed = static_cast<int32_t>(now - s_lastUpdateTime);
    if (elapsed < 0) {
        elapsed = 0;
    }
    s_lastUpdateTime = now;
    SE3ReleaseFinishedInstances();
    SE3CacheUpdate(elapsed, false, false);
    if (g_fmodSystem != nullptr) {
        // FMOD runs with the FPU exceptions masked.
        _clearfp();
        uint32_t control = _control87(0, 0);
        _control87(0x8001f, 0x8001f);
        g_fmodSystem->update();
        _clearfp();
        _control87(control & 0x8001f, 0x8001f);
    }
    SE3CacheTrim();
    SE3CacheReleaseIdle();
    SE3UpdateCombinedKits();
    s_cleanupTimer -= elapsed;
    if (s_cleanupTimer < 1) {
        s_cleanupTimer = 10000;
        SE3ReleaseUnusedKitGroups();
    }
    s_rangeTimer -= elapsed;
    if (s_rangeTimer < 1) {
        s_rangeTimer = 2000;
        SE3UpdateRange();
    }
    SE3EmitterUpdate(elapsed);
    SE3ProcessDoneLoading();
    SE3UpdateWaitingInstances();
    SE3StartLoadedInstances();
    SE3UpdateFades(elapsed);
    SE3ProcessSoundEnded();
    if (g_se3Config.m_getGameTime.m_invoke != nullptr) {
        g_se3GameTime = CallTime(&g_se3Config.m_getGameTime);
    }
    SE3SuppressorsUpdate(elapsed);
    SE3UpdateInstances(elapsed);
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142cfca08 = (void*)&s_initialized;
// ---- end of generated globals ----
