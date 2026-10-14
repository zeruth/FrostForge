#ifndef SOUND_SE3_HPP
#define SOUND_SE3_HPP

// SE3 (Engine/Source/SoundEngine/SE3*.cpp): the engine's sound system over FMOD Ex 4.44. It owns
// the FMOD system, the sound buses (one FMOD channel group per SoundBus record), the sound kit
// instances, and Logs/Sound.log. The game's side (SI3, WoW/Source/SoundInterface) fills the
// configuration below and installs the callbacks the engine asks the game through.

#include <cstdarg>
#include <cstdint>

#include "blz/Function.hpp"

namespace FMOD {
class System;
class ChannelGroup;
class DSP;
}

// The configuration SE3 starts from (DAT_142cfca40, 0x2b0). SE3ConfigDefaults sets the engine's
// defaults; the game's configure callback then fills it from its CVars.
struct SE3CONFIG {
    uint8_t m_silent;               // 0x00 start in silent mode
    uint8_t m_enableSFX;            // 0x01
    uint8_t m_enableMusic;          // 0x02
    uint8_t m_enableAmbience;       // 0x03
    uint8_t m_enableAllSound;       // 0x04
    uint8_t m_enableDialog;         // 0x05
    uint8_t m_useHardware;          // 0x06 initialize hardware channels
    uint8_t m_enableReverb;         // 0x07
    uint8_t m_forceStereo;          // 0x08 ignore the control panel speaker mode
    uint8_t m_09;                   // 0x09
    uint8_t m_pad0a[2];
    float m_masterVolume;           // 0x0C
    float m_sfxVolume;              // 0x10
    float m_musicVolume;            // 0x14
    float m_ambienceVolume;         // 0x18
    float m_dialogVolume;           // 0x1C
    float m_volume20;               // 0x20 the volume of bus 0x17
    int32_t m_maxChannels;          // 0x24 System::init's channel count
    int32_t m_numChannels;          // 0x28 software channels asked for (20..128)
    int32_t m_2c;                   // 0x2C a period in ms (SI3: one day)
    int32_t m_outputDriverIndex;    // 0x30 0 the system default, n the (n-1)th FMOD driver
    int32_t m_mixMode2;             // 0x34 0 PCM16, 1 PCMFLOAT, 2 PCM24, 3 PCM32
    int32_t m_sampleRate;           // 0x38
    int32_t m_dspBufferSize;        // 0x3C buffer count, 0 auto
    const char* m_outputDriverName; // 0x40
    char m_defaultDriverName[0x40]; // 0x48 how the system default is shown
    int32_t m_emitterParam88;       // 0x88 the emitter manager's grid parameters
    int32_t m_emitterParam8c;       // 0x8C
    // The game's callbacks (blz::function, called with the arguments below and their storage).
    blz::function_storage m_getTime;            // 0x90 int(): the current time in ms
    blz::function_storage m_onInitFailed;       // 0xB0 void(SE3CONFIG*): fall back to safe settings
    blz::function_storage m_onDevicesChanged;   // 0xD0 void()
    blz::function_storage m_getGameTime;        // 0xF0 int()
    blz::function_storage m_slot110;            // 0x110
    blz::function_storage m_slot130;            // 0x130
    blz::function_storage m_slot150;            // 0x150
    blz::function_storage m_slot170;            // 0x170
    blz::function_storage m_emitterGridCell;    // 0x190 void(pos*, uint32_t* x, uint32_t* y): the emitter grid cell of a position
    blz::function_storage m_onInitDone;         // 0x1B0 void(): after the engine's systems are up
    blz::function_storage m_slot1d0;            // 0x1D0
    blz::function_storage m_slot1f0;            // 0x1F0
    blz::function_storage m_slot210;            // 0x210
    blz::function_storage m_slot230;            // 0x230
    blz::function_storage m_slot250;            // 0x250
    blz::function_storage m_slot270;            // 0x270
    blz::function_storage m_slot290;            // 0x290
};
static_assert(sizeof(SE3CONFIG) == 0x2b0, "SE3CONFIG is 0x2b0 bytes");

