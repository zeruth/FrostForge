// The Lua methods of animation groups, animations and path control points (the method
// tables 0x142cf70c0 Animation, 0x142cf73e0 AnimationGroup, 0x142cf7300 Alpha, 0x142cf7340
// FlipBook, 0x142cf7650 Scale, 0x142cf76f0 Translation, 0x142cf75a0 Path, 0x142cf75f0
// Rotation, 0x142cf76d0 TextureCoordTranslation, 0x142cf7cd0 ControlPoint), and the
// metatables each class gets.

#include "ui/CSimpleAnim.hpp"

#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "framescript/ScriptMethod.hpp"
#include "ui/CAnimatableObject.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

void CScriptObject_FillScriptMethods(lua_State* L);

namespace {

using namespace ScriptMethod;

CSimpleAnim* AnimSelf(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CSimpleAnim*>(Self(L, CSimpleAnim::GetObjectType(), index, usage));
}

CSimpleAnimGroup* GroupSelf(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CSimpleAnimGroup*>(Self(L, CSimpleAnimGroup::GetObjectType(), index, usage));
}

// A name from a table of names: present, a string, and known, or the usage error.
template <class T, class Parse>
T RequiredName(lua_State* L, int32_t* index, const char* usage, Parse parse) {
    if (lua_type(L, *index) < 1) {
        luaL_error(L, usage);
    }
    T value = T();
    if (lua_type(L, *index) != LUA_TSTRING || !parse(lua_tostring(L, *index), &value)) {
        luaL_argerror(L, *index, usage);
    }
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
    }
    (*index)++;
    return value;
}

// Two plain numbers (the template's paired number argument).
void RequiredPair(lua_State* L, int32_t* index, const char* usage, float* pair) {
    if (lua_type(L, *index) < 1) {
        luaL_error(L, usage);
    }
    if (!lua_isnumber(L, *index) || !lua_isnumber(L, *index + 1)) {
        luaL_argerror(L, *index, usage);
    }
    if (!lua_checkstack(L, 2)) {
        luaL_error(L, "stack overflow");
    }
    pair[0] = static_cast<float>(lua_tonumber(L, *index));
    pair[1] = static_cast<float>(lua_tonumber(L, *index + 1));
    *index += 2;
}

