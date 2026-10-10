// Sound_GameSystem / Sound_ChatSystem: the sound device bindings the audio options read. The
// chat system has no devices in this client and answers "None".

#include <cstring>

#include "framescript/FrameScript.hpp"
#include "glue/ScriptAPIHandlers.hpp"
#include "sound/SE3.hpp"
#include "sound/SI3.hpp"
#include "sound/SE3Kit.hpp"

#include "cas/CAS.hpp"
#include "db/DB2Tables.hpp"
#include "db/WowClientDB2.hpp"
#include "db/WowDB2Lookup.hpp"
#include "framescript/EventCallbacks.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

bool ClientIsLoadingScreenActive();

namespace {

// The bounded copy the handlers inline: at most size - 1 characters, always terminated.
void CopyText(char* dest, const char* source, uint64_t size) {
    uint64_t i = 0;
    for (; i + 1 < size && source[i] != '\0'; i++) {
        dest[i] = source[i];
    }
    dest[i] = '\0';
}

}

// ref: FUN_141309ff0
int Lua_Sound_ChatSystem_GetInputDriverNameByIndex(lua_State* L) {
    if (lua_isnumber(L, 1) != 0) {
        char name[0x800] = "None";
        lua_pushstring(L, name);
        return 1;
    }
    luaL_error(L, "Usage: Sound_ChatSystem_GetInputDriverNameByIndex(InputDriverIndex)");
    return 0;
}

// ref: FUN_14130add0
int Lua_Sound_ChatSystem_GetOutputDriverNameByIndex(lua_State* L) {
    if (lua_isnumber(L, 1) != 0) {
        char name[0x800] = "None";
        lua_pushstring(L, name);
        return 1;
    }
    luaL_error(L, "Usage: Sound_ChatSystem_GetOutputDriverNameByIndex(OutputDriverIndex)");
    return 0;
}

// ref: FUN_141307e00
int Lua_Sound_GameSystem_GetInputDriverNameByIndex(lua_State* L) {
    if (lua_isnumber(L, 1) != 0) {
        lua_pushstring(L, "None");
        return 1;
    }
    luaL_error(L, "Usage: Sound_GameSystem_GetInputDriverNameByIndex(InputDriverIndex)");
    return 0;
}

// ref: FUN_1413077a0
int Lua_Sound_GameSystem_GetNumInputDrivers(lua_State* L) {
    lua_pushnumber(L, 0.0);
    return 1;
}

// ref: FUN_141308ad0
int Lua_Sound_GameSystem_GetNumOutputDrivers(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(SE3GetNumOutputDrivers()));
    return 1;
}

// The driver's name; the system default shows as the localized SYSTEM_DEFAULT, and a driver
// that is not there as NONE.
// ref: FUN_141309140
int Lua_Sound_GameSystem_GetOutputDriverNameByIndex(lua_State* L) {
    if (lua_isnumber(L, 1) != 0) {
        int32_t index = static_cast<int32_t>(lua_tonumber(L, 1));
        char name[0x800];
        CopyText(name, FrameScript_GetText("NONE", -1, 0), 0x800);
        bool isDefault = false;
        SE3GetOutputDriverName(index, name, 0x800, &isDefault);
        if (isDefault) {
            CopyText(name, FrameScript_GetText("SYSTEM_DEFAULT", -1, 0), 0x800);
        }
        lua_pushstring(L, name);
        return 1;
    }
    luaL_error(L, "Usage: Sound_GetOutputDriverNameByIndex(OutputDriverIndex)");
    return 0;
}

// ref: FUN_14130bbb0
int Lua_Sound_GameSystem_RestartSoundSystem(lua_State* L) {
    (void)L;
    SI3RequestRestart();
    return 0;
}

namespace {

// A file argument: a FileDataID, or a path looked up in the manifest.
int32_t FileArgument(lua_State* L, int32_t index, bool* given) {
    *given = true;
    if (lua_isnumber(L, index) != 0) {
        return static_cast<int32_t>(lua_tonumber(L, index));
    }
    if (lua_isstring(L, index) != 0) {
        return cas::CASFileGetIdByName(lua_tostring(L, index), 1);
    }
    *given = false;
    return 0;
}

// LAB_14130c3a0: a PlaySound asked to hear when it ends: SOUNDKIT_FINISHED with its handle.
void SoundKitFinished(SE3SoundKitObject* object) {
    if (object != nullptr) {
        int32_t handle = object->m_handle;
        EventCallbacks_Fire(0x2370, 0x5852baf6871ac08full, &handle);
    }
}

void InvokeSoundKitFinished(SE3SoundKitObject* object, void* storage) {
    (*static_cast<void (**)(SE3SoundKitObject*)>(storage))(object);
}

}

