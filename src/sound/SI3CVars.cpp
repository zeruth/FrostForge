// SI3CVars (WoW/Source/SoundInterface/SI3CVars.cpp): the sound CVars. Most settings are read
// once when the engine starts; the enables and volumes reach the buses at once.

#include "sound/SI3.hpp"

#include <cstdlib>
#include <cstring>

#include "console/Var.hpp"
#include "fmod.hpp"
#include "os/Os.hpp"
#include "sound/SE3.hpp"
#include "storm/String.hpp"
#include "util/Unimplemented.hpp"

CVar* g_soundNumChannels;
CVar* g_soundEnableReverb;
CVar* g_soundOutputDriverIndex;
CVar* g_soundOutputDriverName;
CVar* g_soundDSPBufferSize;
CVar* g_soundEnableMixMode2;
CVar* g_soundOutputSampleRate;
CVar* g_soundEnableSFX;
CVar* g_soundEnableAmbience;
CVar* g_soundEnableErrorSpeech;
CVar* g_soundEnableMusic;
CVar* g_soundEnableAllSound;
CVar* g_soundEnableDialog;
CVar* g_soundMasterVolume;
CVar* g_soundSFXVolume;
CVar* g_soundMusicVolume;
CVar* g_soundAmbienceVolume;
CVar* g_soundDialogVolume;
CVar* g_soundListenerAtCharacter;
CVar* g_soundEnableEmoteSounds;
CVar* g_soundZoneMusicNoDelay;
CVar* g_soundEnableArmorFoleySelf;
CVar* g_soundEnableArmorFoleyOthers;
CVar* g_soundEnableDSPEffects;
CVar* g_soundEnableInBackground;
CVar* g_soundMaxCacheSize;
CVar* g_soundMaxCacheableSize;
CVar* g_footstepSounds;
CVar* g_soundEnablePetSounds;
CVar* g_soundAlternateListener;
CVar* g_soundVoiceInputDriverIndex;
CVar* g_soundVoiceInputDriverName;
CVar* g_soundVoiceOutputDriverIndex;
CVar* g_soundVoiceOutputDriverName;
CVar* g_soundEnablePingSounds;
CVar* g_soundPingVolume;