int32_t PushPair(lua_State* L, const float* pair) {
    if (!lua_checkstack(L, 2)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    lua_pushnumber(L, static_cast<double>(pair[0]));
    lua_pushnumber(L, static_cast<double>(pair[1]));
    return 2;
}

// The generic handler methods: self of the class's type, then the object's own method.
template <int32_t (*Type)(), int32_t (FrameScript_Object::*Method)(lua_State*)>
int ScriptBody(lua_State* L) {
    const char* error = nullptr;
    FrameScript_Object* object = FrameScript_Object::GetObjectFromLua(&error, L, Type(), 1, false, false);
    if (error != nullptr) {
        FrameScript_ActionError(error);
    }
    int32_t results = object != nullptr ? (object->*Method)(L) : 0;
    RaisePendingError(L, nullptr, nullptr);
    return results;
}

template <class T>
int HasScriptMethod(lua_State* L, int32_t type) {
    int32_t index = 1;
    const char* usage = "Usage: local hasScript = self:HasScript(scriptName)";
    auto object = static_cast<T*>(Self(L, type, &index, usage));
    const char* name = RequiredString(L, &index, usage);
    bool has = object->HasScript(name);
    RaisePendingError(L, object, "HasScript");
    return PushBool(L, has);
}

// ---- Animation -----------------------------------------------------------------------------

// ref: FUN_14083a350
int Anim_GetDuration(lua_State* L) {
    int32_t index = 1;
    CSimpleAnim* anim = AnimSelf(L, &index, "Usage: local durationSec = self:GetDuration()");
    float value = anim->m_duration;
    RaisePendingError(L, anim, "GetDuration");
    return PushFloat(L, value);
}

// ref: FUN_14083b0d0
int Anim_GetElapsed(lua_State* L) {
    int32_t index = 1;
    CSimpleAnim* anim = AnimSelf(L, &index, "Usage: local elapsedSec = self:GetElapsed()");
    float value = anim->m_elapsed;
    RaisePendingError(L, anim, "GetElapsed");
    return PushFloat(L, value);
}

// ref: FUN_14083be50
int Anim_GetEndDelay(lua_State* L) {
    int32_t index = 1;
    CSimpleAnim* anim = AnimSelf(L, &index, "Usage: local delaySec = self:GetEndDelay()");
    float value = anim->m_endDelay;
    RaisePendingError(L, anim, "GetEndDelay");
    return PushFloat(L, value);
}

// ref: FUN_14083cbd0
int Anim_GetOrder(lua_State* L) {
    int32_t index = 1;
    CSimpleAnim* anim = AnimSelf(L, &index, "Usage: local order = self:GetOrder()");
    int32_t value = anim->m_order;
    RaisePendingError(L, anim, "GetOrder");
    return PushInt(L, value);
}

// ref: FUN_14083d950
int Anim_GetProgress(lua_State* L) {
    int32_t index = 1;
    CSimpleAnim* anim = AnimSelf(L, &index, "Usage: local progress = self:GetProgress()");
    float value = anim->m_progress;
    RaisePendingError(L, anim, "GetProgress");
    return PushFloat(L, value);
}

// ref: FUN_14083e6d0
int Anim_GetRegionParent(lua_State* L) {
    int32_t index = 1;
    CSimpleAnim* anim = AnimSelf(L, &index, "Usage: local region = self:GetRegionParent()");
    CScriptObject* region = anim->GetRegionParent();
    RaisePendingError(L, anim, "GetRegionParent");
    return PushObject(L, region);
}

// ref: FUN_14083f450
int Anim_GetScript(lua_State* L) {
    return ScriptBody<CSimpleAnim::GetObjectType, &FrameScript_Object::GetScript>(L);
}

// ref: FUN_140840100
int Anim_GetSmoothProgress(lua_State* L) {
    int32_t index = 1;
    CSimpleAnim* anim = AnimSelf(L, &index, "Usage: local progress = self:GetSmoothProgress()");
    float value = anim->m_smoothProgress;
    RaisePendingError(L, anim, "GetSmoothProgress");
    return PushFloat(L, value);
}

// ref: FUN_140840e80
int Anim_GetSmoothing(lua_State* L) {
    int32_t index = 1;
    CSimpleAnim* anim = AnimSelf(L, &index, "Usage: local weights = self:GetSmoothing()");
    float weights[2];
    anim->GetSmoothing(weights);
    RaisePendingError(L, anim, "GetSmoothing");
    return PushStringOrNil(L, AnimSmoothingToString(weights));
}

// ref: FUN_140841c60
int Anim_GetStartDelay(lua_State* L) {
    int32_t index = 1;
    CSimpleAnim* anim = AnimSelf(L, &index, "Usage: local delaySec = self:GetStartDelay()");
    float value = anim->m_startDelay;
    RaisePendingError(L, anim, "GetStartDelay");
    return PushFloat(L, value);
}

// ref: FUN_1408429e0
int Anim_GetTarget(lua_State* L) {
    int32_t index = 1;
    CSimpleAnim* anim = AnimSelf(L, &index, "Usage: local target = self:GetTarget()");
    CScriptObject* target = anim->GetTarget();
    RaisePendingError(L, anim, "GetTarget");
    return PushObject(L, target);
}

// ref: FUN_140843760
int Anim_HasScript(lua_State* L) {
    return HasScriptMethod<CSimpleAnim>(L, CSimpleAnim::GetObjectType());
}

// ref: FUN_140844550
int Anim_HookScript(lua_State* L) {
    return ScriptBody<CSimpleAnim::GetObjectType, &FrameScript_Object::HookScript>(L);
}

// ref: FUN_140845200
int Anim_IsDelaying(lua_State* L) {
    int32_t index = 1;
    CSimpleAnim* anim = AnimSelf(L, &index, "Usage: local isDelaying = self:IsDelaying()");
    bool value = anim->IsDelaying();
    RaisePendingError(L, anim, "IsDelaying");
    return PushBool(L, value);
}

// ref: FUN_140845f80
int Anim_IsDone(lua_State* L) {
    int32_t index = 1;
    CSimpleAnim* anim = AnimSelf(L, &index, "Usage: local isDone = self:IsDone()");
    bool value = anim->m_state == CSimpleAnim::STATE_DONE;
    RaisePendingError(L, anim, "IsDone");
    return PushBool(L, value);
}

// ref: FUN_140846d00
int Anim_IsPaused(lua_State* L) {
    int32_t index = 1;
    CSimpleAnim* anim = AnimSelf(L, &index, "Usage: local isPaused = self:IsPaused()");
    bool value = anim->m_state == CSimpleAnim::STATE_PAUSED;
    RaisePendingError(L, anim, "IsPaused");
    return PushBool(L, value);
}

// ref: FUN_140847a80
int Anim_IsPlaying(lua_State* L) {
    int32_t index = 1;
    CSimpleAnim* anim = AnimSelf(L, &index, "Usage: local isPlaying = self:IsPlaying()");
    bool value = anim->m_state == CSimpleAnim::STATE_PLAYING;
    RaisePendingError(L, anim, "IsPlaying");
    return PushBool(L, value);
}

// ref: FUN_140848800
int Anim_IsStopped(lua_State* L) {
    int32_t index = 1;
    CSimpleAnim* anim = AnimSelf(L, &index, "Usage: local isStopped = self:IsStopped()");
    bool value = anim->m_state == CSimpleAnim::STATE_STOPPED;
    RaisePendingError(L, anim, "IsStopped");
    return PushBool(L, value);
}

// ref: FUN_140849580
int Anim_Pause(lua_State* L) {
    int32_t index = 1;
    CSimpleAnim* anim = AnimSelf(L, &index, "Usage: self:Pause()");
    anim->Pause();
    RaisePendingError(L, anim, "Pause");
    return 0;
}

// ref: FUN_14084a2f0
int Anim_Play(lua_State* L) {
    int32_t index = 1;
    CSimpleAnim* anim = AnimSelf(L, &index, "Usage: self:Play()");
    anim->Play();
    RaisePendingError(L, anim, "Play");
    return 0;
}

// ref: FUN_14084b060
int Anim_Restart(lua_State* L) {
    int32_t index = 1;
    CSimpleAnim* anim = AnimSelf(L, &index, "Usage: self:Restart()");
    anim->Stop(true);
    anim->Play();
    RaisePendingError(L, anim, "Restart");
    return 0;
}

// ref: FUN_14084bde0
int Anim_SetChildKey(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: local success = self:SetChildKey(childKey)";
    CSimpleAnim* anim = AnimSelf(L, &index, usage);
    const char* key = RequiredString(L, &index, usage);
    bool success = anim->SetChildKey(key);
    RaisePendingError(L, anim, "SetChildKey");
    return PushBool(L, success);
}

// ref: FUN_14084cbd0
int Anim_SetDuration(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetDuration(durationSec [, recomputeGroupDuration])";
    CSimpleAnim* anim = AnimSelf(L, &index, usage);
    float duration = RequiredFinite(L, &index, usage);
    bool recompute = lua_type(L, index) >= 1 ? OptionalBool(L, &index) : true;
    anim->SetDuration(duration, recompute);
    RaisePendingError(L, anim, "SetDuration");
    return 0;
}

// ref: FUN_14084da10
int Anim_SetEndDelay(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetEndDelay(delaySec [, recomputeGroupDuration])";
    CSimpleAnim* anim = AnimSelf(L, &index, usage);
    float delay = RequiredFinite(L, &index, usage);
    bool recompute = lua_type(L, index) >= 1 ? OptionalBool(L, &index) : true;
    anim->SetEndDelay(delay, recompute);
    RaisePendingError(L, anim, "SetEndDelay");
    return 0;
}

// ref: FUN_14084e850
int Anim_SetOrder(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetOrder(newOrder)";
    CSimpleAnim* anim = AnimSelf(L, &index, usage);
    int32_t order = RequiredInt32(L, &index, usage);
    anim->SetOrder(order);
    RaisePendingError(L, anim, "SetOrder");
    return 0;
}

// The parent group, by name or as an object.
// ref: FUN_1424c4f00
CSimpleAnimGroup* SetParentArgs(lua_State* L, CSimpleAnim** anim) {
    const char* error = nullptr;
    *anim = static_cast<CSimpleAnim*>(
        FrameScript_Object::GetObjectFromLua(&error, L, CSimpleAnim::GetObjectType(), 1, false, false));
    if (*anim == nullptr) {
        FrameScript_ActionError("Invalid object for SetParent() call. (%s)", error != nullptr ? error : "");
        return nullptr;
    }
    CSimpleAnimGroup* group = nullptr;
    int type = lua_type(L, 2);
    if (type == LUA_TNONE || type == LUA_TNIL) {
        FrameScript_ObjectError(*anim, "SetParent", "Cannot set a 'nil' parent");
        return nullptr;
    }
    if (type == LUA_TSTRING) {
        const char* name = lua_tostring(L, 2);
        bool allowLockdown = ((*anim)->m_flags & FSOBJ_LOCKDOWN) || !((*anim)->m_flags & FSOBJ_ACCESSIBLE);
        lua_State* state = FrameScript_GetContext();
        lua_pushstring(state, name);
        lua_rawget(state, LUA_GLOBALSINDEX);
        group = static_cast<CSimpleAnimGroup*>(FrameScript_Object::GetObjectFromLua(
            nullptr, state, CSimpleAnimGroup::GetObjectType(), -1, false, allowLockdown));
        lua_settop(state, -2);
        if (group == nullptr) {
            FrameScript_ObjectError(*anim, "SetParent", "Couldn't find object named '%s'", name != nullptr ? name : "");
        }
    } else if (type == LUA_TTABLE) {
        const char* groupError = nullptr;
        group = static_cast<CSimpleAnimGroup*>(
            FrameScript_Object::GetObjectFromLua(&groupError, L, CSimpleAnimGroup::GetObjectType(), 2, false, false));
        if (groupError != nullptr) {
            FrameScript_ActionError(groupError);
        }
    }
    return group;
}

// An optional integer argument (luaL_optinteger with the template's checks).
// ref: FUN_1424252d0
int32_t OptionalInteger(lua_State* L, int32_t index, int32_t fallback) {
    if (lua_type(L, index) < 1) {
        return fallback;
    }
    auto value = static_cast<int32_t>(lua_tointeger(L, index));
    if (value == 0 && !lua_isnumber(L, index)) {
        luaL_typerror(L, index, lua_typename(L, LUA_TNUMBER));
    }
    return value;
}

// ref: FUN_14084f640
int Anim_SetParent(lua_State* L) {
    CSimpleAnim* anim = nullptr;
    CSimpleAnimGroup* group = SetParentArgs(L, &anim);
    if (group != nullptr) {
        int32_t order = OptionalInteger(L, 3, anim->m_order);
        anim->SetParentGroup(group, order);
    }
    RaisePendingError(L, nullptr, nullptr);
    return 0;
}

// ref: FUN_1408502f0
int Anim_SetPlaying(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetPlaying(play)";
    CSimpleAnim* anim = AnimSelf(L, &index, usage);
    bool play = RequiredBool(L, &index, usage);
    if (!play) {
        anim->Stop(true);
    } else {
        anim->Play();
    }
    RaisePendingError(L, anim, "SetPlaying");
    return 0;
}

// ref: FUN_1408510d0
int Anim_SetScript(lua_State* L) {
    return ScriptBody<CSimpleAnim::GetObjectType, &FrameScript_Object::SetScript>(L);
}

// ref: FUN_140851d80
int Anim_SetSmoothProgress(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetSmoothProgress(durationSec)";
    CSimpleAnim* anim = AnimSelf(L, &index, usage);
    float progress = RequiredFinite(L, &index, usage);
    anim->SetSmoothProgress(progress);
    RaisePendingError(L, anim, "SetSmoothProgress");
    return 0;
}

// ref: FUN_140852b70
int Anim_SetSmoothing(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetSmoothing(weights)";
    CSimpleAnim* anim = AnimSelf(L, &index, usage);
    struct WEIGHTS {
        float m_values[2] = {0.0f, 0.0f};
    };
    auto parse = [](const char* name, WEIGHTS* weights) {
        return StringToAnimSmoothing(name, weights->m_values);
    };
    WEIGHTS weights = RequiredName<WEIGHTS>(L, &index, usage, parse);
    anim->SetSmoothing(weights.m_values);
    RaisePendingError(L, anim, "SetSmoothing");
    return 0;
}

// ref: FUN_140853930
int Anim_SetStartDelay(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetStartDelay(delaySec [, recomputeGroupDuration])";
    CSimpleAnim* anim = AnimSelf(L, &index, usage);
    float delay = RequiredFinite(L, &index, usage);
    bool recompute = lua_type(L, index) >= 1 ? OptionalBool(L, &index) : true;
    anim->SetStartDelay(delay, recompute);
    RaisePendingError(L, anim, "SetStartDelay");
    return 0;
}

// ref: FUN_140854770
int Anim_SetTarget(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: local success = self:SetTarget(target)";
    CSimpleAnim* anim = AnimSelf(L, &index, usage);
    if (lua_type(L, index) < 1) {
        luaL_error(L, usage);
    }
    auto target = static_cast<CScriptObject*>(
        FrameScript_Object::GetObjectFromLua(nullptr, L, CScriptObject::GetObjectType(), index, false, false));
    if (target == nullptr) {
        luaL_argerror(L, index, usage);
    }
    bool success = anim->SetTarget(target);
    RaisePendingError(L, anim, "SetTarget");
    return PushBool(L, success);
}

// ref: FUN_140855610
int Anim_SetTargetKey(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: local success = self:SetTargetKey(key)";
    CSimpleAnim* anim = AnimSelf(L, &index, usage);
    const char* key = RequiredString(L, &index, usage);
    bool success = anim->SetTargetKey(key);
    RaisePendingError(L, anim, "SetTargetKey");
    return PushBool(L, success);
}

// ref: FUN_140856400
int Anim_SetTargetName(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: local success = self:SetTargetName(name)";
    CSimpleAnim* anim = AnimSelf(L, &index, usage);
    const char* name = RequiredString(L, &index, usage);
    bool success = anim->SetTargetName(name);
    RaisePendingError(L, anim, "SetTargetName");
    return PushBool(L, success);
}

// ref: FUN_1408571f0
int Anim_SetTargetParent(lua_State* L) {
    int32_t index = 1;
    CSimpleAnim* anim = AnimSelf(L, &index, "Usage: local success = self:SetTargetParent()");
    bool success = anim->SetTargetParent();
    RaisePendingError(L, anim, "SetTargetParent");
    return PushBool(L, success);
}

// ref: FUN_140857f70
int Anim_Stop(lua_State* L) {
    int32_t index = 1;
    CSimpleAnim* anim = AnimSelf(L, &index, "Usage: self:Stop()");
    anim->Stop(true);
    RaisePendingError(L, anim, "Stop");
    return 0;
}

// 0x142cf70c0
const luaL_Reg s_animMethods[] = {
    {"GetDuration", Anim_GetDuration},
    {"GetElapsed", Anim_GetElapsed},
    {"GetEndDelay", Anim_GetEndDelay},
    {"GetOrder", Anim_GetOrder},
    {"GetProgress", Anim_GetProgress},
    {"GetRegionParent", Anim_GetRegionParent},
    {"GetScript", Anim_GetScript},
    {"GetSmoothProgress", Anim_GetSmoothProgress},
    {"GetSmoothing", Anim_GetSmoothing},
    {"GetStartDelay", Anim_GetStartDelay},
    {"GetTarget", Anim_GetTarget},
    {"HasScript", Anim_HasScript},
    {"HookScript", Anim_HookScript},
    {"IsDelaying", Anim_IsDelaying},
    {"IsDone", Anim_IsDone},
    {"IsPaused", Anim_IsPaused},
    {"IsPlaying", Anim_IsPlaying},
    {"IsStopped", Anim_IsStopped},
    {"Pause", Anim_Pause},
    {"Play", Anim_Play},
    {"Restart", Anim_Restart},
    {"SetChildKey", Anim_SetChildKey},
    {"SetDuration", Anim_SetDuration},
    {"SetEndDelay", Anim_SetEndDelay},
    {"SetOrder", Anim_SetOrder},
    {"SetParent", Anim_SetParent},
    {"SetPlaying", Anim_SetPlaying},
    {"SetScript", Anim_SetScript},
    {"SetSmoothProgress", Anim_SetSmoothProgress},
    {"SetSmoothing", Anim_SetSmoothing},
    {"SetStartDelay", Anim_SetStartDelay},
    {"SetTarget", Anim_SetTarget},
    {"SetTargetKey", Anim_SetTargetKey},
    {"SetTargetName", Anim_SetTargetName},
    {"SetTargetParent", Anim_SetTargetParent},
    {"Stop", Anim_Stop},
};
static_assert(sizeof(s_animMethods) / sizeof(s_animMethods[0]) == 0x24, "0x24 Animation methods");

// ---- AnimationGroup ------------------------------------------------------------------------

struct CREATEANIMCONTEXT {
    CSimpleAnimGroup* m_group = nullptr;
    const char* m_type = nullptr;
    const char* m_name = nullptr;
    const char* m_inherits = nullptr;
    CSimpleAnim* m_anim = nullptr;
};

// ref: FUN_14080a250
int32_t CreateAnimationBody(lua_State* L, void* context) {
    auto ctx = static_cast<CREATEANIMCONTEXT*>(context);
    int32_t index = 1;
    const char* usage = "Usage: local anim = self:CreateAnimation([animationType, name, templateName])";
    ctx->m_group = GroupSelf(L, &index, usage);
    bool present = false;
    ctx->m_type = OptionalString(L, &index, usage, &present);
    if (!present) {
        ctx->m_type = nullptr;
    }
    ctx->m_name = OptionalString(L, &index, usage, &present);
    if (!present) {
        ctx->m_name = nullptr;
    }
    ctx->m_inherits = OptionalString(L, &index, usage, &present);
    if (!present) {
        ctx->m_inherits = nullptr;
    }
    ctx->m_anim = ctx->m_group->CreateAnimation(ctx->m_type, ctx->m_name, ctx->m_inherits);
    RaisePendingError(L, ctx->m_group, "CreateAnimation");
    return PushObject(L, ctx->m_anim);
}

// ref: FUN_1408227e0
int Group_CreateAnimation(lua_State* L) {
    CREATEANIMCONTEXT context;
    return ProtectedCall(L, CreateAnimationBody, &context);
}

// ref: FUN_140823510
int Group_Finish(lua_State* L) {
    int32_t index = 1;
    CSimpleAnimGroup* group = GroupSelf(L, &index, "Usage: self:Finish()");
    group->Finish();
    RaisePendingError(L, group, "Finish");
    return 0;
}

// ref: FUN_140824280
int Group_GetAnimationSpeedMultiplier(lua_State* L) {
    int32_t index = 1;
    CSimpleAnimGroup* group =
        GroupSelf(L, &index, "Usage: local animationSpeedMultiplier = self:GetAnimationSpeedMultiplier()");
    float value = group->m_speed;
    RaisePendingError(L, group, "GetAnimationSpeedMultiplier");
    return PushFloat(L, value);
}

struct GETANIMSCONTEXT {
    CSimpleAnimGroup* m_group = nullptr;
    ANIMLIST m_anims;
};

// ref: FUN_14080b0a0
int32_t GetAnimationsBody(lua_State* L, void* context) {
    auto ctx = static_cast<GETANIMSCONTEXT*>(context);
    int32_t index = 1;
    ctx->m_group = GroupSelf(L, &index, "Usage: local (scriptObject)* = self:GetAnimations()");
    ctx->m_group->GetScriptAnimations(&ctx->m_anims);
    RaisePendingError(L, ctx->m_group, "GetAnimations");
    int32_t results = 0;
    for (CSimpleAnim* anim : ctx->m_anims) {
        results += PushObject(L, anim);
    }
    return results;
}

// ref: FUN_140825000
int Group_GetAnimations(lua_State* L) {
    GETANIMSCONTEXT context;
    return ProtectedCall(L, GetAnimationsBody, &context);
}

// ref: FUN_140825d60
int Group_GetDuration(lua_State* L) {
    int32_t index = 1;
    CSimpleAnimGroup* group = GroupSelf(L, &index, "Usage: local durationSec = self:GetDuration()");
    float value = group->m_duration;
    RaisePendingError(L, group, "GetDuration");
    return PushFloat(L, value);
}

// ref: FUN_140826ae0
int Group_GetElapsed(lua_State* L) {
    int32_t index = 1;
    CSimpleAnimGroup* group = GroupSelf(L, &index, "Usage: local elapsedSec = self:GetElapsed()");
    float value = group->m_elapsed;
    RaisePendingError(L, group, "GetElapsed");
    return PushFloat(L, value);
}

// ref: FUN_140827860
int Group_GetLoopState(lua_State* L) {
    int32_t index = 1;
    CSimpleAnimGroup* group = GroupSelf(L, &index, "Usage: local loopState = self:GetLoopState()");
    const char* state = group->GetLoopState();
    RaisePendingError(L, group, "GetLoopState");
    return PushStringOrNil(L, state);
}

// ref: FUN_1408285e0
int Group_GetLooping(lua_State* L) {
    int32_t index = 1;
    CSimpleAnimGroup* group = GroupSelf(L, &index, "Usage: local loopType = self:GetLooping()");
    uint8_t looping = group->m_looping;
    RaisePendingError(L, group, "GetLooping");
    return PushStringOrNil(L, AnimLoopTypeToString(looping));
}

// ref: FUN_1408293c0
int Group_GetProgress(lua_State* L) {
    int32_t index = 1;
    CSimpleAnimGroup* group = GroupSelf(L, &index, "Usage: local progress = self:GetProgress()");
    float value = group->m_elapsed / group->m_duration;
    RaisePendingError(L, group, "GetProgress");
    return PushFloat(L, value);
}

// ref: FUN_14082a150
int Group_GetScript(lua_State* L) {
    return ScriptBody<CSimpleAnimGroup::GetObjectType, &FrameScript_Object::GetScript>(L);
}

// ref: FUN_14082ae00
int Group_HasScript(lua_State* L) {
    return HasScriptMethod<CSimpleAnimGroup>(L, CSimpleAnimGroup::GetObjectType());
}

// ref: FUN_14082bbf0
int Group_HookScript(lua_State* L) {
    return ScriptBody<CSimpleAnimGroup::GetObjectType, &FrameScript_Object::HookScript>(L);
}

// ref: FUN_14082c8a0
int Group_IsDone(lua_State* L) {
    int32_t index = 1;
    CSimpleAnimGroup* group = GroupSelf(L, &index, "Usage: local isDone = self:IsDone()");
    bool value = group->IsDone();
    RaisePendingError(L, group, "IsDone");
    return PushBool(L, value);
}

// ref: FUN_14082d620
int Group_IsPaused(lua_State* L) {
    int32_t index = 1;
    CSimpleAnimGroup* group = GroupSelf(L, &index, "Usage: local isPaused = self:IsPaused()");
    bool value = (group->m_groupFlags >> 2) & 1;
    RaisePendingError(L, group, "IsPaused");
    return PushBool(L, value);
}

// ref: FUN_14082e3a0
int Group_IsPendingFinish(lua_State* L) {
    int32_t index = 1;
    CSimpleAnimGroup* group = GroupSelf(L, &index, "Usage: local isPendingFinish = self:IsPendingFinish()");
    bool value = (group->m_groupFlags >> 1) & 1;
    RaisePendingError(L, group, "IsPendingFinish");
    return PushBool(L, value);
}

// ref: FUN_14082f120
int Group_IsPlaying(lua_State* L) {
    int32_t index = 1;
    CSimpleAnimGroup* group = GroupSelf(L, &index, "Usage: local isPlaying = self:IsPlaying()");
    bool value = group->m_playing && !(group->m_groupFlags & CSimpleAnimGroup::GROUP_PAUSED);
    RaisePendingError(L, group, "IsPlaying");
    return PushBool(L, value);
}

// ref: FUN_14082feb0
int Group_IsReverse(lua_State* L) {
    int32_t index = 1;
    CSimpleAnimGroup* group = GroupSelf(L, &index, "Usage: local isReverse = self:IsReverse()");
    bool value = group->m_playing ? group->m_reverse : false;
    RaisePendingError(L, group, "IsReverse");
    return PushBool(L, value);
}

// ref: FUN_140830c40
int Group_IsSetToFinalAlpha(lua_State* L) {
    int32_t index = 1;
    CSimpleAnimGroup* group = GroupSelf(L, &index, "Usage: local isSetToFinalAlpha = self:IsSetToFinalAlpha()");
    bool value = (group->m_groupFlags >> 4) & 1;
    RaisePendingError(L, group, "IsSetToFinalAlpha");
    return PushBool(L, value);
}

// ref: FUN_1408319c0
int Group_Pause(lua_State* L) {
    int32_t index = 1;
    CSimpleAnimGroup* group = GroupSelf(L, &index, "Usage: self:Pause()");
    group->Pause();
    RaisePendingError(L, group, "Pause");
    return 0;
}

// The optional reverse flag and time offset of Play and Restart.
void PlayArgs(lua_State* L, int32_t* index, const char* usage, bool* reverse, float* offset) {
    *reverse = false;
    if (lua_type(L, *index) >= 1) {
        *reverse = OptionalBool(L, index);
    } else {
        (*index)++;
    }
    *offset = 0.0f;
    if (lua_type(L, *index) >= 1) {
        *offset = RequiredFinite(L, index, usage);
    }
}

// ref: FUN_140832730
int Group_Play(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:Play([reverse, offset])";
    CSimpleAnimGroup* group = GroupSelf(L, &index, usage);
    bool reverse;
    float offset;
    PlayArgs(L, &index, usage, &reverse, &offset);
    group->Play(reverse, offset);
    RaisePendingError(L, group, "Play");
    return 0;
}

// ref: FUN_140833560
int Group_RemoveAnimations(lua_State* L) {
    int32_t index = 1;
    CSimpleAnimGroup* group = GroupSelf(L, &index, "Usage: self:RemoveAnimations()");
    group->RemoveAnimations();
    RaisePendingError(L, group, "RemoveAnimations");
    return 0;
}

// ref: FUN_1408342d0
int Group_Restart(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:Restart([reverse, offset])";
    CSimpleAnimGroup* group = GroupSelf(L, &index, usage);
    bool reverse;
    float offset;
    PlayArgs(L, &index, usage, &reverse, &offset);
    group->Restart(reverse, offset);
    RaisePendingError(L, group, "Restart");
    return 0;
}

// ref: FUN_140835100
int Group_SetAnimationSpeedMultiplier(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetAnimationSpeedMultiplier(animationSpeedMultiplier)";
    CSimpleAnimGroup* group = GroupSelf(L, &index, usage);
    float speed = RequiredFinite(L, &index, usage);
    group->m_speed = speed;
    RaisePendingError(L, group, "SetAnimationSpeedMultiplier");
    return 0;
}

// ref: FUN_140835ef0
int Group_SetLooping(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetLooping(loopType)";
    CSimpleAnimGroup* group = GroupSelf(L, &index, usage);
    uint8_t looping = RequiredName<uint8_t>(L, &index, usage, StringToAnimLoopType);
    group->m_looping = looping;
    RaisePendingError(L, group, "SetLooping");
    return 0;
}

// ref: FUN_140836d80
int Group_SetPlaying(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetPlaying(play)";
    CSimpleAnimGroup* group = GroupSelf(L, &index, usage);
    bool play = RequiredBool(L, &index, usage);
    group->SetPlaying(play);
    RaisePendingError(L, group, "SetPlaying");
    return 0;
}

// ref: FUN_140837b50
int Group_SetScript(lua_State* L) {
    return ScriptBody<CSimpleAnimGroup::GetObjectType, &FrameScript_Object::SetScript>(L);
}

// ref: FUN_140838800
int Group_SetToFinalAlpha(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetToFinalAlpha(setToFinalAlpha)";
    CSimpleAnimGroup* group = GroupSelf(L, &index, usage);
    bool value = RequiredBool(L, &index, usage);
    if (value) {
        group->m_groupFlags |= CSimpleAnimGroup::GROUP_SETTOFINALALPHA;
    } else {
        group->m_groupFlags &= ~CSimpleAnimGroup::GROUP_SETTOFINALALPHA;
    }
    RaisePendingError(L, group, "SetToFinalAlpha");
    return 0;
}

// ref: FUN_1408395e0
int Group_Stop(lua_State* L) {
    int32_t index = 1;
    CSimpleAnimGroup* group = GroupSelf(L, &index, "Usage: self:Stop()");
    group->Stop(true);
    RaisePendingError(L, group, "Stop");
    return 0;
}

// 0x142cf73e0
const luaL_Reg s_groupMethods[] = {
    {"CreateAnimation", Group_CreateAnimation},
    {"Finish", Group_Finish},
    {"GetAnimationSpeedMultiplier", Group_GetAnimationSpeedMultiplier},
    {"GetAnimations", Group_GetAnimations},
    {"GetDuration", Group_GetDuration},
    {"GetElapsed", Group_GetElapsed},
    {"GetLoopState", Group_GetLoopState},
    {"GetLooping", Group_GetLooping},
    {"GetProgress", Group_GetProgress},
    {"GetScript", Group_GetScript},
    {"HasScript", Group_HasScript},
    {"HookScript", Group_HookScript},
    {"IsDone", Group_IsDone},
    {"IsPaused", Group_IsPaused},
    {"IsPendingFinish", Group_IsPendingFinish},
    {"IsPlaying", Group_IsPlaying},
    {"IsReverse", Group_IsReverse},
    {"IsSetToFinalAlpha", Group_IsSetToFinalAlpha},
    {"Pause", Group_Pause},
    {"Play", Group_Play},
    {"RemoveAnimations", Group_RemoveAnimations},
    {"Restart", Group_Restart},
    {"SetAnimationSpeedMultiplier", Group_SetAnimationSpeedMultiplier},
    {"SetLooping", Group_SetLooping},
    {"SetPlaying", Group_SetPlaying},
    {"SetScript", Group_SetScript},
    {"SetToFinalAlpha", Group_SetToFinalAlpha},
    {"Stop", Group_Stop},
};
static_assert(sizeof(s_groupMethods) / sizeof(s_groupMethods[0]) == 0x1c, "0x1c AnimationGroup methods");

// ---- Alpha ---------------------------------------------------------------------------------

CSimpleAlphaAnim* AlphaSelf(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CSimpleAlphaAnim*>(Self(L, CSimpleAlphaAnim::GetObjectType(), index, usage));
}

// ref: FUN_14081f100
int Alpha_GetFromAlpha(lua_State* L) {
    int32_t index = 1;
    CSimpleAlphaAnim* anim = AlphaSelf(L, &index, "Usage: local normalizedAlpha = self:GetFromAlpha()");
    float value = anim->GetFromAlpha();
    RaisePendingError(L, anim, "GetFromAlpha");
    return PushFloat(L, value);
}

// ref: FUN_14081fe80
int Alpha_GetToAlpha(lua_State* L) {
    int32_t index = 1;
    CSimpleAlphaAnim* anim = AlphaSelf(L, &index, "Usage: local normalizedAlpha = self:GetToAlpha()");
    float value = anim->GetToAlpha();
    RaisePendingError(L, anim, "GetToAlpha");
    return PushFloat(L, value);
}

// ref: FUN_140820c00
int Alpha_SetFromAlpha(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetFromAlpha(normalizedAlpha)";
    CSimpleAlphaAnim* anim = AlphaSelf(L, &index, usage);
    float value = RequiredFinite(L, &index, usage);
    anim->SetFromAlpha(value);
    RaisePendingError(L, anim, "SetFromAlpha");
    return 0;
}

// ref: FUN_1408219f0
int Alpha_SetToAlpha(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetToAlpha(normalizedAlpha)";
    CSimpleAlphaAnim* anim = AlphaSelf(L, &index, usage);
    float value = RequiredFinite(L, &index, usage);
    anim->SetToAlpha(value);
    RaisePendingError(L, anim, "SetToAlpha");
    return 0;
}

// 0x142cf7300
const luaL_Reg s_alphaMethods[] = {
    {"GetFromAlpha", Alpha_GetFromAlpha},
    {"GetToAlpha", Alpha_GetToAlpha},
    {"SetFromAlpha", Alpha_SetFromAlpha},
    {"SetToAlpha", Alpha_SetToAlpha},
};

// ---- FlipBook ------------------------------------------------------------------------------

CSimpleFlipBookAnim* FlipBookSelf(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CSimpleFlipBookAnim*>(Self(L, CSimpleFlipBookAnim::GetObjectType(), index, usage));
}

template <uint32_t CSimpleFlipBookAnim::*Field>
int FlipBookGet(lua_State* L, const char* usage, const char* method) {
    int32_t index = 1;
    CSimpleFlipBookAnim* anim = FlipBookSelf(L, &index, usage);
    uint32_t value = anim->*Field;
    RaisePendingError(L, anim, method);
    return PushUInt(L, value);
}

template <uint32_t CSimpleFlipBookAnim::*Field>
int FlipBookSet(lua_State* L, const char* usage, const char* method) {
    int32_t index = 1;
    CSimpleFlipBookAnim* anim = FlipBookSelf(L, &index, usage);
    uint32_t value = RequiredUInt32(L, &index, usage);
    anim->*Field = value;
    RaisePendingError(L, anim, method);
    return 0;
}

// ref: FUN_14088a420
int FlipBook_GetFlipBookColumns(lua_State* L) {
    return FlipBookGet<&CSimpleFlipBookAnim::m_columns>(L, "Usage: local columns = self:GetFlipBookColumns()",
        "GetFlipBookColumns");
}

// ref: FUN_14088b1a0
int FlipBook_GetFlipBookFrameHeight(lua_State* L) {
    return FlipBookGet<&CSimpleFlipBookAnim::m_frameHeight>(L, "Usage: local height = self:GetFlipBookFrameHeight()",
        "GetFlipBookFrameHeight");
}

// ref: FUN_14088bf20
int FlipBook_GetFlipBookFrameWidth(lua_State* L) {
    return FlipBookGet<&CSimpleFlipBookAnim::m_frameWidth>(L, "Usage: local width = self:GetFlipBookFrameWidth()",
        "GetFlipBookFrameWidth");
}

// ref: FUN_14088cca0
int FlipBook_GetFlipBookFrames(lua_State* L) {
    return FlipBookGet<&CSimpleFlipBookAnim::m_frames>(L, "Usage: local frames = self:GetFlipBookFrames()",
        "GetFlipBookFrames");
}

// ref: FUN_14088da20
int FlipBook_GetFlipBookRows(lua_State* L) {
    return FlipBookGet<&CSimpleFlipBookAnim::m_rows>(L, "Usage: local rows = self:GetFlipBookRows()", "GetFlipBookRows");
}

// ref: FUN_14088e7a0
int FlipBook_SetFlipBookColumns(lua_State* L) {
    return FlipBookSet<&CSimpleFlipBookAnim::m_columns>(L, "Usage: self:SetFlipBookColumns(columns)",
        "SetFlipBookColumns");
}

// ref: FUN_14088f630
int FlipBook_SetFlipBookFrameHeight(lua_State* L) {
    return FlipBookSet<&CSimpleFlipBookAnim::m_frameHeight>(L, "Usage: self:SetFlipBookFrameHeight(height)",
        "SetFlipBookFrameHeight");
}

// ref: FUN_1408904c0
int FlipBook_SetFlipBookFrameWidth(lua_State* L) {
    return FlipBookSet<&CSimpleFlipBookAnim::m_frameWidth>(L, "Usage: self:SetFlipBookFrameWidth(width)",
        "SetFlipBookFrameWidth");
}

// ref: FUN_140891350
int FlipBook_SetFlipBookFrames(lua_State* L) {
    return FlipBookSet<&CSimpleFlipBookAnim::m_frames>(L, "Usage: self:SetFlipBookFrames(frames)", "SetFlipBookFrames");
}

// ref: FUN_1408921e0
int FlipBook_SetFlipBookRows(lua_State* L) {
    return FlipBookSet<&CSimpleFlipBookAnim::m_rows>(L, "Usage: self:SetFlipBookRows(rows)", "SetFlipBookRows");
}

// 0x142cf7340
const luaL_Reg s_flipBookMethods[] = {
    {"GetFlipBookColumns", FlipBook_GetFlipBookColumns},
    {"GetFlipBookFrameHeight", FlipBook_GetFlipBookFrameHeight},
    {"GetFlipBookFrameWidth", FlipBook_GetFlipBookFrameWidth},
    {"GetFlipBookFrames", FlipBook_GetFlipBookFrames},
    {"GetFlipBookRows", FlipBook_GetFlipBookRows},
    {"SetFlipBookColumns", FlipBook_SetFlipBookColumns},
    {"SetFlipBookFrameHeight", FlipBook_SetFlipBookFrameHeight},
    {"SetFlipBookFrameWidth", FlipBook_SetFlipBookFrameWidth},
    {"SetFlipBookFrames", FlipBook_SetFlipBookFrames},
    {"SetFlipBookRows", FlipBook_SetFlipBookRows},
};

// ---- Scale and Rotation origins ------------------------------------------------------------

template <class T>
int GetOriginMethod(lua_State* L, int32_t type) {
    int32_t index = 1;
    auto anim = static_cast<T*>(Self(L, type, &index, "Usage: local point, x, y = self:GetOrigin()"));
    uint32_t point = anim->m_originPoint;
    float origin[2] = {anim->m_origin[0], anim->m_origin[1]};
    RaisePendingError(L, anim, "GetOrigin");
    int32_t results = PushFramePoint(L, point);
    return results + PushPair(L, origin);
}

template <class T>
int SetOriginMethod(lua_State* L, int32_t type) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetOrigin(point, origin)";
    auto anim = static_cast<T*>(Self(L, type, &index, usage));
    uint32_t point = RequiredFramePoint(L, &index, usage);
    float origin[2] = {0.0f, 0.0f};
    RequiredPair(L, &index, usage, origin);
    anim->m_origin[0] = origin[0];
    anim->m_origin[1] = origin[1];
    anim->m_originPoint = point;
    RaisePendingError(L, anim, "SetOrigin");
    return 0;
}