// ref: FUN_1416a4320
int Lua_PlayGlueMusic(lua_State* L) {
    if (lua_isnumber(L, 1) == 0) {
        luaL_error(L, "Usage: PlayGlueMusic(\"SoundKit ID\")");
        return 0;
    }
    SI3PlayGlueMusic(static_cast<int32_t>(lua_tonumber(L, 1)));
    return 0;
}

// ref: FUN_1416a4370
int Lua_PlayGlueAmbience(lua_State* L) {
    if (lua_isnumber(L, 1) == 0) {
        luaL_error(L, "Usage: PlayGlueAmbience(\"SoundKit ID\", \"(optional)fade in time\")");
        return 0;
    }
    int32_t fadeMs = -1;
    if (lua_isnumber(L, 2) != 0) {
        fadeMs = static_cast<int32_t>(static_cast<float>(lua_tonumber(L, 2)) * 1000.0f);
    }
    SI3PlayGlueAmbience(static_cast<int32_t>(lua_tonumber(L, 1)), fadeMs);
    return 0;
}

// ref: FUN_1416a4410
int Lua_StopGlueAmbience(lua_State* L) {
    (void)L;
    SI3StopGlueAmbience();
    return 0;
}

// ref: FUN_1416a4420
int Lua_PlayCreditsMusic(lua_State* L) {
    if (lua_isnumber(L, 1) == 0) {
        luaL_error(L, "Usage: PlayCreditsMusic(expansionID)");
        return 0;
    }
    if (SI3SetCreditsExpansion(static_cast<uint32_t>(lua_tonumber(L, 1)))) {
        SI3StopCreditsMusic();
        SI3StopGlueMusic(0);
        SI3PlayCreditsMusic(0);
    }
    return 0;
}

// ref: FUN_1416a4480
int Lua_StopGlueMusic(lua_State* L) {
    (void)L;
    SI3StopCreditsMusic();
    SI3StopGlueMusic(0);
    return 0;
}

// PlaySound(soundKitID [, channel, forceNoDuplicates, runFinishCallback, overridePriority]):
// willPlay and the sound's handle.
// ref: FUN_141300e60
int Lua_PlaySound(lua_State* L) {
    if (lua_isnumber(L, 1) == 0) {
        luaL_error(L, "Usage: PlaySound(soundKitID [, \"channel\", forceNoDuplicates, runFinishCallback])");
        return 0;
    }
    // DAT_14311c528 / FUN_141925900: a game state that silences script sounds; never set before the world
    if (ClientIsLoadingScreenActive()) {
        return 0;
    }
    int32_t kitId = static_cast<int32_t>(lua_tonumber(L, 1));
    int32_t category = 0;
    if (lua_isstring(L, 2) != 0) {
        category = SI3CategoryFromName(lua_tostring(L, 2));
    }
    bool noDuplicates = false;
    if (lua_type(L, 3) == LUA_TBOOLEAN) {
        noDuplicates = lua_toboolean(L, 3) != 0;
    }
    int32_t priority = 0;
    bool hasPriority = false;
    if (lua_isnumber(L, 5) != 0) {
        priority = static_cast<int32_t>(lua_tonumber(L, 5));
        hasPriority = true;
    }
    int32_t handle = 0;
    uint32_t result = SI3PlayUISound(kitId, category, noDuplicates, &handle, &priority, hasPriority);
    if (result != 0 && result != 0x13) {
        return 0;
    }
    if (lua_type(L, 4) == LUA_TBOOLEAN && lua_toboolean(L, 4) != 0) {
        blz::function_storage callback;
        callback.m_invoke = reinterpret_cast<void*>(&InvokeSoundKitFinished);
        callback.m_manager = &blz::function_trivial_manager;
        void* function = reinterpret_cast<void*>(&SoundKitFinished);
        std::memcpy(callback.m_storage, &function, sizeof(function));
        SE3SetOnEnded(static_cast<uint32_t>(handle), &callback, 0);
    }
    lua_pushboolean(L, 1);
    lua_pushnumber(L, static_cast<double>(static_cast<uint32_t>(handle)));
    return 2;
}