namespace {

const char s_file[] = "D:\\BuildServer\\A\\work-git\\wow\\WoW\\Source\\SoundInterface\\SI3CVars.cpp";

// The CVars' integer parse: strtol, in hex after "0x".
long ParseInt(const char* value) {
    int radix = 10;
    if (value != nullptr && value[0] == '0' && ((static_cast<uint8_t>(value[1]) + 0xa8u) & 0xdf) == 0) {
        radix = 16;
    }
    return std::strtol(value, nullptr, radix);
}

// ref: FUN_14130dac0
bool NumChannelsChanged(CVar*, const char*, const char* value, void*) {
    return static_cast<unsigned long>(ParseInt(value) - 0x14) < 0x6d;
}

// The reverb: on, the zone reverbs; off, FMOD's reverb preset "off".
// ref: FUN_14130c620
bool EnableReverbChanged(CVar*, const char*, const char* value, void*) {
    if (ParseInt(value) != 0) {
        SI3SetReverbEnabled(true);
        SI3UpdateReverb();
        return true;
    }
    SI3SetReverbEnabled(false);
    FMOD_REVERB_PROPERTIES off = FMOD_PRESET_OFF;
    SE3SetReverbProperties(&off);
    return true;
}

// A new index stores that driver's name.
// ref: FUN_14130c860
bool OutputDriverIndexChanged(CVar*, const char*, const char* value, void*) {
    long index = ParseInt(value);
    char name[0x100] = {};
    if (SE3GetOutputDriverName(static_cast<int32_t>(index), name, 0x100, nullptr)) {
        CVarSet(g_soundOutputDriverName, name, true, false, false, true);
    }
    return true;
}

void SetBusEnabled(int8_t bus, const char* value) {
    long enabled = ParseInt(value);
    SE3BusSetEnabled(bus, enabled != 0);
    SE3BusSetMuted(bus, enabled == 0);
}

// ref: FUN_14130c720
bool EnableSFXChanged(CVar*, const char*, const char* value, void*) {
    SetBusEnabled(SE3BUS_SFX, value);
    return true;
}

// ref: FUN_14130c4a0
bool EnableAmbienceChanged(CVar*, const char*, const char* value, void*) {
    SetBusEnabled(SE3BUS_AMBIENCE, value);
    return true;
}

// ref: FUN_14130c560
bool EnableErrorSpeechChanged(CVar*, const char*, const char* value, void*) {
    SetBusEnabled(SE3BUS_ERRORSPEECH, value);
    return true;
}

// ref: FUN_14130c5c0
bool EnableMusicChanged(CVar*, const char*, const char* value, void*) {
    long enabled = ParseInt(value);
    SE3BusSetEnabled(SE3BUS_MUSIC, enabled != 0);
    SE3BusSetMuted(SE3BUS_MUSIC, enabled == 0);
    if (enabled == 0) {
        SI3StopMusic();
    }
    return true;
}

// ref: FUN_14130c440
bool EnableAllSoundChanged(CVar*, const char*, const char* value, void*) {
    SetBusEnabled(SE3BUS_MASTER, value);
    return true;
}

// ref: FUN_14130c500
bool EnableDialogChanged(CVar*, const char*, const char* value, void*) {
    SetBusEnabled(SE3BUS_DIALOG, value);
    return true;
}

// ref: FUN_14130c780
bool MasterVolumeChanged(CVar*, const char*, const char* value, void*) {
    SE3BusSetVolume(SE3BUS_MASTER, SStrToFloat(value));
    return true;
}

// ref: FUN_14130daa0
bool SFXVolumeChanged(CVar*, const char*, const char* value, void*) {
    SE3BusSetVolume(SE3BUS_SFX, SStrToFloat(value));
    return true;
}

// ref: FUN_14130c840
bool MusicVolumeChanged(CVar*, const char*, const char* value, void*) {
    SE3BusSetVolume(SE3BUS_MUSIC, SStrToFloat(value));
    return true;
}

// ref: FUN_14130c400
bool AmbienceVolumeChanged(CVar*, const char*, const char* value, void*) {
    SE3BusSetVolume(SE3BUS_AMBIENCE, SStrToFloat(value));
    return true;
}

// ref: FUN_14130c420
bool DialogVolumeChanged(CVar*, const char*, const char* value, void*) {
    SE3BusSetVolume(SE3BUS_DIALOG, SStrToFloat(value));
    return true;
}

// ref: FUN_14130c7a0
bool MaxCacheSizeChanged(CVar*, const char*, const char* value, void*) {
    SE3CacheSetMaxSize(static_cast<int32_t>(ParseInt(value)));
    return true;
}

// ref: FUN_14130c7f0
bool MaxCacheableSizeChanged(CVar*, const char*, const char* value, void*) {
    SE3CacheSetMaxCacheableSize(static_cast<int32_t>(ParseInt(value)));
    return true;
}

// FUN_14026bea0: accepts anything.
bool Accept(CVar*, const char*, const char*, void*) {
    return true;
}

CVar* Register(const char* name, const char* help, const char* value, CVARCALLBACK callback) {
    return CVarRegister(name, help, 0, value, callback, 7, false, nullptr, false);
}

}