// ---- Scale ---------------------------------------------------------------------------------

CSimpleScaleAnim* ScaleSelf(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CSimpleScaleAnim*>(Self(L, CSimpleScaleAnim::GetObjectType(), index, usage));
}

// ref: FUN_14089c910
int Scale_GetOrigin(lua_State* L) {
    return GetOriginMethod<CSimpleScaleAnim>(L, CSimpleScaleAnim::GetObjectType());
}

template <void (CSimpleScaleAnim::*Get)(float*)>
int ScaleGet(lua_State* L, const char* usage, const char* method) {
    int32_t index = 1;
    CSimpleScaleAnim* anim = ScaleSelf(L, &index, usage);
    float scale[2];
    (anim->*Get)(scale);
    RaisePendingError(L, anim, method);
    return PushPair(L, scale);
}

template <void (CSimpleScaleAnim::*Set)(const float*)>
int ScaleSet(lua_State* L, const char* usage, const char* method) {
    int32_t index = 1;
    CSimpleScaleAnim* anim = ScaleSelf(L, &index, usage);
    float scale[2] = {0.0f, 0.0f};
    RequiredPair(L, &index, usage, scale);
    (anim->*Set)(scale);
    RaisePendingError(L, anim, method);
    return 0;
}

// ref: FUN_14089d710
int Scale_GetScale(lua_State* L) {
    return ScaleGet<&CSimpleScaleAnim::GetScale>(L, "Usage: local x, y = self:GetScale()", "GetScale");
}

