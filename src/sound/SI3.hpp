#ifndef SOUND_SI3_HPP
#define SOUND_SI3_HPP

// SI3 (WoW/Source/SoundInterface): the game's side of the sound system. It owns the sound CVars,
// fills SE3's configuration from them, and restarts the engine when a setting asks for it.

#include <cstdint>

struct CVar;

// The CVars (SI3CVars.cpp).
extern CVar* g_soundNumChannels;            // DAT_1430a9e38
extern CVar* g_soundEnableReverb;           // DAT_1430a9e40
extern CVar* g_soundOutputDriverIndex;      // DAT_1430a9e48
extern CVar* g_soundOutputDriverName;       // DAT_1430a9e50
extern CVar* g_soundDSPBufferSize;          // DAT_1430a9e58
extern CVar* g_soundEnableMixMode2;         // DAT_1430a9e60
extern CVar* g_soundOutputSampleRate;       // DAT_1430a9e68
extern CVar* g_soundEnableSFX;              // DAT_1430a9e70
extern CVar* g_soundEnableAmbience;         // DAT_1430a9e78
extern CVar* g_soundEnableErrorSpeech;      // DAT_1430a9e80
extern CVar* g_soundEnableMusic;            // DAT_1430a9e88
extern CVar* g_soundEnableAllSound;         // DAT_1430a9e90
extern CVar* g_soundEnableDialog;           // DAT_1430a9e98
extern CVar* g_soundMasterVolume;           // DAT_1430a9ea0
extern CVar* g_soundSFXVolume;              // DAT_1430a9ea8
extern CVar* g_soundMusicVolume;            // DAT_1430a9eb0
extern CVar* g_soundAmbienceVolume;         // DAT_1430a9eb8
extern CVar* g_soundDialogVolume;           // DAT_1430a9ec0
extern CVar* g_soundListenerAtCharacter;    // DAT_1430a9ec8
extern CVar* g_soundEnableEmoteSounds;      // DAT_1430a9ed0
extern CVar* g_soundZoneMusicNoDelay;       // DAT_1430a9ed8
extern CVar* g_soundEnableArmorFoleySelf;   // DAT_1430a9ee0
extern CVar* g_soundEnableArmorFoleyOthers; // DAT_1430a9ee8
extern CVar* g_soundEnableDSPEffects;       // DAT_1430a9ef0
extern CVar* g_soundEnableInBackground;     // DAT_1430a9ef8
extern CVar* g_soundMaxCacheSize;           // DAT_1430a9f00
extern CVar* g_soundMaxCacheableSize;       // DAT_1430a9f08
extern CVar* g_footstepSounds;              // DAT_1430a9f10
extern CVar* g_soundEnablePetSounds;        // DAT_1430a9f18
extern CVar* g_soundAlternateListener;      // DAT_1430a9f20
extern CVar* g_soundVoiceInputDriverIndex;  // DAT_1430a9f30
extern CVar* g_soundVoiceInputDriverName;   // DAT_1430a9f38
extern CVar* g_soundVoiceOutputDriverIndex; // DAT_1430a9f40
extern CVar* g_soundVoiceOutputDriverName;  // DAT_1430a9f48
extern CVar* g_soundEnablePingSounds;       // DAT_1430a9f50
extern CVar* g_soundPingVolume;             // DAT_1430a9f58

// Registers the sound CVars and logs the user settings report.
// ref: FUN_14130c900
void SI3RegisterCVars();

// Starts the sound system from the CVars.
// ref: FUN_1412e59c0
void SI3Init(bool restart);
// The window took (true) or lost the focus.
// ref: FUN_1412e5ae0
void SI3OnFocus(bool active);
// ref: FUN_1412e6a50
void SI3Shutdown(bool restart);
// The frame's update: the interface's own lists, then a pending restart.
void SI3Update();
// Asks for a restart at the next update (Sound_GameSystem_RestartSoundSystem).
// ref: FUN_1412e6b30
void SI3RequestRestart();
// The main loop's check for a sound reset the device asked for.
// ref: FUN_1412e1c00
void SI3CheckPendingReset();

// The zone reverbs (SI3Reverb): on or off, and a fresh pick of the current one.
// ref: FUN_141312260
void SI3SetReverbEnabled(bool enabled);
// ref: FUN_141312590
void SI3UpdateReverb();
// Stops the zone and glue music (Sound_EnableMusic off).
// ref: FUN_1412e6c50
void SI3StopMusic();

// The device-change slot (LAB_14021f080): the main loop's pending reset (ClientLoop.cpp).
void ClientSetPendingSoundReset();

// Playing (SI3Sound.cpp).
// ref: FUN_141311c90
int8_t SI3GlueBus(int32_t which);
// ref: FUN_141311cd0
int8_t SI3CategoryBus(int32_t category);
// ref: FUN_14130c210
int32_t SI3CategoryFromName(const char* name);
// ref: FUN_1412e6120
uint32_t SI3PlayUISound(int32_t kitId, int32_t category, bool noDuplicates, int32_t* handle, const int32_t* priority,
    bool hasPriority);
// ref: FUN_1412cdfa0
uint32_t SI3PlaySoundFile(int32_t fileDataId, int32_t* handle, int8_t bus, int32_t mode);
// ref: FUN_1412f2140
void SI3PlayGlueMusic(int32_t kitId);
// ref: FUN_1412f20e0
void SI3PlayGlueAmbience(int32_t kitId, int32_t fadeMs);
// ref: FUN_1412f2300
void SI3StopGlueAmbience();
// ref: FUN_1412f22e0
void SI3StopCreditsMusic();
// ref: FUN_1412f2320
void SI3StopGlueMusic(int32_t fadeType);
// ref: FUN_1412f1f00
bool SI3SetCreditsExpansion(uint32_t expansion);
// ref: FUN_1412f1f20
void SI3PlayCreditsMusic(uint32_t track);
// ref: FUN_1412f97f0
void SI3PlayMusicFile(int32_t fileDataId);
// ref: FUN_1412fcc70
void SI3StopMusicFile();

#endif
