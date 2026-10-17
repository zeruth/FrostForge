// SI3 (WoW/Source/SoundInterface): playing sounds for the game -- the glue's music and
// ambience, the credits music, UI sounds, and sound files.

#include "sound/SI3.hpp"

#include <cstring>

#include "sound/SE3.hpp"
#include "sound/SE3Kit.hpp"
#include "services/ClientServices.hpp"
#include "util/Unimplemented.hpp"

namespace {

int32_t s_creditsHandle;        // DAT_1430a9928
int32_t s_glueMusicKit;         // DAT_1430a992c
int32_t s_glueMusicHandle;      // DAT_1430a9930
int32_t s_glueAmbienceKit;      // DAT_1430a9934
int32_t s_glueAmbienceHandle;   // DAT_1430a9938
uint32_t s_creditsExpansion;    // DAT_1430a993c
uint8_t s_creditsStopped;       // DAT_142cfd2c8
uint8_t s_glueMusicStopped;     // DAT_142cfd2c9
uint8_t s_glueAmbienceStopped;  // DAT_142cfd2ca
int32_t s_creditsTrack;         // DAT_142cfd2cc
int32_t s_musicHandle;          // DAT_1430a99f4

// The credits' tracks by expansion (0x14272c8d0, 0x14272c8f0, 0x14272c918).
const int32_t s_credits0[7] = {10763, 139624, 139625, 139626, 139627, 139628, 139629};
const int32_t s_credits1[10] = {10804, 181016, 181017, 181018, 181019, 181020, 181021, 181022, 181023, 181024};
const int32_t s_credits2[10] = {13822, 181016, 181017, 181018, 181019, 181020, 181021, 181022, 181023, 181024};

void InitParams(SE3PLAYPARAMS* params) {
    SE3PlayParamsConstruct(params);
}

}

// The glue's buses: ambience 0 -> SE3 bus 2, music 1 and 2 -> 1.
// ref: FUN_141311c90
int8_t SI3GlueBus(int32_t which) {
    if (which == 0) {
        return SE3BUS_AMBIENCE;
    }
    if (which != 1 && which != 2) {
        return -1;
    }
    return SE3BUS_MUSIC;
}

// The bus of a script sound category.
// ref: FUN_141311cd0
int8_t SI3CategoryBus(int32_t category) {
    switch (category) {
        case 0:
            return 0x21;
        case 1:
            return 5;
        case 2:
            return 10;
        case 3:
            return 7;
        case 4:
            return 9;
        case 5:
            return 3;
        case 6:
            return 6;
        case 7:
            return 8;
        case 8:
            return 0x3e;
        case 9:
            return 0xb;
        default:
            return -1;
    }
}

// A script channel name's category ("SFX" and anything else: 3).
// ref: FUN_14130c210
int32_t SI3CategoryFromName(const char* name) {
    if (name == nullptr) {
        return 3;
    }
    if (_stricmp("Master", name) == 0) {
        return 6;
    }
    if (_stricmp("Music", name) == 0) {
        return 7;
    }
    if (_stricmp("Ambience", name) == 0) {
        return 4;
    }
    if (_stricmp("Dialog", name) == 0) {
        return 5;
    }
    return _stricmp("Talking Head", name) == 0 ? 8 : 3;
}

// A UI sound by kit; category 8 (Talking Head) plays as a voice-over.
// ref: FUN_1412e6120
uint32_t SI3PlayUISound(int32_t kitId, int32_t category, bool noDuplicates, int32_t* handle, const int32_t* priority,
    bool hasPriority) {
    SE3PLAYPARAMS params;
    InitParams(&params);
    params.m_bus = SI3CategoryBus(category);
    if (params.m_bus == -1) {
        params.m_bus = SE3BUS_MASTER;
    }
    params.m_13d = 1;
    params.m_24 = 0x69;
    if (noDuplicates) {
        params.m_duplicates = 2;
    }
    params.m_136 = 0;
    uint32_t result;
    if (category == 8) {
        SE3VOICEOVERPRIORITY voiceOver = {0xd, 1};
        if (hasPriority) {
            voiceOver.m_priority = *priority;
        }
        result = SE3PlayVoiceOver(static_cast<uint32_t>(kitId), nullptr, &params, handle, "SI3::PlayUISound", true,
            nullptr, &voiceOver);
    } else {
        result = SE3PlayKit(static_cast<uint32_t>(kitId), nullptr, &params, handle, nullptr, false, nullptr);
    }
    SE3PlayParamsDestroy(&params);
    return result;
}