// ref: FUN_14089e490
int Scale_GetScaleFrom(lua_State* L) {
    return ScaleGet<&CSimpleScaleAnim::GetScaleFrom>(L, "Usage: local x, y = self:GetScaleFrom()", "GetScaleFrom");
}

// ref: FUN_14089f210
int Scale_GetScaleTo(lua_State* L) {
    return ScaleGet<&CSimpleScaleAnim::GetScaleTo>(L, "Usage: local x, y = self:GetScaleTo()", "GetScaleTo");
}

// ref: FUN_14089ff90
int Scale_SetOrigin(lua_State* L) {
    return SetOriginMethod<CSimpleScaleAnim>(L, CSimpleScaleAnim::GetObjectType());
}

// ref: FUN_1408a0eb0
int Scale_SetScale(lua_State* L) {
    return ScaleSet<&CSimpleScaleAnim::SetScale>(L, "Usage: self:SetScale(scale)", "SetScale");
}

// ref: FUN_1408a1d90
int Scale_SetScaleFrom(lua_State* L) {
    return ScaleSet<&CSimpleScaleAnim::SetScaleFrom>(L, "Usage: self:SetScaleFrom(scale)", "SetScaleFrom");
}

// ref: FUN_1408a2c70
int Scale_SetScaleTo(lua_State* L) {
    return ScaleSet<&CSimpleScaleAnim::SetScaleTo>(L, "Usage: self:SetScaleTo(scale)", "SetScaleTo");
}