extern SE3CONFIG g_se3Config;               // DAT_142cfca40
extern FMOD::System* g_fmodSystem;          // DAT_1430a9460
extern int32_t g_fmodOutputType;            // DAT_142cfca00
extern volatile uint8_t g_se3ShuttingDown;  // DAT_142ddcea4

// A slot holding a plain function: the invoker calls the function pointer kept in the storage
// with the slot's arguments, then the storage (the reference's LAB_1402282e0, FUN_14032a520 and
// their siblings).
template <class F>
struct SE3SlotInvoker;
template <class R, class... A>
struct SE3SlotInvoker<R (*)(A...)> {
    static R Call(A... args, void* storage) {
        return (*static_cast<R (**)(A...)>(storage))(args...);
    }
};

// Replaces a slot's callable with `function`.
void SE3SetSlot(blz::function_storage* slot, void* invoke, void* function);
template <class F>
void SE3SetSlot(blz::function_storage* slot, F function) {
    SE3SetSlot(slot, reinterpret_cast<void*>(&SE3SlotInvoker<F>::Call), reinterpret_cast<void*>(function));
}

// Logs/Sound.log: an FMOD error (when `result` is not FMOD_OK) and/or a message.
// ref: FUN_1412d9970
void SE3Log(int32_t line, const char* file, int32_t result, const char* format, ...);
// Opens Logs/Sound.log (and removes the old SESound.log).
// ref: FUN_1412d9900
void SE3LogOpen();
// ref: FUN_1412d9950
void SE3LogClose();

// The engine's defaults.
// ref: FUN_1412d2260
void SE3ConfigDefaults(SE3CONFIG* config);
// Starts the sound system: the defaults, the game's `configure(config)`, then FMOD; a failed
// start retries with the game's fallback settings, then falls back to silence. 0 on success.
// ref: FUN_1412cb260
int32_t SE3Init(bool restart, blz::function_storage* configure);
// ref: FUN_1412d41d0
void SE3Shutdown(bool restart);
// The frame's update.
// ref: FUN_1412cab50
void SE3Update();

// Output drivers: the count (the system default counts as driver 0, before FMOD's own), and
// each one's name (*isDefault set for the system default).
// ref: FUN_1412c9520
int32_t SE3GetNumOutputDrivers();
// ref: FUN_1412c9580
bool SE3GetOutputDriverName(int32_t index, char* name, int32_t size, bool* isDefault);
// The name of the driver in use.
// ref: FUN_1412c9450
const char* SE3GetActiveDriverName();

// Sound buses (SE3Bus.cpp).
enum : int8_t {
    SE3BUS_SFX = 0,
    SE3BUS_MUSIC = 1,
    SE3BUS_AMBIENCE = 2,
    SE3BUS_DIALOG = 3,
    SE3BUS_ERRORSPEECH = 0xb,
    SE3BUS_MASTER = 0x16,
    SE3BUS_17 = 0x17,
    SE3BUS_COUNT = 0x62,
};
// ref: FUN_1412d8840
void SE3BusInit(SE3CONFIG* config);
// ref: FUN_1412d7df0
void SE3BusSetEnabled(int8_t bus, bool enabled);
// ref: FUN_1412d8d50
void SE3BusSetMuted(int8_t bus, bool muted);
// ref: FUN_1412d8d00
bool SE3BusIsMuted(int8_t bus, bool inherited);
// ref: FUN_1412d8e60
void SE3BusSetVolume(int8_t bus, float volume);
// The bus a SoundBus record id names (SE3BUS_COUNT when none).
// ref: FUN_1412d7860
int8_t SE3BusFromRecord(int32_t id);

// The sound cache limits (SE3Cache.cpp).
// ref: FUN_1412db880
void SE3CacheSetMaxSize(int32_t bytes);
// ref: FUN_1412db890
void SE3CacheSetMaxCacheableSize(int32_t bytes);

// Whether the buses sound: on at start, then by the client's focus.
// ref: FUN_1412d3420
void SE3SetActive(bool active);

// Sets the FMOD reverb (an FMOD_REVERB_PROPERTIES).
// ref: FUN_1412d3ad0
void SE3SetReverbProperties(const void* properties);

// Makes an FMOD channel group.
// ref: FUN_1412c85f0
FMOD::ChannelGroup* SE3CreateChannelGroup(const char* name);

#endif