// PlaySoundFile(file [, channel]): willPlay and the handle.
// ref: FUN_141303d20
int Lua_PlaySoundFile(lua_State* L) {
    bool given;
    int32_t fileDataId = FileArgument(L, 1, &given);
    if (!given) {
        luaL_error(L, "Usage: PlaySoundFile(\"soundFile\" or soundFileID, optional[\"SFX\",\"Music\",\"Ambience\" or \"Master\"])");
    }
    int32_t category = 3;
    if (lua_isstring(L, 2) != 0) {
        category = SI3CategoryFromName(lua_tostring(L, 2));
    }
    if (fileDataId != 0) {
        int8_t bus = SI3CategoryBus(category);
        if (bus == -1) {
            bus = SE3BUS_MASTER;
        }
        int32_t handle = 0;
        uint32_t result = SI3PlaySoundFile(fileDataId, &handle, bus, 2);
        if (result == 0 || result == 0x13) {
            lua_pushboolean(L, 1);
            lua_pushnumber(L, static_cast<double>(static_cast<uint32_t>(handle)));
            return 2;
        }
    }
    return 0;
}

// StopSound(handle [, fadeOutMs]).
// ref: FUN_141306a90
int Lua_StopSound(lua_State* L) {
    if (lua_isnumber(L, 1) == 0) {
        return 0;
    }
    if (lua_isnumber(L, 2) == 0) {
        SE3StopNow(static_cast<uint32_t>(static_cast<int64_t>(lua_tonumber(L, 1))), nullptr);
    } else {
        int32_t fadeMs = static_cast<int32_t>(lua_tonumber(L, 2));
        SE3Stop(static_cast<uint32_t>(static_cast<int64_t>(lua_tonumber(L, 1))), 0xb, fadeMs, nullptr);
    }
    return 0;
}

// PlayMusic(file): true.
// ref: FUN_141303660
int Lua_PlayMusic(lua_State* L) {
    bool given;
    int32_t fileDataId = FileArgument(L, 1, &given);
    if (!given) {
        luaL_error(L, "Usage: PlayMusic(\"musicfile\" or fileDataID)");
    }
    SI3PlayMusicFile(fileDataId);
    lua_pushboolean(L, 1);
    return 1;
}

// ref: FUN_141306450
int Lua_StopMusic(lua_State* L) {
    (void)L;
    SI3StopMusicFile();
    return 0;
}

// ref: FUN_141304a90
int Lua_MuteSoundFile(lua_State* L) {
    bool given;
    int32_t fileDataId = FileArgument(L, 1, &given);
    if (!given) {
        luaL_error(L, "Usage: MuteSoundFile(\"soundFile\" or fileDataID)");
    }
    if (fileDataId == 0) {
        luaL_error(L, "MuteSoundFile Error - Invalid fileDataID for sound.");
        return 0;
    }
    SE3MuteFile(fileDataId, true);  // FUN_1412cda50
    return 0;
}

// ref: FUN_141305770
int Lua_UnmuteSoundFile(lua_State* L) {
    bool given;
    int32_t fileDataId = FileArgument(L, 1, &given);
    if (!given) {
        luaL_error(L, "Usage: UnmuteSoundFile(\"soundFile\" or fileDataID)");
    }
    if (fileDataId == 0) {
        luaL_error(L, "UnmuteSoundFile Error - Invalid fileDataID for sound.");
        return 0;
    }
    SE3MuteFile(fileDataId, false);
    return 0;
}

// The count of a kit's SoundKitEntry records.
// ref: FUN_141302890
int Lua_GetSoundEntryCount(lua_State* L) {
    if (lua_isnumber(L, 1) == 0) {
        luaL_error(L, "Usage: GetSoundEntryCount(soundKitID)");
        return 0;
    }
    int32_t kitId = static_cast<int32_t>(lua_tonumber(L, 1));
    int32_t count = 0;
    if (WowDB2ParentLookup* entries = g_soundKitEntryDB.m_parentLookup) {
        DB2PARENTITER it;
        DB2PARENTITER end;
        entries->Begin(&it, kitId);
        entries->End(&end);
        while (it.m_entry != end.m_entry || it.m_overflow != end.m_overflow || it.m_parent != end.m_parent
               || it.m_source != end.m_source) {
            count++;
            entries->Next(&it);
        }
    }
    lua_pushinteger(L, count);
    return 1;
}