// 0x142cf7650 (Scale and LineScale)
const luaL_Reg s_scaleMethods[] = {
    {"GetOrigin", Scale_GetOrigin},
    {"GetScale", Scale_GetScale},
    {"GetScaleFrom", Scale_GetScaleFrom},
    {"GetScaleTo", Scale_GetScaleTo},
    {"SetOrigin", Scale_SetOrigin},
    {"SetScale", Scale_SetScale},
    {"SetScaleFrom", Scale_SetScaleFrom},
    {"SetScaleTo", Scale_SetScaleTo},
};

// ---- Translation ---------------------------------------------------------------------------

// ref: FUN_1408a57e0
int Translation_GetOffset(lua_State* L) {
    int32_t index = 1;
    auto anim = static_cast<CSimpleTranslationAnim*>(
        Self(L, CSimpleTranslationAnim::GetObjectType(), &index, "Usage: local offsetX, offsetY = self:GetOffset()"));
    float x = anim->m_offset[0];
    float y = anim->m_offset[1];
    RaisePendingError(L, anim, "GetOffset");
    return PushUIUnits(L, x) + PushUIUnits(L, y);
}

// ref: FUN_1408a6620
int Translation_SetOffset(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetOffset(offsetX, offsetY)";
    auto anim = static_cast<CSimpleTranslationAnim*>(Self(L, CSimpleTranslationAnim::GetObjectType(), &index, usage));
    float offset[2];
    offset[0] = RequiredUIUnits(L, &index, usage);
    offset[1] = RequiredUIUnits(L, &index, usage);
    anim->SetOffset(offset);
    RaisePendingError(L, anim, "SetOffset");
    return 0;
}