// ref: FUN_14130c900
void SI3RegisterCVars() {
    const OSCPUINFO* cpu = OsGetCpuInfo();
    Register("ChatMusicVolume", "Music volume (0.0 to 1.0)", "0.3", nullptr);
    Register("ChatSoundVolume", "Sound volume (0.0 to 1.0)", "0.4", nullptr);
    Register("ChatAmbienceVolume", "Ambience Volume (0.0 to 1.0)", "0.3", nullptr);
    const char* channels = cpu->m_logical < 4 ? "24" : cpu->m_logical < 8 ? "32" : "64";
    g_soundNumChannels = Register("Sound_NumChannels", "number of sound channels", channels, &NumChannelsChanged);
    g_soundEnableReverb = Register("Sound_EnableReverb", "", "0", &EnableReverbChanged);
    g_soundOutputDriverIndex = Register("Sound_OutputDriverIndex", "", "0", &OutputDriverIndexChanged);
    g_soundOutputDriverName = Register("Sound_OutputDriverName", "", "Primary Sound Driver", nullptr);
    g_soundVoiceInputDriverIndex = Register("Sound_VoiceChatInputDriverIndex", "", "0", nullptr);
    g_soundVoiceInputDriverName =
        Register("Sound_VoiceChatInputDriverName", "", "Primary Sound Capture Driver", nullptr);
    g_soundVoiceOutputDriverIndex = Register("Sound_VoiceChatOutputDriverIndex", "", "0", nullptr);
    g_soundVoiceOutputDriverName = Register("Sound_VoiceChatOutputDriverName", "", "Primary Sound Driver", nullptr);
    g_soundDSPBufferSize = Register("Sound_DSPBufferSize", "sound buffer size, default 0", "0", nullptr);
    g_soundOutputSampleRate = Register("Sound_OutputSampleRate", "output sample rate", "44100", nullptr);
    g_soundEnableMixMode2 = Register("Sound_EnableMixMode2", "test", "0", nullptr);
    g_soundEnableSFX = Register("Sound_EnableSFX", "", "1", &EnableSFXChanged);
    g_soundEnableAmbience = Register("Sound_EnableAmbience", "Enable Ambience", "1", &EnableAmbienceChanged);
    g_soundEnableErrorSpeech = Register("Sound_EnableErrorSpeech", "error speech", "1", &EnableErrorSpeechChanged);
    g_soundEnableMusic = Register("Sound_EnableMusic", "Enables music", "1", &EnableMusicChanged);
    g_soundEnableAllSound = Register("Sound_EnableAllSound", "", "1", &EnableAllSoundChanged);
    g_soundEnableDialog = Register("Sound_EnableDialog", "all dialog", "1", &EnableDialogChanged);
    g_soundMasterVolume = Register("Sound_MasterVolume", "master volume (0.0 to 1.0)", "1.0", &MasterVolumeChanged);
    g_soundSFXVolume = Register("Sound_SFXVolume", "sound volume (0.0 to 1.0)", "1.0", &SFXVolumeChanged);
    g_soundMusicVolume = Register("Sound_MusicVolume", "music volume (0.0 to 1.0)", "0.4", &MusicVolumeChanged);
    g_soundAmbienceVolume =
        Register("Sound_AmbienceVolume", "Ambience Volume (0.0 to 1.0)", "0.6", &AmbienceVolumeChanged);
    g_soundDialogVolume = Register("Sound_DialogVolume", "Dialog Volume (0.0 to 1.0)", "1.0", &DialogVolumeChanged);
    g_soundListenerAtCharacter = Register("Sound_ListenerAtCharacter", "lock listener at character", "1", &Accept);
    g_soundEnableEmoteSounds = Register("Sound_EnableEmoteSounds", "", "1", &Accept);
    g_soundZoneMusicNoDelay = Register("Sound_ZoneMusicNoDelay", "", "0", &Accept);
    g_soundEnableArmorFoleySelf = Register("Sound_EnableArmorFoleySoundForSelf", "", "1", nullptr);
    g_soundEnableArmorFoleyOthers = Register("Sound_EnableArmorFoleySoundForOthers", "", "1", nullptr);
    g_soundEnableDSPEffects = Register("Sound_EnableDSPEffects", "", 1 < cpu->m_logical ? "1" : "0", &Accept);
    g_soundEnablePetSounds = Register("Sound_EnablePetSounds", "Enables pet sounds", "1", &Accept);
    g_soundMaxCacheSize =
        Register("Sound_MaxCacheSizeInBytes", "Max cache size in bytes", "134217728", &MaxCacheSizeChanged);
    char cacheable[16];
    SStrPrintf(cacheable, sizeof(cacheable), "%u", 0x2aaaa);
    g_soundMaxCacheableSize = Register("Sound_MaxCacheableSizeInBytes",
        "Max sound size that will be cached, larger files will be streamed instead", cacheable,
        &MaxCacheableSizeChanged);
    g_footstepSounds = Register("FootstepSounds", "play footstep sounds", "1", &Accept);
    g_soundAlternateListener = Register("Sound_AlternateListener",
        "When enabled, calculates listener forward by simply using the camera's yaw value, instead of a vector "
        "from camera position to listener position",
        "1", &Accept);
    g_soundEnablePingSounds = Register("Sound_EnablePingSounds", "Enable Ping Sounds", "1", &Accept);
    g_soundPingVolume = Register("Sound_PingVolume", "Ping Volume (0.0 to 1.0)", "1.0", &Accept);
    g_soundEnableInBackground = Register("Sound_EnableSoundWhenGameIsInBG", "Enable Sound When Game Is In Background",
        "0", &Accept);

    SE3Log(0x164, s_file, 0, " ");
    SE3Log(0x165, s_file, 0, "=> User Settings Report:");
    SE3Log(0x166, s_file, 0, " - ========= PLAYBACK =========");
    SE3Log(0x167, s_file, 0, " - Enable All Sound         [%d]", g_soundEnableAllSound->m_intValue);
    SE3Log(0x168, s_file, 0, " - Enable SFX               [%d]", g_soundEnableSFX->m_intValue);
    SE3Log(0x169, s_file, 0, " --- Enable Emote Sounds    [%d]", g_soundEnableEmoteSounds->m_intValue);
    SE3Log(0x16a, s_file, 0, " - Enable Music             [%d]", g_soundEnableMusic->m_intValue);
    SE3Log(0x16b, s_file, 0, " --- Loop Music             [%d]", g_soundZoneMusicNoDelay->m_intValue);
    SE3Log(0x16f, s_file, 0, " - Enable Ambient Sounds    [%d]", g_soundEnableAmbience->m_intValue);
    SE3Log(0x170, s_file, 0, " - Enable Dialog            [%d]", g_soundEnableDialog->m_intValue);
    SE3Log(0x171, s_file, 0, " --- Enable Error Speech    [%d]", g_soundEnableErrorSpeech->m_intValue);
    SE3Log(0x172, s_file, 0, " - Sound at Character       [%d]", g_soundListenerAtCharacter->m_intValue);
    SE3Log(0x173, s_file, 0, " - Sound in Background      [%d]", g_soundEnableInBackground->m_intValue);
    SE3Log(0x174, s_file, 0, " - Enable Reverb            [%d]", g_soundEnableReverb->m_intValue);
    SE3Log(0x175, s_file, 0, " -");
    SE3Log(0x176, s_file, 0, " - ========== VOLUME ==========");
    SE3Log(0x177, s_file, 0, " - Master Volume         [%.2f]", static_cast<double>(g_soundMasterVolume->m_floatValue));
    SE3Log(0x178, s_file, 0, " - SFX Volume            [%.2f]", static_cast<double>(g_soundSFXVolume->m_floatValue));
    SE3Log(0x179, s_file, 0, " - Music Volume          [%.2f]", static_cast<double>(g_soundMusicVolume->m_floatValue));
    SE3Log(0x17a, s_file, 0, " - Ambience Volume       [%.2f]",
        static_cast<double>(g_soundAmbienceVolume->m_floatValue));
    SE3Log(0x17b, s_file, 0, " - Dialog Volume         [%.2f]", static_cast<double>(g_soundDialogVolume->m_floatValue));
    SE3Log(0x17c, s_file, 0, " -");
    SE3Log(0x17d, s_file, 0, " - =========== PING ===========");
    // The reference passes the int value to %.2f: its bits read as a double.
    uint64_t pingBits = static_cast<uint32_t>(g_soundEnablePingSounds->m_intValue);
    double ping;
    std::memcpy(&ping, &pingBits, sizeof(ping));
    SE3Log(0x17e, s_file, 0, " - Enable Ping Sounds    [%.2f]", ping);
    SE3Log(0x17f, s_file, 0, " - Ping Volume           [%.2f]", static_cast<double>(g_soundPingVolume->m_floatValue));
    SE3Log(0x180, s_file, 0, " -");
    SE3Log(0x181, s_file, 0, " - =========== MISC ===========");
    SE3Log(0x182, s_file, 0, " - Sound Channels          [%d]", g_soundNumChannels->m_intValue);
    SE3Log(0x183, s_file, 0, " - Sound Cache           [%dMB]", g_soundMaxCacheSize->m_intValue / 0x100000);
    SE3Log(0x184, s_file, 0, " - Mix Mode 2               [%d]", g_soundEnableMixMode2->m_intValue);
    if (g_soundDSPBufferSize->m_intValue == 0) {
        SE3Log(0x18b, s_file, 0, " - DSP Buffer Size       [AUTO]");
    } else {
        SE3Log(0x187, s_file, 0, " - DSP Buffer Size          [%d]", g_soundDSPBufferSize->m_intValue);
    }
    SE3Log(0x18d, s_file, 0, " - Armor Foley SFX (Self)   [%d]", g_soundEnableArmorFoleySelf->m_intValue);
    SE3Log(0x18e, s_file, 0, " - Armor Foley SFX (Others) [%d]", g_soundEnableArmorFoleyOthers->m_intValue);
    SE3Log(399, s_file, 0, "=> End of User Settings Report");
    SE3Log(400, s_file, 0, " ");
}