// A sound file (no kit) on a bus.
// ref: FUN_1412cdfa0
uint32_t SI3PlaySoundFile(int32_t fileDataId, int32_t* handle, int8_t bus, int32_t mode) {
    SE3PLAYPARAMS params;
    InitParams(&params);
    params.m_13c = bus == 8;
    params.m_prefetchOnly = 0;
    params.m_bus = bus;
    params.m_3dMode = mode;
    params.m_fileDataId = fileDataId;
    uint32_t result = 8;
    if (fileDataId != 0) {
        if (SE3BusIsUnder(bus, SE3BUS_MUSIC)) {
            params.m_136 = 0;
        }
        result = SE3PlayKit(0xffffff80, nullptr, &params, handle, nullptr, false, nullptr);
    }
    SE3PlayParamsDestroy(&params);
    return result;
}

// The glue's music: a new kit stops the old one and the credits.
// ref: FUN_1412f2140
void SI3PlayGlueMusic(int32_t kitId) {
    if (ClientServicesIsInWorld()) {
        return;
    }
    if (SE3IsPlaying(static_cast<uint32_t>(s_glueMusicHandle))) {
        if (kitId == SE3GetKitId(static_cast<uint32_t>(s_glueMusicHandle))) {
            return;
        }
        s_glueMusicStopped = 1;
        SE3Stop(static_cast<uint32_t>(s_glueMusicHandle), 4, 0, nullptr);
    }
    s_creditsStopped = 1;
    SE3StopNow(static_cast<uint32_t>(s_creditsHandle), nullptr);
    if (0 < kitId) {
        s_glueMusicKit = kitId;
        SE3PLAYPARAMS params;
        InitParams(&params);
        params.m_bus = SI3GlueBus(2);
        if (params.m_bus == -1) {
            params.m_bus = SE3BUS_MASTER;
        }
        params.m_3dMode = 1;
        params.m_13c = 1;
        params.m_fadeOutType = 8;
        SE3PlayKit(static_cast<uint32_t>(kitId), nullptr, &params, &s_glueMusicHandle, nullptr, true, nullptr);
        s_glueMusicStopped = 0;
        SE3PlayParamsDestroy(&params);
    }
}

// The glue's ambience, fading in over `fadeMs`.
// ref: FUN_1412f1db0
void SI3PlayGlueAmbienceKit(int32_t kitId, int32_t fadeMs) {
    if (kitId < 1) {
        return;
    }
    s_glueAmbienceKit = kitId;
    SE3PLAYPARAMS params;
    InitParams(&params);
    params.m_bus = SI3GlueBus(0);
    if (params.m_bus == -1) {
        params.m_bus = SE3BUS_MASTER;
    }
    params.m_3dMode = 1;
    params.m_fadeInType = 0xb;
    params.m_13c = 1;
    params.m_fadeOutType = 6;
    params.m_fadeInMs = fadeMs;
    SE3PlayKit(static_cast<uint32_t>(kitId), nullptr, &params, &s_glueAmbienceHandle, nullptr, true, nullptr);
    SE3PlayParamsDestroy(&params);
}

// ref: FUN_1412f20e0
void SI3PlayGlueAmbience(int32_t kitId, int32_t fadeMs) {
    if (SE3IsPlaying(static_cast<uint32_t>(s_glueAmbienceHandle))) {
        if (kitId == SE3GetKitId(static_cast<uint32_t>(s_glueAmbienceHandle))) {
            return;
        }
        s_glueAmbienceStopped = 1;
        SE3StopNow(static_cast<uint32_t>(s_glueAmbienceHandle), nullptr);
    }
    if (0 < kitId) {
        s_glueAmbienceStopped = 0;
        SI3PlayGlueAmbienceKit(kitId, fadeMs);
    }
}

// ref: FUN_1412f2300
void SI3StopGlueAmbience() {
    s_glueAmbienceStopped = 1;
    SE3StopNow(static_cast<uint32_t>(s_glueAmbienceHandle), nullptr);
}