// 0x142cf76f0 (Translation and LineTranslation)
const luaL_Reg s_translationMethods[] = {
    {"GetOffset", Translation_GetOffset},
    {"SetOffset", Translation_SetOffset},
};

// ---- TextureCoordTranslation ---------------------------------------------------------------

// ref: FUN_1408a3b50
int TexCoord_GetOffset(lua_State* L) {
    int32_t index = 1;
    auto anim = static_cast<CSimpleTexCoordTranslationAnim*>(Self(L, CSimpleTexCoordTranslationAnim::GetObjectType(),
        &index, "Usage: local offsetU, offsetV = self:GetOffset()"));
    float offset[2] = {anim->m_offset[0], anim->m_offset[1]};
    RaisePendingError(L, anim, "GetOffset");
    return PushFloat(L, offset[0]) + PushFloat(L, offset[1]);
}

// ref: FUN_1408a4960
int TexCoord_SetOffset(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetOffset(offsetU, offsetV)";
    auto anim = static_cast<CSimpleTexCoordTranslationAnim*>(
        Self(L, CSimpleTexCoordTranslationAnim::GetObjectType(), &index, usage));
    float offset[2];
    offset[0] = RequiredFinite(L, &index, usage);
    offset[1] = RequiredFinite(L, &index, usage);
    // FUN_14060ba40 on the texture coordinate offset
    if (2.384185791015625e-07f < offset[0] * offset[0] + offset[1] * offset[1]) {
        anim->m_offset[0] = offset[0];
        anim->m_offset[1] = offset[1];
    } else {
        anim->m_offset[0] = 0.0f;
        anim->m_offset[1] = 0.0f;
    }
    RaisePendingError(L, anim, "SetOffset");
    return 0;
}

// 0x142cf76d0
const luaL_Reg s_texCoordMethods[] = {
    {"GetOffset", TexCoord_GetOffset},
    {"SetOffset", TexCoord_SetOffset},
};

// ---- Rotation ------------------------------------------------------------------------------

CSimpleRotationAnim* RotationSelf(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CSimpleRotationAnim*>(Self(L, CSimpleRotationAnim::GetObjectType(), index, usage));
}

// ref: FUN_1408974f0
int Rotation_GetDegrees(lua_State* L) {
    int32_t index = 1;
    CSimpleRotationAnim* anim = RotationSelf(L, &index, "Usage: local angle = self:GetDegrees()");
    float value = anim->m_radians * 57.295780181884766f;
    RaisePendingError(L, anim, "GetDegrees");
    return PushFloat(L, value);
}

// ref: FUN_140898280
int Rotation_GetOrigin(lua_State* L) {
    return GetOriginMethod<CSimpleRotationAnim>(L, CSimpleRotationAnim::GetObjectType());
}

// ref: FUN_140899080
int Rotation_GetRadians(lua_State* L) {
    int32_t index = 1;
    CSimpleRotationAnim* anim = RotationSelf(L, &index, "Usage: local angle = self:GetRadians()");
    float value = anim->m_radians;
    RaisePendingError(L, anim, "GetRadians");
    return PushFloat(L, value);
}

// ref: FUN_140899e00
int Rotation_SetDegrees(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetDegrees(angle)";
    CSimpleRotationAnim* anim = RotationSelf(L, &index, usage);
    float degrees = RequiredFinite(L, &index, usage);
    anim->m_radians = degrees * 0.01745329238474369f;
    RaisePendingError(L, anim, "SetDegrees");
    return 0;
}

// ref: FUN_14089ac00
int Rotation_SetOrigin(lua_State* L) {
    return SetOriginMethod<CSimpleRotationAnim>(L, CSimpleRotationAnim::GetObjectType());
}

// ref: FUN_14089bb20
int Rotation_SetRadians(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetRadians(angle)";
    CSimpleRotationAnim* anim = RotationSelf(L, &index, usage);
    anim->m_radians = RequiredFinite(L, &index, usage);
    RaisePendingError(L, anim, "SetRadians");
    return 0;
}

// 0x142cf75f0
const luaL_Reg s_rotationMethods[] = {
    {"GetDegrees", Rotation_GetDegrees},
    {"GetOrigin", Rotation_GetOrigin},
    {"GetRadians", Rotation_GetRadians},
    {"SetDegrees", Rotation_SetDegrees},
    {"SetOrigin", Rotation_SetOrigin},
    {"SetRadians", Rotation_SetRadians},
};

// ---- Path ----------------------------------------------------------------------------------

CSimpleAnimPath* PathSelf(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CSimpleAnimPath*>(Self(L, CSimpleAnimPath::GetObjectType(), index, usage));
}

struct CREATEPOINTCONTEXT {
    CSimpleAnimPath* m_path = nullptr;
    const char* m_name = nullptr;
    const char* m_inherits = nullptr;
    int32_t m_order = 0;
    bool m_hasOrder = false;
    CSimpleControlPoint* m_point = nullptr;
};

// ref: FUN_140811960
int32_t CreateControlPointBody(lua_State* L, void* context) {
    auto ctx = static_cast<CREATEPOINTCONTEXT*>(context);
    int32_t index = 1;
    const char* usage = "Usage: local point = self:CreateControlPoint([name, templateName, order])";
    ctx->m_path = PathSelf(L, &index, usage);
    bool present = false;
    ctx->m_name = OptionalString(L, &index, usage, &present);
    if (!present) {
        ctx->m_name = nullptr;
    }
    ctx->m_inherits = OptionalString(L, &index, usage, &present);
    if (!present) {
        ctx->m_inherits = nullptr;
    }
    if (lua_type(L, index) >= 1) {
        ctx->m_order = RequiredInt32(L, &index, usage);
        ctx->m_hasOrder = true;
    }
    ctx->m_point = ctx->m_path->CreateControlPoint(ctx->m_name, ctx->m_inherits, ctx->m_hasOrder ? &ctx->m_order : nullptr);
    RaisePendingError(L, ctx->m_path, "CreateControlPoint");
    return PushObject(L, ctx->m_point);
}

// ref: FUN_140893070
int Path_CreateControlPoint(lua_State* L) {
    CREATEPOINTCONTEXT context;
    return ProtectedCall(L, CreateControlPointBody, &context);
}

struct GETPOINTSCONTEXT {
    CSimpleAnimPath* m_path = nullptr;
    CONTROLPOINTLIST m_points;
};

// ref: FUN_1408127b0
int32_t GetControlPointsBody(lua_State* L, void* context) {
    auto ctx = static_cast<GETPOINTSCONTEXT*>(context);
    int32_t index = 1;
    ctx->m_path = PathSelf(L, &index, "Usage: local (scriptObject)* = self:GetControlPoints()");
    ctx->m_path->GetScriptControlPoints(&ctx->m_points);
    RaisePendingError(L, ctx->m_path, "GetControlPoints");
    int32_t results = 0;
    for (CSimpleControlPoint* point : ctx->m_points) {
        results += PushObject(L, point);
    }
    return results;
}

// ref: FUN_140893da0
int Path_GetControlPoints(lua_State* L) {
    GETPOINTSCONTEXT context;
    return ProtectedCall(L, GetControlPointsBody, &context);
}

// ref: FUN_140894b00
int Path_GetCurveType(lua_State* L) {
    int32_t index = 1;
    CSimpleAnimPath* path = PathSelf(L, &index, "Usage: local curveType = self:GetCurveType()");
    uint8_t curve = path->m_curve;
    RaisePendingError(L, path, "GetCurveType");
    return PushStringOrNil(L, AnimCurveTypeToString(curve));
}

// ref: FUN_1408958e0
int Path_GetMaxControlPointOrder(lua_State* L) {
    int32_t index = 1;
    CSimpleAnimPath* path = PathSelf(L, &index, "Usage: local maxOrder = self:GetMaxControlPointOrder()");
    int32_t order = path->GetMaxControlPointOrder();
    RaisePendingError(L, path, "GetMaxControlPointOrder");
    return PushInt(L, order);
}

// ref: FUN_140896660
int Path_SetCurveType(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetCurveType(curveType)";
    CSimpleAnimPath* path = PathSelf(L, &index, usage);
    uint8_t curve = RequiredName<uint8_t>(L, &index, usage, StringToAnimCurveType);
    path->m_curve = curve;
    RaisePendingError(L, path, "SetCurveType");
    return 0;
}

// 0x142cf75a0
const luaL_Reg s_pathMethods[] = {
    {"CreateControlPoint", Path_CreateControlPoint},
    {"GetControlPoints", Path_GetControlPoints},
    {"GetCurveType", Path_GetCurveType},
    {"GetMaxControlPointOrder", Path_GetMaxControlPointOrder},
    {"SetCurveType", Path_SetCurveType},
};

// ---- ControlPoint --------------------------------------------------------------------------

CSimpleControlPoint* PointSelf(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CSimpleControlPoint*>(Self(L, CSimpleControlPoint::GetObjectType(), index, usage));
}

// ref: FUN_1409006f0
int Point_GetOffset(lua_State* L) {
    int32_t index = 1;
    CSimpleControlPoint* point = PointSelf(L, &index, "Usage: local offsetX, offsetY = self:GetOffset()");
    float x = point->m_offset[0];
    float y = point->m_offset[1];
    RaisePendingError(L, point, "GetOffset");
    return PushUIUnits(L, x) + PushUIUnits(L, y);
}

// ref: FUN_140901530
int Point_GetOrder(lua_State* L) {
    int32_t index = 1;
    CSimpleControlPoint* point = PointSelf(L, &index, "Usage: local order = self:GetOrder()");
    int32_t order = point->m_order;
    RaisePendingError(L, point, "GetOrder");
    return PushInt(L, order);
}

// ref: FUN_1409022b0
int Point_SetOffset(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetOffset(offsetX, offsetY)";
    CSimpleControlPoint* point = PointSelf(L, &index, usage);
    float offset[2];
    offset[0] = RequiredUIUnits(L, &index, usage);
    offset[1] = RequiredUIUnits(L, &index, usage);
    point->SetOffset(offset);
    RaisePendingError(L, point, "SetOffset");
    return 0;
}