// ref: FUN_1412f22e0
void SI3StopCreditsMusic() {
    s_creditsStopped = 1;
    SE3StopNow(static_cast<uint32_t>(s_creditsHandle), nullptr);
}

// ref: FUN_1412f2320
void SI3StopGlueMusic(int32_t fadeType) {
    s_glueMusicStopped = 1;
    SE3Stop(static_cast<uint32_t>(s_glueMusicHandle), fadeType, 0, nullptr);
}

// ref: FUN_1412f1f00
bool SI3SetCreditsExpansion(uint32_t expansion) {
    if (expansion < 3) {
        s_creditsExpansion = expansion;
        return true;
    }
    return false;
}

// The credits' `track`-th song of the expansion.
// ref: FUN_1412f1f20
void SI3PlayCreditsMusic(uint32_t track) {
    if (2 < s_creditsExpansion) {
        return;
    }
    int32_t kitId = 0;
    if (s_creditsExpansion == 0) {
        s_creditsTrack = static_cast<int32_t>(track % 7);
        kitId = s_credits0[s_creditsTrack];
    } else if (s_creditsExpansion == 1) {
        s_creditsTrack = static_cast<int32_t>(track % 10);
        kitId = s_credits1[s_creditsTrack];
    } else {
        s_creditsTrack = static_cast<int32_t>(track % 10);
        kitId = s_credits2[s_creditsTrack];
    }
    SE3PLAYPARAMS params;
    InitParams(&params);
    params.m_bus = SI3GlueBus(1);
    params.m_10 = 0;
    if (params.m_bus == -1) {
        params.m_bus = SE3BUS_MASTER;
    }
    params.m_13c = 1;
    params.m_fadeOutType = 4;
    SE3PlayKit(static_cast<uint32_t>(kitId), nullptr, &params, &s_creditsHandle, nullptr, true, nullptr);
    s_creditsStopped = 0;
    SE3PlayParamsDestroy(&params);
}

// The zone music's state reset (FUN_1412fcbe0); the zone music is the world's.
// ref: FUN_1412fcbe0
void SI3ResetZoneMusic() {
    FROST_UNIMPLEMENTED();  // the zone music's state (0x1430a9c3c..0x1430a9c64)
}

// ref: FUN_1412fcd00
void SI3StopZoneMusic() {
    FROST_UNIMPLEMENTED();  // the zone music's handles (0x142cfd2e0..0x142cfd3bc)
}

// PlayMusic: a file on the music bus, taking over from the zone music.
// ref: FUN_1412f97f0
void SI3PlayMusicFile(int32_t fileDataId) {
    if (fileDataId == 0) {
        return;
    }
    int8_t bus = SI3CategoryBus(7);
    if (bus == -1) {
        bus = SE3BUS_MASTER;
    }
    if (SI3PlaySoundFile(fileDataId, &s_musicHandle, bus, 1) == 0) {
        SI3StopZoneMusic();
        SI3ResetZoneMusic();
    }
}

// ref: FUN_1412fcc70
void SI3StopMusicFile() {
    SE3StopNow(static_cast<uint32_t>(s_musicHandle), nullptr);
}

// ref: FUN_1412e6c50
void SI3StopMusic() {
    SI3StopZoneMusic();
    SI3StopMusicFile();
    SI3ResetZoneMusic();
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142cfd2c8 = (void*)&s_creditsStopped;
extern "C" void* const gen_p_142cfd2c9 = (void*)&s_glueMusicStopped;
extern "C" void* const gen_p_142cfd2ca = (void*)&s_glueAmbienceStopped;
extern "C" void* const gen_p_142cfd2cc = (void*)&s_creditsTrack;
extern "C" void* const gen_p_1430a9928 = (void*)&s_creditsHandle;
extern "C" void* const gen_p_1430a992c = (void*)&s_glueMusicKit;
extern "C" void* const gen_p_1430a9930 = (void*)&s_glueMusicHandle;
extern "C" void* const gen_p_1430a9934 = (void*)&s_glueAmbienceKit;
extern "C" void* const gen_p_1430a9938 = (void*)&s_glueAmbienceHandle;
extern "C" void* const gen_p_1430a99f4 = (void*)&s_musicHandle;
// ---- end of generated globals ----