// ref: FUN_1409031b0
int Point_SetOrder(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetOrder(order)";
    CSimpleControlPoint* point = PointSelf(L, &index, usage);
    int32_t order = RequiredInt32(L, &index, usage);
    point->SetOrder(order, true, false);
    RaisePendingError(L, point, "SetOrder");
    return 0;
}

// The parent path, by name or as an object.
// ref: FUN_1424c5130
CSimpleAnimPath* PointSetParentArgs(lua_State* L, CSimpleControlPoint** point) {
    const char* error = nullptr;
    *point = static_cast<CSimpleControlPoint*>(
        FrameScript_Object::GetObjectFromLua(&error, L, CSimpleControlPoint::GetObjectType(), 1, false, false));
    if (*point == nullptr) {
        FrameScript_ActionError("Invalid object for SetParent() call. (%s)", error != nullptr ? error : "");
        return nullptr;
    }
    CSimpleAnimPath* path = nullptr;
    int type = lua_type(L, 2);
    if (type == LUA_TNONE || type == LUA_TNIL) {
        FrameScript_ObjectError(*point, "SetParent", "Cannot set a 'nil' parent");
        return nullptr;
    }
    if (type == LUA_TSTRING) {
        const char* name = lua_tostring(L, 2);
        bool allowLockdown = ((*point)->m_flags & FSOBJ_LOCKDOWN) || !((*point)->m_flags & FSOBJ_ACCESSIBLE);
        lua_State* state = FrameScript_GetContext();
        lua_pushstring(state, name);
        lua_rawget(state, LUA_GLOBALSINDEX);
        path = static_cast<CSimpleAnimPath*>(FrameScript_Object::GetObjectFromLua(
            nullptr, state, CSimpleAnimPath::GetObjectType(), -1, false, allowLockdown));
        lua_settop(state, -2);
        if (path == nullptr) {
            FrameScript_ObjectError(*point, "SetParent", "Couldn't find object named '%s'", name != nullptr ? name : "");
        }
    } else if (type == LUA_TTABLE) {
        const char* pathError = nullptr;
        path = static_cast<CSimpleAnimPath*>(
            FrameScript_Object::GetObjectFromLua(&pathError, L, CSimpleAnimPath::GetObjectType(), 2, false, false));
        if (pathError != nullptr) {
            FrameScript_ActionError(pathError);
        }
    }
    return path;
}

// ref: FUN_140903f70
int Point_SetParent(lua_State* L) {
    CSimpleControlPoint* point = nullptr;
    CSimpleAnimPath* path = PointSetParentArgs(L, &point);
    if (path != nullptr) {
        int8_t previous = point->m_order;
        int32_t order;
        bool isDefault;
        if (!lua_isnumber(L, 3)) {
            order = point->m_order;
            if (order == -1) {
                order = path->GetMaxControlPointOrder();
            }
            isDefault = true;
        } else {
            order = static_cast<int32_t>(lua_tointeger(L, 3));
            isDefault = false;
        }
        point->SetOrder(order, false, isDefault);
        if (!point->SetParentPath(path) && previous != point->m_order) {
            path->OnControlPointOrderChanged(point);
        }
    }
    RaisePendingError(L, nullptr, nullptr);
    return 0;
}

// 0x142cf7cd0
const luaL_Reg s_pointMethods[] = {
    {"GetOffset", Point_GetOffset},
    {"GetOrder", Point_GetOrder},
    {"SetOffset", Point_SetOffset},
    {"SetOrder", Point_SetOrder},
    {"SetParent", Point_SetParent},
};

// ---- the method tables of each class -------------------------------------------------------

// ref: FUN_1408a83d0
void Anim_Fill(lua_State* L) {
    CScriptObject_FillScriptMethods(L);
    FrameScript_Object::FillScriptMethodTable(L, s_animMethods, 0x24);
}

// ref: FUN_1408a8390
void Alpha_Fill(lua_State* L) {
    Anim_Fill(L);
    FrameScript_Object::FillScriptMethodTable(L, s_alphaMethods, 4);
}

// ref: FUN_1408a8490
void FlipBook_Fill(lua_State* L) {
    Anim_Fill(L);
    FrameScript_Object::FillScriptMethodTable(L, s_flipBookMethods, 10);
}

// ref: FUN_1408a8550
void Path_Fill(lua_State* L) {
    Anim_Fill(L);
    FrameScript_Object::FillScriptMethodTable(L, s_pathMethods, 5);
}

// ref: FUN_1408a8590
void Rotation_Fill(lua_State* L) {
    Anim_Fill(L);
    FrameScript_Object::FillScriptMethodTable(L, s_rotationMethods, 6);
}

// ref: FUN_1408a84d0
void Scale_Fill(lua_State* L) {
    Anim_Fill(L);
    FrameScript_Object::FillScriptMethodTable(L, s_scaleMethods, 8);
}

// ref: FUN_1408a85d0
void TexCoord_Fill(lua_State* L) {
    Anim_Fill(L);
    FrameScript_Object::FillScriptMethodTable(L, s_texCoordMethods, 2);
}

// ref: FUN_1408a8510
void Translation_Fill(lua_State* L) {
    Anim_Fill(L);
    FrameScript_Object::FillScriptMethodTable(L, s_translationMethods, 2);
}

// ref: FUN_140a50f20
void Point_Fill(lua_State* L) {
    CScriptObject_FillScriptMethods(L);
    FrameScript_Object::FillScriptMethodTable(L, s_pointMethods, 5);
}

template <class T>
void CreateMetaTables(void (*fill)(lua_State*)) {
    lua_State* L = FrameScript_GetContext();
    T::s_metaTable = FrameScript_Object::CreateScriptMetaTable(L, fill, false);
    T::s_restrictedMetaTable = FrameScript_Object::CreateScriptMetaTable(L, fill, true);
}

}

// ref: FUN_1408a8400
void CSimpleAnimGroup_FillScriptMethods(lua_State* L) {
    CScriptObject_FillScriptMethods(L);
    FrameScript_Object::FillScriptMethodTable(L, s_groupMethods, 0x1c);
}

// ref: FUN_140606530
void CSimpleAnim::CreateScriptMetaTables() {
    CreateMetaTables<CSimpleAnim>(Anim_Fill);
}

// ref: FUN_1406064e0
void CSimpleAlphaAnim::CreateScriptMetaTables() {
    CreateMetaTables<CSimpleAlphaAnim>(Alpha_Fill);
}

// ref: FUN_140606620
void CSimpleFlipBookAnim::CreateScriptMetaTables() {
    CreateMetaTables<CSimpleFlipBookAnim>(FlipBook_Fill);
}

// ref: FUN_140606710
void CSimpleAnimPath::CreateScriptMetaTables() {
    CreateMetaTables<CSimpleAnimPath>(Path_Fill);
}

// ref: FUN_140606760
void CSimpleRotationAnim::CreateScriptMetaTables() {
    CreateMetaTables<CSimpleRotationAnim>(Rotation_Fill);
}

// ref: FUN_1406067b0
void CSimpleScaleAnim::CreateScriptMetaTables() {
    CreateMetaTables<CSimpleScaleAnim>(Scale_Fill);
}

// ref: FUN_140606670
void CSimpleLineScaleAnim::CreateScriptMetaTables() {
    CreateMetaTables<CSimpleLineScaleAnim>(Scale_Fill);
}

// ref: FUN_140606800
void CSimpleTexCoordTranslationAnim::CreateScriptMetaTables() {
    CreateMetaTables<CSimpleTexCoordTranslationAnim>(TexCoord_Fill);
}

// ref: FUN_140606850
void CSimpleTranslationAnim::CreateScriptMetaTables() {
    CreateMetaTables<CSimpleTranslationAnim>(Translation_Fill);
}

// ref: FUN_1406066c0
void CSimpleLineTranslationAnim::CreateScriptMetaTables() {
    CreateMetaTables<CSimpleLineTranslationAnim>(Translation_Fill);
}

// ref: FUN_1406065d0
void CSimpleControlPoint::CreateScriptMetaTables() {
    CreateMetaTables<CSimpleControlPoint>(Point_Fill);
}
