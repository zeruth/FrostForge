// CSimpleAnim (Engine/Source/Frame/CSimpleAnim.cpp): the animations of a group, the path
// control points, and the smoothing they ease through.

#include "ui/CSimpleAnim.hpp"

#include <cmath>
#include <cstring>
#include <new>

#include "base/Coordinate.hpp"
#include "base/Status.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "ui/BinaryXML.hpp"
#include "ui/CAnimatableObject.hpp"
#include "ui/CScriptRegion.hpp"
#include "ui/FrameXML.hpp"

extern "C" {
#include "lua.h"
}

int32_t CSimpleAnim::s_metaTable = -2;
int32_t CSimpleAnim::s_restrictedMetaTable = -2;
int32_t CSimpleAlphaAnim::s_metaTable = -2;
int32_t CSimpleAlphaAnim::s_restrictedMetaTable = -2;
int32_t CSimpleTranslationAnim::s_metaTable = -2;
int32_t CSimpleTranslationAnim::s_restrictedMetaTable = -2;
int32_t CSimpleLineTranslationAnim::s_metaTable = -2;
int32_t CSimpleLineTranslationAnim::s_restrictedMetaTable = -2;
int32_t CSimpleTexCoordTranslationAnim::s_metaTable = -2;
int32_t CSimpleTexCoordTranslationAnim::s_restrictedMetaTable = -2;
int32_t CSimpleRotationAnim::s_metaTable = -2;
int32_t CSimpleRotationAnim::s_restrictedMetaTable = -2;
int32_t CSimpleScaleAnim::s_metaTable = -2;
int32_t CSimpleScaleAnim::s_restrictedMetaTable = -2;
int32_t CSimpleLineScaleAnim::s_metaTable = -2;
int32_t CSimpleLineScaleAnim::s_restrictedMetaTable = -2;
int32_t CSimpleFlipBookAnim::s_metaTable = -2;
int32_t CSimpleFlipBookAnim::s_restrictedMetaTable = -2;
int32_t CSimpleControlPoint::s_metaTable = -2;
int32_t CSimpleControlPoint::s_restrictedMetaTable = -2;
int32_t CSimpleAnimPath::s_metaTable = -2;
int32_t CSimpleAnimPath::s_restrictedMetaTable = -2;

namespace {

const char s_source[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Frame\\CSimpleAnim.cpp";
const char s_fixedArrayTag[] = ".?AV?$TSFixedArray@PEAVCSimpleControlPoint@@$0BP@@@";
const char s_growableArrayTag[] = ".?AV?$TSGrowableArray@PEAVCSimpleControlPoint@@$0BP@@@";
const char s_pointListTag[] =
    ".?AV?$TSList@VCSimpleControlPoint@@V?$TSGetExplicitLink@VCSimpleControlPoint@@@@$07@@";

// Below this an offset is no offset, and a length of time no time.
const float s_epsilon = 2.384185791015625e-07f;

void* AllocAnim(uint64_t size, int32_t line) {
    return SMemAlloc(size, 0x1f, s_source, SStrHashFnv1a(s_source), line, 0);
}

// The metatable of a script object: the plain one, or the restricted one for a forbidden
// object or one whose metatable was switched.
int32_t MetaTable(FrameScript_Object* object, int32_t normal, int32_t restricted) {
    if (!(object->m_flags & FSOBJ_FORBIDDEN) && !(object->m_flags & FSOBJ_PROTECTED_METATABLE)) {
        return normal;
    }
    return restricted;
}

// The name an animation goes by in load errors.
const char* LoadName(CScriptObject* object) {
    const char* name = object->GetName();
    return name != nullptr ? name : object->GetObjectTypeName();
}

// A UI-unit offset from XML as layout units.
float XMLToLayout(float value) {
    return value / (g_coordinateSpace.m_aspectCompensation * 1024.0f) * g_coordinateSpace.m_diagonalWidth;
}

// Copies a name of at most 63 characters (the target name field).
void CopyName63(char* dest, const char* source) {
    uint32_t i = 0;
    for (; i < 0x3f && source[i] != '\0'; i++) {
        dest[i] = source[i];
    }
    dest[i] = '\0';
}

// Copies a template name as the loaders do (at most 127 characters).
void CopyInheritsName(char* name, const char* source) {
    uint32_t i = 0;
    for (; i < 0x7f && source[i] != '\0'; i++) {
        name[i] = source[i];
    }
    name[i] = '\0';
}

// The template an element inherits, through `load` (the virtual LoadXML of the object), with
// the loaders' errors.
template <class Load>
void LoadInherited(XMLOBJECT* node, CStatus* status, Load load) {
    if (!(node->m_attributes & 0x100)) {
        return;
    }
    char name[0x80] = {};
    CopyInheritsName(name, node->m_inherits.m_text);
    const char* taint = nullptr;
    bool busy = true;
    XMLOBJECT* inherited = FrameXML_AcquireVirtual(name, &taint, &busy);
    if (inherited == nullptr) {
        status->Add(1, "Couldn't find inherited node: %s", name);
    } else if (busy) {
        status->Add(1, "Recursively inherited node: %s", name);
    } else {
        load(inherited);
    }
    if (inherited != nullptr && !busy) {
        FrameXML_ReleaseVirtual(name);
    }
}

// The <Origin> children of a rotation or scale: a frame point (CENTER by default) and an
// offset, read only when the point is given.
// ref: FUN_140667b00
bool XMLOriginGet(XMLORIGIN* origin, uint32_t* point, float* offset, CStatus* status) {
    *point = 4;
    offset[0] = 0.0f;
    offset[1] = 0.0f;
    if (origin->m_originFlags & 1) {
        *point = origin->m_point;
        XMLDimensionGet(origin, &offset[0], &offset[1], status);
    }
    return true;
}

void LoadOrigins(CSimpleAnim* anim, XMLOBJECT* node, CStatus* status, uint32_t* point, float* offset) {
    for (XMLOBJECT* child = node->m_child; child != nullptr; child = child->m_next) {
        if (child->m_type == XMLTYPE_ORIGIN
            && !XMLOriginGet(reinterpret_cast<XMLORIGIN*>(child), point, offset, status)) {
            status->Add(1, "%s %s: Error loading Origin element", anim->GetObjectTypeName(), LoadName(anim));
        }
    }
}

// The truncation the path walk uses to pick a segment: |value| + 65536 leaves the integer
// part in the mantissa above bit 7.
// ref: FUN_141343980
int16_t TruncateToShort(float value) {
    float shifted = std::fabs(value) + 65536.0f;
    uint32_t bits;
    std::memcpy(&bits, &shifted, sizeof(bits));
    int16_t sign = value < 0.0f || std::signbit(value) ? -1 : 0;
    auto magnitude = static_cast<int16_t>((bits >> 7) & 0x7fff);
    return static_cast<int16_t>((magnitude ^ sign) - sign);
}

}

// ---- names -------------------------------------------------------------------------------

// ref: FUN_1406675f0
bool StringToAnimCurveType(const char* name, uint8_t* type) {
    if (_stricmp("NONE", name) == 0) {
        *type = ANIMCURVE_NONE;
        return true;
    }
    if (_stricmp("SMOOTH", name) == 0) {
        *type = ANIMCURVE_SMOOTH;
        return true;
    }
    return false;
}

// ref: FUN_1406675c0
const char* AnimCurveTypeToString(uint8_t type) {
    switch (type) {
    case ANIMCURVE_NONE:
        return "NONE";
    case ANIMCURVE_SMOOTH:
        return "SMOOTH";
    default:
        return "UNKNOWN";
    }
}

namespace {

// 0x142cf5900
const struct {
    float m_in;
    float m_out;
    const char* m_name;
} s_smoothings[] = {
    {0.0f, 0.0f, "NONE"},
    {1.0f, 0.0f, "IN"},
    {0.0f, 1.0f, "OUT"},
    {1.0f, 1.0f, "IN_OUT"},
    {1.0f, 1.0f, "OUT_IN"},
};

}

// ref: FUN_140667770
bool StringToAnimSmoothing(const char* name, float* weights) {
    for (const auto& smoothing : s_smoothings) {
        if (_stricmp(smoothing.m_name, name) == 0) {
            weights[0] = smoothing.m_in;
            weights[1] = smoothing.m_out;
            return true;
        }
    }
    return false;
}

// ref: FUN_140667700
const char* AnimSmoothingToString(const float* weights) {
    for (const auto& smoothing : s_smoothings) {
        if (smoothing.m_in == weights[0] && smoothing.m_out == weights[1]) {
            return smoothing.m_name;
        }
    }
    return "UNKNOWN";
}

// ---- CSimpleAnimSmoothing ----------------------------------------------------------------

// ref: FUN_14060c1c0
void CSimpleAnimSmoothing::SetWeights(float in, float out) {
    if (in < 0.0f) {
        in = 0.0f;
    } else if (in >= 1.0f) {
        in = 1.0f;
    }
    this->m_in = in;
    if (out < 0.0f) {
        this->m_out = 0.0f;
        return;
    }
    if (out >= 1.0f) {
        out = 1.0f;
    }
    this->m_out = out;
}

// In alone eases in along a quarter cosine, out alone eases out along a quarter sine, both
// follow half a cosine.
// ref: FUN_140607a40
float CSimpleAnimSmoothing::Apply(float progress) {
    const float halfPi = 1.5707963705062866f;
    bool in = std::fabs(this->m_in) >= 0.001f;
    bool out = std::fabs(this->m_out) >= 0.001f;
    if (!in) {
        if (out) {
            return -std::cos((progress + 1.0f) * halfPi);
        }
        return progress;
    }
    if (out) {
        return 0.5f - std::cos(progress * 3.1415927410125732f) * 0.5f;
    }
    return 1.0f - std::cos(progress * halfPi);
}

// ---- CSimpleAnim ---------------------------------------------------------------------------

// ref: FUN_140603b30
CSimpleAnim::CSimpleAnim(CSimpleAnimGroup* group) {
    this->m_group = group;
}

// ref: FUN_140604280
CSimpleAnim::~CSimpleAnim() {
    delete this->m_smoothing;
}

// The reference's type id and its magic-static guard (CSimpleAnim), held here so translated code
// and the port number the class once, alike.
int32_t g_cSimpleAnimType;  // DAT_142e97aa4
int32_t g_cSimpleAnimTypeGuard;  // DAT_142e97aa8

int32_t CSimpleAnim::GetObjectType() {
    return FrameScript_TypeId(&g_cSimpleAnimType, &g_cSimpleAnimTypeGuard);
}

// ref: FUN_1406077f0
int32_t CSimpleAnim::GetScriptMetaTable() {
    return MetaTable(this, s_metaTable, s_restrictedMetaTable);
}

// ref: FUN_14060c350
bool CSimpleAnim::GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) {
    switch (index) {
    case ANIMSCRIPT_ONLOAD:
    case ANIMSCRIPT_ONFINISHED:
    case ANIMSCRIPT_ONPLAY:
    case ANIMSCRIPT_ONPAUSE:
        return true;
    case ANIMSCRIPT_ONSTOP:
        data->m_wrapper = "return function(self,requested) %s end";
        return true;
    case ANIMSCRIPT_ONUPDATE:
        data->m_wrapper = "return function(self,elapsed) %s end";
        return true;
    default:
        return false;
    }
}

// ref: FUN_140608120
bool CSimpleAnim::IsObjectTypeName(const char* name) {
    return _stricmp(name, "Animation") == 0 || _stricmp(name, "Object") == 0
        || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_140608080
bool CSimpleAnim::IsObjectType(int32_t type) {
    return type == CSimpleAnim::GetObjectType() || this->CScriptObject::IsObjectType(type);
}

// ref: FUN_1406075e0
const char* CSimpleAnim::GetObjectTypeName() {
    return "Animation";
}

// ref: FUN_140607420
uint32_t CSimpleAnim::GetObjectSize() {
    return 0x90;
}

// ref: FUN_140607a00
CScriptObject* CSimpleAnim::GetParent() {
    return this->m_group;
}

void CSimpleAnim::CopyTargetName(const char* name, uint8_t kind) {
    if (name == nullptr) {
        this->m_targetName[0] = '\0';
        this->m_targetKind = TARGET_OWNER;
        return;
    }
    CopyName63(this->m_targetName, name);
    this->m_targetKind = kind;
}

// ref: FUN_140608fc0
void CSimpleAnim::LoadXML(XMLOBJECT* node, CStatus* status) {
    this->m_animFlags |= ANIM_LOADING;
    if (!(node->m_flags & 0x80000)) {
        return;
    }
    LoadInherited(node, status, [&](XMLOBJECT* inherited) {
        this->LoadXML(inherited, status);
    });
    auto anim = reinterpret_cast<XMLANIMATION*>(node);
    if (anim->m_animFlags & 1) {
        this->SetParentKey(anim->m_parentKey.m_text);
    }
    if (anim->m_targetFlags & 8) {
        this->CopyTargetName(anim->m_targetKey.m_text, TARGET_KEY);
    }
    if (anim->m_targetFlags & 4) {
        this->CopyTargetName(anim->m_childKey.m_text, TARGET_CHILDKEY);
    }
    if (anim->m_targetFlags & 2) {
        const char* target = anim->m_target.m_text;
        if (target == nullptr) {
            this->m_targetName[0] = '\0';
            this->m_targetKind = TARGET_OWNER;
        } else {
            char fullName[0x400];
            this->CreateName(target, fullName, sizeof(fullName));
            if (strlen(fullName) < 0x40) {
                CopyName63(this->m_targetName, fullName);
                this->m_targetKind = TARGET_NAME;
            } else {
                FrameScript_ActionError("Error setting target (%s), full name was too long: (%s)",
                    *target != '\0' ? target : "empty name", fullName);
            }
        }
    }
    if (anim->m_animFlags & 2) {
        this->m_startDelay = anim->m_startDelay;
        if (this->m_startDelay < 0.0f) {
            this->m_startDelay = 0.0f;
        }
    }
    if (anim->m_animFlags & 4) {
        this->m_endDelay = anim->m_endDelay;
        if (this->m_endDelay < 0.0f) {
            this->m_endDelay = 0.0f;
        }
    }
    if (anim->m_animFlags & 8) {
        this->m_duration = anim->m_duration;
        if (this->m_duration < 0.0f) {
            this->m_duration = 0.0f;
        }
    }
    if (anim->m_hasSmoothing != 0) {
        float weights[2] = {0.0f, 0.0f};
        if (!StringToAnimSmoothing(anim->m_smoothing.m_text, weights)) {
            status->Add(1, "%s: Invalid smoothing value: %s", LoadName(this), anim->m_smoothing.m_text);
        } else {
            this->SetSmoothing(weights);
        }
    }
    if (anim->m_targetFlags & 1) {
        uint32_t order = static_cast<uint32_t>(anim->m_order);
        uint8_t value = 0;
        if (order < 100) {
            value = static_cast<uint8_t>(order);
        } else {
            status->Add(1, "%s: Invalid order value: %d. Order must be between %d and %d.", LoadName(this),
                static_cast<int32_t>(order), 1, 100);
            if (static_cast<int32_t>(order) >= 0) {
                value = 99;
            }
        }
        this->m_order = static_cast<int8_t>(value);
    }
    for (XMLOBJECT* child = node->m_child; child != nullptr; child = child->m_next) {
        if (child->m_type != XMLTYPE_SCRIPTS) {
            continue;
        }
        lua_State* L = FrameScript_GetContext();
        for (XMLOBJECT* handler = child->m_child; handler != nullptr; handler = handler->m_next) {
            if (handler->m_flags & 0x400) {
                this->LoadScriptXML(L, reinterpret_cast<XMLSCRIPTHANDLER*>(handler), status, 1);
            }
        }
    }
}

// ref: FUN_14060b380
void CSimpleAnim::PostLoadXML(XMLOBJECT* node, CStatus* status) {
    (void)node;
    (void)status;
    this->m_animFlags &= ~ANIM_LOADING;
    this->RunScript(ANIMSCRIPT_ONLOAD, nullptr);
}

void CSimpleAnim::Apply(float progress) {
    (void)progress;
}

// ref: FUN_1406076b0
CScriptObject* CSimpleAnim::GetChildKeyObject(const char* key) {
    if (key == nullptr || *key == '\0') {
        return nullptr;
    }
    CScriptObject* group = this->GetParent();
    if (group == nullptr) {
        return nullptr;
    }
    CScriptObject* owner = group->GetParent();
    if (owner == nullptr) {
        return nullptr;
    }
    return owner->GetObjectByPath(key);
}

// ref: FUN_140607b30
CAnimatableObject* CSimpleAnim::GetTargetObject() {
    CScriptObject* object;
    switch (this->m_targetKind) {
    case TARGET_NAME: {
        bool allowLockdown = (this->m_flags & FSOBJ_LOCKDOWN) || !(this->m_flags & FSOBJ_ACCESSIBLE);
        lua_State* L = FrameScript_GetContext();
        lua_pushstring(L, this->m_targetName);
        lua_rawget(L, LUA_GLOBALSINDEX);
        object = static_cast<CScriptObject*>(FrameScript_Object::GetObjectFromLua(
            nullptr, L, CScriptObject::GetObjectType(), -1, false, allowLockdown));
        lua_settop(L, -2);
        break;
    }
    case TARGET_KEY:
        object = this->GetObjectByPath(this->m_targetName);
        break;
    case TARGET_CHILDKEY:
        object = this->GetChildKeyObject(this->m_targetName);
        break;
    case TARGET_OBJECT:
        return this->m_target;
    default:
        return this->m_group != nullptr ? this->m_group->m_owner : nullptr;
    }
    return object != nullptr ? object->GetAnimatableObject() : nullptr;
}

// ref: FUN_140607c60
CScriptObject* CSimpleAnim::GetTarget() {
    CAnimatableObject* target = this->GetTargetObject();
    return target != nullptr ? target->GetScriptRegion() : nullptr;
}

// ref: FUN_140607690
CScriptObject* CSimpleAnim::GetRegionParent() {
    if (this->m_group != nullptr && this->m_group->m_owner != nullptr) {
        return this->m_group->m_owner->GetScriptRegion();
    }
    return nullptr;
}

// ref: FUN_140606f60
float CSimpleAnim::Update(float elapsed, bool reverse) {
    float previous = this->m_elapsed;
    float step = elapsed * this->m_group->m_speed;
    float now;
    if (!reverse) {
        now = previous + step;
        this->m_elapsed = now;
        if (!(now < this->GetTotalTime())) {
            this->m_state = STATE_DONE;
        }
    } else {
        now = previous - step;
        this->m_elapsed = now;
        if (!(0.0f < now)) {
            this->m_state = STATE_DONE;
        }
    }
    float progress;
    if (this->m_startDelay <= now) {
        progress = 1.0f;
        if (now <= this->m_duration + this->m_startDelay && 1.1754943508222875e-35f < std::fabs(this->m_duration)) {
            progress = (now - this->m_startDelay) / this->m_duration;
            if (progress < 0.0f) {
                progress = 0.0f;
            } else if (progress >= 1.0f) {
                progress = 1.0f;
            }
        }
    } else {
        progress = 0.0f;
    }
    this->m_progress = progress;
    if (this->m_smoothing != nullptr) {
        progress = this->m_smoothing->Apply(progress);
    }
    this->m_smoothProgress = progress;
    if (!reverse) {
        float total = this->GetTotalTime();
        if (total < this->m_elapsed) {
            step = total - previous;
        }
    } else if (this->m_elapsed < 0.0f) {
        step = previous;
    }
    return step;
}

// ref: FUN_14060b070
bool CSimpleAnim::Play() {
    this->m_target = this->GetTargetObject();
    if (this->m_state != STATE_PLAYING) {
        this->m_state = STATE_PLAYING;
        this->RunScript(ANIMSCRIPT_ONPLAY, nullptr);
    }
    return true;
}

// ref: FUN_14060af70
void CSimpleAnim::Pause() {
    this->m_group->Pause();
    if (this->m_state == STATE_PLAYING) {
        this->m_state = STATE_PAUSED;
        this->RunScript(ANIMSCRIPT_ONPAUSE, nullptr);
    }
}

// ref: FUN_14060c200
void CSimpleAnim::Stop(bool requested) {
    if (this->m_state == STATE_STOPPED) {
        return;
    }
    this->m_elapsed = 0.0f;
    this->m_progress = 0.0f;
    this->m_smoothProgress = 0.0f;
    this->m_state = STATE_STOPPED;
    this->RunScript(ANIMSCRIPT_ONSTOP, nullptr, requested);
}

// ref: FUN_140608dc0
bool CSimpleAnim::IsDelaying() {
    return s_epsilon <= std::fabs(this->GetTotalTime())
        && (this->m_elapsed < this->m_startDelay || this->m_duration + this->m_startDelay < this->m_elapsed);
}

// ref: FUN_140608d50
bool CSimpleAnim::IsDelayingNow() {
    if (std::fabs(this->GetTotalTime()) < s_epsilon) {
        return false;
    }
    CSimpleAnimGroup* group = this->m_group;
    bool reverse = group->m_playing ? group->m_reverse : false;
    if (reverse != (((group->m_groupFlags >> 6) & 1) != 0)) {
        return this->m_duration + this->m_startDelay < this->m_elapsed;
    }
    return this->m_elapsed < this->m_startDelay;
}

// ref: FUN_14060bfa0
void CSimpleAnim::SetStartDelay(float delay, bool recompute) {
    this->m_startDelay = delay;
    if (delay < 0.0f) {
        this->m_startDelay = 0.0f;
    }
    if (recompute && this->m_group != nullptr) {
        this->m_group->UpdateDuration();
    }
}

// ref: FUN_14060b9b0
void CSimpleAnim::SetEndDelay(float delay, bool recompute) {
    this->m_endDelay = delay;
    if (delay < 0.0f) {
        this->m_endDelay = 0.0f;
    }
    if (recompute && this->m_group != nullptr) {
        this->m_group->UpdateDuration();
    }
}

// ref: FUN_14060b980
void CSimpleAnim::SetDuration(float duration, bool recompute) {
    this->m_duration = duration;
    if (duration < 0.0f) {
        this->m_duration = 0.0f;
    }
    if (recompute && this->m_group != nullptr) {
        this->m_group->UpdateDuration();
    }
}

// ref: FUN_14060bae0
void CSimpleAnim::SetOrder(int32_t order) {
    if (this->m_group != nullptr) {
        this->m_group->SetAnimOrder(this, order);
    }
}

// ref: FUN_14060bb00
void CSimpleAnim::SetParentGroup(CSimpleAnimGroup* group, int32_t order) {
    if (this->m_group == group) {
        group->SetAnimOrder(this, order);
        return;
    }
    if (this->m_group != nullptr) {
        this->m_group->RemoveAnim(this);
        this->m_group->UpdateDuration();
    }
    this->m_group = group;
    if (order < 0) {
        order = 0;
    } else if (order > 99) {
        order = 99;
    }
    this->m_order = static_cast<int8_t>(order);
    group->AddAnim(this);
    group->UpdateDuration();
    uint16_t flags = this->m_group->m_flags;
    if (((flags & FSOBJ_FORBIDDEN) || (flags & FSOBJ_PROTECTED_METATABLE)) && (flags & FSOBJ_PROTECTED)) {
        this->SetProtected();
    }
}

// Weights of zero ask for no smoothing, which leaves any smoothing the animation has.
// ref: FUN_14060bec0
void CSimpleAnim::SetSmoothing(const float* weights) {
    float in = weights[0];
    float out = weights[1];
    if ((0.0f < in && in <= 1.0f) || (0.0f < out && out <= 1.0f)) {
        if (this->m_smoothing == nullptr) {
            this->m_smoothing = new (AllocAnim(0x10, 0x26c)) CSimpleAnimSmoothing();
            if (this->m_smoothing == nullptr) {
                return;
            }
        }
        this->m_smoothing->SetWeights(in, out);
    }
}

// ref: FUN_140607af0
void CSimpleAnim::GetSmoothing(float* weights) {
    if (this->m_smoothing != nullptr) {
        weights[0] = this->m_smoothing->m_in;
        weights[1] = this->m_smoothing->m_out;
        return;
    }
    weights[0] = 0.0f;
    weights[1] = 0.0f;
}

// ref: FUN_14060beb0
void CSimpleAnim::SetSmoothProgress(float progress) {
    this->m_smoothProgress = progress;
}

// ref: FUN_14060bfd0
bool CSimpleAnim::SetTarget(CScriptObject* target) {
    CAnimatableObject* animatable = target->GetAnimatableObject();
    if (animatable != nullptr) {
        this->m_target = animatable;
        this->m_targetKind = TARGET_OBJECT;
        return true;
    }
    FrameScript_ActionError("Error setting target %s", LoadName(target));
    return false;
}

// ref: FUN_14060c040
bool CSimpleAnim::SetTargetKey(const char* key) {
    this->CopyTargetName(key, TARGET_KEY);
    return true;
}

// ref: FUN_14060b920
bool CSimpleAnim::SetChildKey(const char* key) {
    this->CopyTargetName(key, TARGET_CHILDKEY);
    return true;
}

// ref: FUN_14060c090
bool CSimpleAnim::SetTargetName(const char* name) {
    if (name == nullptr) {
        this->m_targetName[0] = '\0';
        this->m_targetKind = TARGET_OWNER;
        return true;
    }
    char fullName[0x400];
    this->CreateName(name, fullName, sizeof(fullName));
    if (strlen(fullName) > 0x3f) {
        FrameScript_ActionError("Error setting target (%s), full name was too long: (%s)",
            *name != '\0' ? name : "empty name", fullName);
        return false;
    }
    CopyName63(this->m_targetName, fullName);
    this->m_targetKind = TARGET_NAME;
    return true;
}

// ref: FUN_14060c170
bool CSimpleAnim::SetTargetParent() {
    this->m_target = nullptr;
    this->m_targetKind = TARGET_OWNER;
    return true;
}

// ---- Alpha -----------------------------------------------------------------------------------

// ref: FUN_140603ad0
CSimpleAlphaAnim::CSimpleAlphaAnim(CSimpleAnimGroup* group) : CSimpleAnim(group) {
}

// The reference's type id and its magic-static guard (CSimpleAlphaAnim), held here so translated code
// and the port number the class once, alike.
int32_t g_cSimpleAlphaAnimType;  // DAT_142e97aec
int32_t g_cSimpleAlphaAnimTypeGuard;  // DAT_142e97af0

int32_t CSimpleAlphaAnim::GetObjectType() {
    return FrameScript_TypeId(&g_cSimpleAlphaAnimType, &g_cSimpleAlphaAnimTypeGuard);
}

// ref: FUN_1406077c0
int32_t CSimpleAlphaAnim::GetScriptMetaTable() {
    return MetaTable(this, s_metaTable, s_restrictedMetaTable);
}

// ref: FUN_140608010
bool CSimpleAlphaAnim::IsObjectTypeName(const char* name) {
    return _stricmp(name, "Alpha") == 0 || _stricmp(name, "Animation") == 0 || _stricmp(name, "Object") == 0
        || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_140607fd0
bool CSimpleAlphaAnim::IsObjectType(int32_t type) {
    return type == CSimpleAlphaAnim::GetObjectType() || this->CSimpleAnim::IsObjectType(type);
}

// ref: FUN_1406075d0
const char* CSimpleAlphaAnim::GetObjectTypeName() {
    return "Alpha";
}

// ref: FUN_140607410
uint32_t CSimpleAlphaAnim::GetObjectSize() {
    return 0x98;
}

// ref: FUN_140608e50
void CSimpleAlphaAnim::LoadXML(XMLOBJECT* node, CStatus* status) {
    this->CSimpleAnim::LoadXML(node, status);
    auto pair = reinterpret_cast<XMLANIMATIONPAIR*>(node);
    if (pair->m_pairFlags & 1) {
        float value = pair->m_first;
        if (value < 0.0f || 1.0f < value) {
            status->Add(1, "%s: Invalid fromAlpha value: %f. Value must be between %d and %d, inclusive.",
                LoadName(this), static_cast<double>(value), 0, 1);
        } else {
            this->SetFromAlpha(value);
        }
    }
    if (pair->m_pairFlags & 2) {
        float value = pair->m_second;
        if (value < 0.0f || 1.0f < value) {
            status->Add(1, "%s: Invalid toAlpha value: %f. Value must be between %d and %d, inclusive.",
                LoadName(this), static_cast<double>(value), 0, 1);
        } else {
            this->SetToAlpha(value);
        }
    }
}

// ref: FUN_140604ab0
void CSimpleAlphaAnim::Apply(float progress) {
    CAnimatableObject* target = this->m_target;
    if (target == nullptr || this->IsDelayingNow()) {
        return;
    }
    int32_t alpha = static_cast<int32_t>(static_cast<float>(static_cast<int32_t>(this->m_toAlpha - this->m_fromAlpha)) * progress)
        + this->m_fromAlpha;
    if (alpha < 0) {
        alpha = 0;
    }
    if (alpha > 0xff) {
        alpha = 0xff;
    }
    target->SetAnimAlpha(static_cast<uint8_t>(alpha));
}

// ref: FUN_14060b9e0
void CSimpleAlphaAnim::SetFromAlpha(float alpha) {
    if (alpha <= 0.0f) {
        alpha = 0.0f;
    }
    if (1.0f <= alpha) {
        alpha = 1.0f;
    }
    this->m_fromAlpha = static_cast<uint8_t>(static_cast<int32_t>(alpha * 255.0f));
}

// ref: FUN_14060c190
void CSimpleAlphaAnim::SetToAlpha(float alpha) {
    if (alpha <= 0.0f) {
        alpha = 0.0f;
    }
    if (1.0f <= alpha) {
        alpha = 1.0f;
    }
    this->m_toAlpha = static_cast<uint8_t>(static_cast<int32_t>(alpha * 255.0f));
}

// ref: FUN_140607390
float CSimpleAlphaAnim::GetFromAlpha() {
    return static_cast<float>(this->m_fromAlpha) * 0.003921568859368563f;
}

// ref: FUN_140607c90
float CSimpleAlphaAnim::GetToAlpha() {
    return static_cast<float>(this->m_toAlpha) * 0.003921568859368563f;
}

// ---- Translation -----------------------------------------------------------------------------

// ref: FUN_140603f20
CSimpleTranslationAnim::CSimpleTranslationAnim(CSimpleAnimGroup* group) : CSimpleAnim(group) {
}

// The reference's type id and its magic-static guard (CSimpleTranslationAnim), held here so translated code
// and the port number the class once, alike.
int32_t g_cSimpleTranslationAnimType;  // DAT_142e97aac
int32_t g_cSimpleTranslationAnimTypeGuard;  // DAT_142e97ab0

int32_t CSimpleTranslationAnim::GetObjectType() {
    return FrameScript_TypeId(&g_cSimpleTranslationAnimType, &g_cSimpleTranslationAnimTypeGuard);
}

// ref: FUN_1406079d0
int32_t CSimpleTranslationAnim::GetScriptMetaTable() {
    return MetaTable(this, s_metaTable, s_restrictedMetaTable);
}

// ref: FUN_140608c10
bool CSimpleTranslationAnim::IsObjectTypeName(const char* name) {
    return _stricmp(name, "Translation") == 0 || _stricmp(name, "Animation") == 0 || _stricmp(name, "Object") == 0
        || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_140608b70
bool CSimpleTranslationAnim::IsObjectType(int32_t type) {
    return type == CSimpleTranslationAnim::GetObjectType() || this->CSimpleAnim::IsObjectType(type);
}

// ref: FUN_140607680
const char* CSimpleTranslationAnim::GetObjectTypeName() {
    return "Translation";
}

// ref: FUN_140607410
uint32_t CSimpleTranslationAnim::GetObjectSize() {
    return 0x98;
}

// The offset is set before the template and the base settings are read.
// ref: FUN_14060a8c0
void CSimpleTranslationAnim::LoadXML(XMLOBJECT* node, CStatus* status) {
    auto pair = reinterpret_cast<XMLANIMATIONPAIR*>(node);
    float x = (pair->m_pairFlags & 1) ? XMLToLayout(pair->m_first) : 0.0f;
    float y = (pair->m_pairFlags & 2) ? XMLToLayout(pair->m_second) : 0.0f;
    if (x * x + y * y <= s_epsilon) {
        y = 0.0f;
        x = 0.0f;
    }
    this->m_offset[0] = x;
    this->m_offset[1] = y;
    this->CSimpleAnim::LoadXML(node, status);
}

// ref: FUN_140605220
void CSimpleTranslationAnim::Apply(float progress) {
    if (this->m_target != nullptr) {
        float offset[2] = {progress * this->m_offset[0], progress * this->m_offset[1]};
        this->m_target->Unknown40(offset);
    }
}

// ref: FUN_14060ba40
void CSimpleTranslationAnim::SetOffset(const float* offset) {
    if (s_epsilon < offset[0] * offset[0] + offset[1] * offset[1]) {
        this->m_offset[0] = offset[0];
        this->m_offset[1] = offset[1];
        return;
    }
    this->m_offset[0] = 0.0f;
    this->m_offset[1] = 0.0f;
}

// ---- LineTranslation -------------------------------------------------------------------------

// ref: FUN_140603cd0
CSimpleLineTranslationAnim::CSimpleLineTranslationAnim(CSimpleAnimGroup* group) : CSimpleTranslationAnim(group) {
}

// The reference's type id and its magic-static guard (CSimpleLineTranslationAnim), held here so translated code
// and the port number the class once, alike.
int32_t g_cSimpleLineTranslationAnimType;  // DAT_142e97ab4
int32_t g_cSimpleLineTranslationAnimTypeGuard;  // DAT_142e97ab8

int32_t CSimpleLineTranslationAnim::GetObjectType() {
    return FrameScript_TypeId(&g_cSimpleLineTranslationAnimType, &g_cSimpleLineTranslationAnimTypeGuard);
}

// ref: FUN_1406078e0
int32_t CSimpleLineTranslationAnim::GetScriptMetaTable() {
    return MetaTable(this, s_metaTable, s_restrictedMetaTable);
}

// ref: FUN_140608710
bool CSimpleLineTranslationAnim::IsObjectTypeName(const char* name) {
    return _stricmp(name, "LineTranslation") == 0 || _stricmp(name, "Translation") == 0
        || _stricmp(name, "Animation") == 0 || _stricmp(name, "Object") == 0
        || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_140608610
bool CSimpleLineTranslationAnim::IsObjectType(int32_t type) {
    return type == CSimpleLineTranslationAnim::GetObjectType() || type == CSimpleTranslationAnim::GetObjectType()
        || this->CSimpleAnim::IsObjectType(type);
}

// ref: FUN_140607630
const char* CSimpleLineTranslationAnim::GetObjectTypeName() {
    return "LineTranslation";
}

// ref: FUN_140604c50
void CSimpleLineTranslationAnim::Apply(float progress) {
    if (this->m_target != nullptr) {
        float offset[2] = {this->m_offset[0] * progress, this->m_offset[1] * progress};
        this->m_target->Unknown48(offset);
    }
}

// ---- TextureCoordTranslation -----------------------------------------------------------------

// ref: FUN_140603ec0
CSimpleTexCoordTranslationAnim::CSimpleTexCoordTranslationAnim(CSimpleAnimGroup* group) : CSimpleAnim(group) {
}

// The reference's type id and its magic-static guard (CSimpleTexCoordTranslationAnim), held here so translated code
// and the port number the class once, alike.
int32_t g_cSimpleTexCoordTranslationAnimType;  // DAT_142e97abc
int32_t g_cSimpleTexCoordTranslationAnimTypeGuard;  // DAT_142e97ac0

int32_t CSimpleTexCoordTranslationAnim::GetObjectType() {
    return FrameScript_TypeId(&g_cSimpleTexCoordTranslationAnimType, &g_cSimpleTexCoordTranslationAnimTypeGuard);
}

// ref: FUN_1406079a0
int32_t CSimpleTexCoordTranslationAnim::GetScriptMetaTable() {
    return MetaTable(this, s_metaTable, s_restrictedMetaTable);
}

// ref: FUN_140608b00
bool CSimpleTexCoordTranslationAnim::IsObjectTypeName(const char* name) {
    return _stricmp(name, "TextureCoordTranslation") == 0 || _stricmp(name, "Animation") == 0
        || _stricmp(name, "Object") == 0 || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_140608ac0
bool CSimpleTexCoordTranslationAnim::IsObjectType(int32_t type) {
    return type == CSimpleTexCoordTranslationAnim::GetObjectType() || this->CSimpleAnim::IsObjectType(type);
}

// ref: FUN_140607670
const char* CSimpleTexCoordTranslationAnim::GetObjectTypeName() {
    return "TextureCoordTranslation";
}

// ref: FUN_140607410
uint32_t CSimpleTexCoordTranslationAnim::GetObjectSize() {
    return 0x98;
}

// ref: FUN_14060a860
void CSimpleTexCoordTranslationAnim::LoadXML(XMLOBJECT* node, CStatus* status) {
    auto pair = reinterpret_cast<XMLANIMATIONPAIR*>(node);
    float u = (pair->m_pairFlags & 1) ? pair->m_first : 0.0f;
    float v = (pair->m_pairFlags & 2) ? pair->m_second : 0.0f;
    if (u * u + v * v <= s_epsilon) {
        v = 0.0f;
        u = 0.0f;
    }
    this->m_offset[0] = u;
    this->m_offset[1] = v;
    this->CSimpleAnim::LoadXML(node, status);
}

// ref: FUN_1406051e0
void CSimpleTexCoordTranslationAnim::Apply(float progress) {
    if (this->m_target != nullptr) {
        float offset[2] = {progress * this->m_offset[0], progress * this->m_offset[1]};
        this->m_target->Unknown50(offset);
    }
}

// ---- Rotation --------------------------------------------------------------------------------

// ref: FUN_140603dd0
CSimpleRotationAnim::CSimpleRotationAnim(CSimpleAnimGroup* group) : CSimpleAnim(group) {
}

// The reference's type id and its magic-static guard (CSimpleRotationAnim), held here so translated code
// and the port number the class once, alike.
int32_t g_cSimpleRotationAnimType;  // DAT_142e97ac4
int32_t g_cSimpleRotationAnimTypeGuard;  // DAT_142e97ac8

int32_t CSimpleRotationAnim::GetObjectType() {
    return FrameScript_TypeId(&g_cSimpleRotationAnimType, &g_cSimpleRotationAnimTypeGuard);
}

// ref: FUN_140607940
int32_t CSimpleRotationAnim::GetScriptMetaTable() {
    return MetaTable(this, s_metaTable, s_restrictedMetaTable);
}

// ref: FUN_140608940
bool CSimpleRotationAnim::IsObjectTypeName(const char* name) {
    return _stricmp(name, "Rotation") == 0 || _stricmp(name, "Animation") == 0 || _stricmp(name, "Object") == 0
        || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_1406088a0
bool CSimpleRotationAnim::IsObjectType(int32_t type) {
    return type == CSimpleRotationAnim::GetObjectType() || this->CSimpleAnim::IsObjectType(type);
}

// ref: FUN_140607650
const char* CSimpleRotationAnim::GetObjectTypeName() {
    return "Rotation";
}

// ref: FUN_140607460
uint32_t CSimpleRotationAnim::GetObjectSize() {
    return 0xa0;
}

// ref: FUN_14060a2f0
void CSimpleRotationAnim::LoadXML(XMLOBJECT* node, CStatus* status) {
    auto pair = reinterpret_cast<XMLANIMATIONPAIR*>(node);
    if (pair->m_pairFlags & 1) {
        this->m_radians = pair->m_first * 0.01745329238474369f;
    }
    if (pair->m_pairFlags & 2) {
        this->m_radians = pair->m_second;
    }
    this->CSimpleAnim::LoadXML(node, status);
    LoadOrigins(this, node, status, &this->m_originPoint, this->m_origin);
}

// ref: FUN_140605100
void CSimpleRotationAnim::Apply(float progress) {
    if (this->m_target != nullptr) {
        this->m_target->Unknown58(static_cast<int32_t>(this->m_originPoint), this->m_origin, progress * this->m_radians);
    }
}

// ---- Scale -----------------------------------------------------------------------------------

// ref: FUN_140603e40
CSimpleScaleAnim::CSimpleScaleAnim(CSimpleAnimGroup* group) : CSimpleAnim(group) {
}

// The reference's type id and its magic-static guard (CSimpleScaleAnim), held here so translated code
// and the port number the class once, alike.
int32_t g_cSimpleScaleAnimType;  // DAT_142e97acc
int32_t g_cSimpleScaleAnimTypeGuard;  // DAT_142e97ad0

int32_t CSimpleScaleAnim::GetObjectType() {
    return FrameScript_TypeId(&g_cSimpleScaleAnimType, &g_cSimpleScaleAnimTypeGuard);
}

// ref: FUN_140607970
int32_t CSimpleScaleAnim::GetScriptMetaTable() {
    return MetaTable(this, s_metaTable, s_restrictedMetaTable);
}

// ref: FUN_140608a50
bool CSimpleScaleAnim::IsObjectTypeName(const char* name) {
    return _stricmp(name, "Scale") == 0 || _stricmp(name, "Animation") == 0 || _stricmp(name, "Object") == 0
        || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_1406089b0
bool CSimpleScaleAnim::IsObjectType(int32_t type) {
    return type == CSimpleScaleAnim::GetObjectType() || this->CSimpleAnim::IsObjectType(type);
}

// ref: FUN_140607660
const char* CSimpleScaleAnim::GetObjectTypeName() {
    return "Scale";
}

// ref: FUN_140607440
uint32_t CSimpleScaleAnim::GetObjectSize() {
    return 0xb0;
}

// scaleX/Y scale by; fromScaleX/Y and toScaleX/Y scale from one to the other. A value under
// 0.001 counts as 0.001.
// ref: FUN_14060a3f0
void CSimpleScaleAnim::LoadXML(XMLOBJECT* node, CStatus* status) {
    auto scale = reinterpret_cast<XMLANIMATIONSCALE*>(node);
    const char* invalid = "%s: Invalid %s value: %f. Value must be at least %f.";
    uint32_t flags = scale->m_scaleFlags;
    float x = 0.0f;
    if (flags & 1) {
        x = scale->m_scaleX;
        if (x < 0.0f) {
            status->Add(1, invalid, LoadName(this), "scaleX", static_cast<double>(x), 0.0);
        }
    }
    float y = 0.0f;
    if (flags & 2) {
        y = scale->m_scaleY;
        if (y < 0.0f) {
            status->Add(1, invalid, LoadName(this), "scaleY", static_cast<double>(y), 0.0);
        }
    }
    if (flags & 3) {
        this->m_animFlags &= ~ANIM_SCALEFROMTO;
        this->m_scale[0] = 1.0f - (x <= 0.001f ? 0.001f : x);
        this->m_scale[1] = 1.0f - (y <= 0.001f ? 0.001f : y);
        this->m_fromScale[0] = 0.0f;
        this->m_fromScale[1] = 0.0f;
    }
    float fromX = 0.0f;
    if (flags & 4) {
        fromX = scale->m_fromScaleX;
        if (fromX < 0.0f) {
            status->Add(1, invalid, LoadName(this), "fromScaleX", static_cast<double>(fromX), 0.0);
        }
    }
    float fromY = 0.0f;
    if (flags & 8) {
        fromY = scale->m_fromScaleY;
        if (fromY < 0.0f) {
            status->Add(1, invalid, LoadName(this), "fromScaleY", static_cast<double>(fromY), 0.0);
        }
    }
    if (flags & 0xc) {
        this->m_animFlags |= ANIM_SCALEFROMTO;
        this->m_fromScale[0] = 1.0f - (fromX <= 0.001f ? 0.001f : fromX);
        this->m_fromScale[1] = 1.0f - (fromY <= 0.001f ? 0.001f : fromY);
    }
    // The "to" values reuse the "by" values' variables, so a missing toScaleX takes scaleX.
    if (flags & 0x10) {
        x = scale->m_toScaleX;
        if (x < 0.0f) {
            status->Add(1, invalid, LoadName(this), "toScaleX", static_cast<double>(x), 0.0);
        }
    }
    if (flags & 0x20) {
        y = scale->m_toScaleY;
        if (y < 0.0f) {
            status->Add(1, invalid, LoadName(this), "toScaleY", static_cast<double>(y), 0.0);
        }
    }
    if (flags & 0x30) {
        this->m_animFlags |= ANIM_SCALEFROMTO;
        this->m_scale[0] = 1.0f - (x <= 0.001f ? 0.001f : x);
        this->m_scale[1] = 1.0f - (y <= 0.001f ? 0.001f : y);
    }
    this->CSimpleAnim::LoadXML(node, status);
    LoadOrigins(this, node, status, &this->m_originPoint, this->m_origin);
}

// ref: FUN_140605130
void CSimpleScaleAnim::Apply(float progress) {
    if (this->m_target == nullptr) {
        return;
    }
    float scale[2];
    if (this->m_animFlags & ANIM_SCALEFROMTO) {
        scale[0] = (this->m_scale[0] - this->m_fromScale[0]) * progress + this->m_fromScale[0];
        scale[1] = (this->m_scale[1] - this->m_fromScale[1]) * progress + this->m_fromScale[1];
    } else {
        scale[0] = progress * this->m_scale[0];
        scale[1] = progress * this->m_scale[1];
    }
    this->m_target->Unknown60(static_cast<int32_t>(this->m_originPoint), this->m_origin, scale);
}

// ref: FUN_14060bdc0
void CSimpleScaleAnim::SetScale(const float* scale) {
    this->m_animFlags &= ~ANIM_SCALEFROMTO;
    this->m_scale[0] = 1.0f - (scale[0] <= 0.001f ? 0.001f : scale[0]);
    this->m_fromScale[0] = 0.0f;
    this->m_fromScale[1] = 0.0f;
    this->m_scale[1] = 1.0f - (scale[1] <= 0.001f ? 0.001f : scale[1]);
}

// ref: FUN_14060be10
void CSimpleScaleAnim::SetScaleFrom(const float* scale) {
    this->m_animFlags |= ANIM_SCALEFROMTO;
    this->m_fromScale[0] = 1.0f - (scale[0] <= 0.001f ? 0.001f : scale[0]);
    this->m_fromScale[1] = 1.0f - (scale[1] <= 0.001f ? 0.001f : scale[1]);
}

// ref: FUN_14060be60
void CSimpleScaleAnim::SetScaleTo(const float* scale) {
    this->m_animFlags |= ANIM_SCALEFROMTO;
    this->m_scale[0] = 1.0f - (scale[0] <= 0.001f ? 0.001f : scale[0]);
    this->m_scale[1] = 1.0f - (scale[1] <= 0.001f ? 0.001f : scale[1]);
}

// ref: FUN_140607700
void CSimpleScaleAnim::GetScale(float* scale) {
    if (!(this->m_animFlags & ANIM_SCALEFROMTO)) {
        scale[0] = 1.0f - this->m_scale[0];
        scale[1] = 1.0f - this->m_scale[1];
        return;
    }
    scale[0] = 0.0f;
    scale[1] = 0.0f;
}

// ref: FUN_140607740
void CSimpleScaleAnim::GetScaleFrom(float* scale) {
    if (this->m_animFlags & ANIM_SCALEFROMTO) {
        scale[0] = 1.0f - this->m_fromScale[0];
        scale[1] = 1.0f - this->m_fromScale[1];
        return;
    }
    scale[0] = 0.0f;
    scale[1] = 0.0f;
}

// ref: FUN_140607780
void CSimpleScaleAnim::GetScaleTo(float* scale) {
    if (this->m_animFlags & ANIM_SCALEFROMTO) {
        scale[0] = 1.0f - this->m_scale[0];
        scale[1] = 1.0f - this->m_scale[1];
        return;
    }
    scale[0] = 0.0f;
    scale[1] = 0.0f;
}

// ---- LineScale -------------------------------------------------------------------------------

// A Scale with the LineScale vtable (FUN_140605be0 and FUN_140609520 make it so).
CSimpleLineScaleAnim::CSimpleLineScaleAnim(CSimpleAnimGroup* group) : CSimpleScaleAnim(group) {
}

// The reference's type id and its magic-static guard (CSimpleLineScaleAnim), held here so translated code
// and the port number the class once, alike.
int32_t g_cSimpleLineScaleAnimType;  // DAT_142e97ad4
int32_t g_cSimpleLineScaleAnimTypeGuard;  // DAT_142e97ad8

int32_t CSimpleLineScaleAnim::GetObjectType() {
    return FrameScript_TypeId(&g_cSimpleLineScaleAnimType, &g_cSimpleLineScaleAnimTypeGuard);
}

// ref: FUN_1406078b0
int32_t CSimpleLineScaleAnim::GetScriptMetaTable() {
    return MetaTable(this, s_metaTable, s_restrictedMetaTable);
}

// ref: FUN_140608590
bool CSimpleLineScaleAnim::IsObjectTypeName(const char* name) {
    return _stricmp(name, "LineScale") == 0 || _stricmp(name, "Scale") == 0 || _stricmp(name, "Animation") == 0
        || _stricmp(name, "Object") == 0 || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_140608490
bool CSimpleLineScaleAnim::IsObjectType(int32_t type) {
    return type == CSimpleLineScaleAnim::GetObjectType() || type == CSimpleScaleAnim::GetObjectType()
        || this->CSimpleAnim::IsObjectType(type);
}

// ref: FUN_140607620
const char* CSimpleLineScaleAnim::GetObjectTypeName() {
    return "LineScale";
}

// ref: FUN_140604ba0
void CSimpleLineScaleAnim::Apply(float progress) {
    if (this->m_target == nullptr) {
        return;
    }
    float scale[2];
    if (this->m_animFlags & ANIM_SCALEFROMTO) {
        scale[0] = (this->m_scale[0] - this->m_fromScale[0]) * progress + this->m_fromScale[0];
        scale[1] = (this->m_scale[1] - this->m_fromScale[1]) * progress + this->m_fromScale[1];
    } else {
        scale[0] = progress * this->m_scale[0];
        scale[1] = progress * this->m_scale[1];
    }
    this->m_target->Unknown68(static_cast<int32_t>(this->m_originPoint), this->m_origin, scale);
}

// ---- FlipBook --------------------------------------------------------------------------------

// ref: FUN_140603c60
CSimpleFlipBookAnim::CSimpleFlipBookAnim(CSimpleAnimGroup* group) : CSimpleAnim(group) {
}

// The reference's type id and its magic-static guard (CSimpleFlipBookAnim), held here so translated code
// and the port number the class once, alike.
int32_t g_cSimpleFlipBookAnimType;  // DAT_142e97af4
int32_t g_cSimpleFlipBookAnimTypeGuard;  // DAT_142e97af8

int32_t CSimpleFlipBookAnim::GetObjectType() {
    return FrameScript_TypeId(&g_cSimpleFlipBookAnimType, &g_cSimpleFlipBookAnimTypeGuard);
}

// ref: FUN_140607880
int32_t CSimpleFlipBookAnim::GetScriptMetaTable() {
    return MetaTable(this, s_metaTable, s_restrictedMetaTable);
}

// ref: FUN_140608420
bool CSimpleFlipBookAnim::IsObjectTypeName(const char* name) {
    return _stricmp(name, "FlipBook") == 0 || _stricmp(name, "Animation") == 0 || _stricmp(name, "Object") == 0
        || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_140608380
bool CSimpleFlipBookAnim::IsObjectType(int32_t type) {
    return type == CSimpleFlipBookAnim::GetObjectType() || this->CSimpleAnim::IsObjectType(type);
}

// ref: FUN_140607610
const char* CSimpleFlipBookAnim::GetObjectTypeName() {
    return "FlipBook";
}

// ref: FUN_140607430
uint32_t CSimpleFlipBookAnim::GetObjectSize() {
    return 0xa8;
}

// ref: FUN_140609fb0
void CSimpleFlipBookAnim::LoadXML(XMLOBJECT* node, CStatus* status) {
    this->CSimpleAnim::LoadXML(node, status);
    auto book = reinterpret_cast<XMLANIMATIONFLIPBOOK*>(node);
    if (book->m_flipBookFlags & 1) {
        if (book->m_rows == 0) {
            status->Add(1, "%s: Invalid flipBookRows value: %u. Value must be greater than 0.", LoadName(this), 0);
        } else {
            this->m_rows = book->m_rows;
        }
    }
    if (book->m_flipBookFlags & 2) {
        if (book->m_columns == 0) {
            status->Add(1, "%s: Invalid flipBookColumns value: %u. Value must be greater than 0.", LoadName(this), 0);
        } else {
            this->m_columns = book->m_columns;
        }
    }
    if (book->m_flipBookFlags & 4) {
        this->m_frames = book->m_frames;
    }
    if (book->m_flipBookFlags & 8) {
        this->m_frameWidth = book->m_frameWidth;
    }
    if (book->m_flipBookFlags & 0x10) {
        this->m_frameHeight = book->m_frameHeight;
    }
}

// ref: FUN_140604b30
void CSimpleFlipBookAnim::Apply(float progress) {
    CAnimatableObject* target = this->m_target;
    if (target != nullptr && !this->IsDelayingNow()) {
        target->Unknown78(progress, this->m_rows, this->m_columns, this->m_frames, this->m_frameWidth,
            this->m_frameHeight);
    }
}

// ---- CSimpleControlPoint ---------------------------------------------------------------------

// ref: FUN_140604350
CSimpleControlPoint::~CSimpleControlPoint() {
    this->m_link.Unlink();
}

// The reference's type id and its magic-static guard (CSimpleControlPoint), held here so translated code
// and the port number the class once, alike.
int32_t g_cSimpleControlPointType;  // DAT_142e97adc
int32_t g_cSimpleControlPointTypeGuard;  // DAT_142e97ae0

int32_t CSimpleControlPoint::GetObjectType() {
    return FrameScript_TypeId(&g_cSimpleControlPointType, &g_cSimpleControlPointTypeGuard);
}

// ref: FUN_140607850
int32_t CSimpleControlPoint::GetScriptMetaTable() {
    return MetaTable(this, s_metaTable, s_restrictedMetaTable);
}

// ref: FUN_140608320
bool CSimpleControlPoint::IsObjectTypeName(const char* name) {
    return _stricmp(name, "ControlPoint") == 0 || _stricmp(name, "Object") == 0
        || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_140608280
bool CSimpleControlPoint::IsObjectType(int32_t type) {
    return type == CSimpleControlPoint::GetObjectType() || this->CScriptObject::IsObjectType(type);
}

// ref: FUN_140607600
const char* CSimpleControlPoint::GetObjectTypeName() {
    return "ControlPoint";
}

// ref: FUN_14038ab70
uint32_t CSimpleControlPoint::GetObjectSize() {
    return 0x40;
}

// ref: FUN_140607a30
CScriptObject* CSimpleControlPoint::GetParent() {
    return this->m_path;
}

// ref: FUN_140609dd0
void CSimpleControlPoint::LoadXML(XMLOBJECT* node, CStatus* status) {
    LoadInherited(node, status, [&](XMLOBJECT* inherited) {
        this->LoadXML(inherited, status);
    });
    auto point = reinterpret_cast<XMLCONTROLPOINT*>(node);
    if (point->m_pointFlags & 4) {
        this->SetParentKey(point->m_parentKey.m_text);
    }
    float x = (point->m_pointFlags & 1) ? XMLToLayout(point->m_offsetX) : 0.0f;
    float y = (point->m_pointFlags & 2) ? XMLToLayout(point->m_offsetY) : 0.0f;
    if (x * x + y * y <= s_epsilon) {
        this->m_offset[0] = 0.0f;
        this->m_offset[1] = 0.0f;
    } else {
        this->m_offset[0] = x;
        this->m_offset[1] = y;
    }
}

// ref: FUN_14060ba10
void CSimpleControlPoint::SetOffset(const float* offset) {
    if (s_epsilon < offset[0] * offset[0] + offset[1] * offset[1]) {
        this->m_offset[0] = offset[0];
        this->m_offset[1] = offset[1];
        return;
    }
    this->m_offset[0] = 0.0f;
    this->m_offset[1] = 0.0f;
}

// ref: FUN_14060ba80
void CSimpleControlPoint::SetOrder(int32_t order, bool notify, bool isDefault) {
    int8_t previous = this->m_order;
    int8_t value;
    if (!isDefault) {
        value = order < 0 ? 0 : static_cast<int8_t>(order > 99 ? 99 : order);
    } else if (order < 0) {
        value = 99;
    } else {
        value = order > 99 ? 0 : static_cast<int8_t>(order);
    }
    this->m_order = value;
    if (this->m_path != nullptr && notify && previous != value) {
        this->m_path->OnControlPointOrderChanged(this);
    }
}

// ref: FUN_14060bbb0
bool CSimpleControlPoint::SetParentPath(CSimpleAnimPath* path) {
    if (path == nullptr || this->m_path == path) {
        return false;
    }
    if (this->m_path != nullptr) {
        this->m_link.Unlink();
    }
    this->m_link.Unlink();
    path->m_points.LinkToTail(this);
    CSimpleAnimPath* previous = this->m_path;
    this->m_path = path;
    if (previous != nullptr && this->m_order != -1 && previous->HasOrder(this->m_order)) {
        previous->SortControlPoints();
        previous->UpdateControlPointTimes();
    }
    if (this->m_order != -1 && path->HasOrder(this->m_order)) {
        path->SortControlPoints();
        path->UpdateControlPointTimes();
        return true;
    }
    if (path->InsertSorted(this) >= 0) {
        path->UpdateControlPointTimes();
    }
    return true;
}

// ---- CONTROLPOINTARRAY -----------------------------------------------------------------------

// Grows to the next multiple of the granularity: the largest power of two up to the count
// below 32 points, 32 from then on.
// ref: FUN_14060b860
void CONTROLPOINTARRAY::Reserve(uint32_t count) {
    uint32_t needed = this->m_count + count;
    if (needed <= this->m_capacity) {
        return;
    }
    uint32_t granularity = this->m_granularity;
    if (granularity == 0) {
        if (needed < 0x20) {
            granularity = needed;
            for (uint32_t bits = (needed - 1) & needed; bits != 0; bits = (bits - 1) & bits) {
                granularity = bits;
            }
            if (granularity == 0) {
                granularity = 1;
            }
        } else {
            this->m_granularity = 0x20;
            granularity = 0x20;
        }
    }
    if (needed % granularity != 0) {
        needed += granularity - needed % granularity;
    }
    // FUN_1402ef6f0
    auto data = static_cast<CSimpleControlPoint**>(SMemAlloc(needed * sizeof(CSimpleControlPoint*), 8,
        s_growableArrayTag, SStrHashFnv1a(s_growableArrayTag), -2, 0));
    for (uint32_t i = 0; i < this->m_count; i++) {
        data[i] = this->m_data[i];
    }
    if (this->m_data != nullptr) {
        SMemFree(this->m_data, s_growableArrayTag, -2, 0);
    }
    this->m_data = data;
    this->m_capacity = needed;
}

void CONTROLPOINTARRAY::Clear() {
    if (this->m_data != nullptr) {
        SMemFree(this->m_data, s_fixedArrayTag, -2, 0);
    }
    this->m_capacity = 0;
    this->m_count = 0;
    this->m_data = nullptr;
}

// ---- Path ------------------------------------------------------------------------------------

// ref: FUN_140603d30
CSimpleAnimPath::CSimpleAnimPath(CSimpleAnimGroup* group) : CSimpleAnim(group) {
    this->m_points.InitEmpty(0x18);
}

// ref: FUN_1406044a0
CSimpleAnimPath::~CSimpleAnimPath() {
    this->m_sorted.Clear();
    while (CSimpleControlPoint* point = this->m_points.Head()) {
        point->~CSimpleControlPoint();
        SMemFree(point, s_pointListTag, -2, 0);
    }
    this->m_sorted.Clear();
    this->m_points.UnlinkAll();
    this->m_points.m_terminator.Unlink();
}

// The reference's type id and its magic-static guard (CSimpleAnimPath), held here so translated code
// and the port number the class once, alike.
int32_t g_cSimpleAnimPathType;  // DAT_142e97ae4
int32_t g_cSimpleAnimPathTypeGuard;  // DAT_142e97ae8

int32_t CSimpleAnimPath::GetObjectType() {
    return FrameScript_TypeId(&g_cSimpleAnimPathType, &g_cSimpleAnimPathTypeGuard);
}

// ref: FUN_140607910
int32_t CSimpleAnimPath::GetScriptMetaTable() {
    return MetaTable(this, s_metaTable, s_restrictedMetaTable);
}

// ref: FUN_140608830
bool CSimpleAnimPath::IsObjectTypeName(const char* name) {
    return _stricmp(name, "Path") == 0 || _stricmp(name, "Animation") == 0 || _stricmp(name, "Object") == 0
        || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_140608790
bool CSimpleAnimPath::IsObjectType(int32_t type) {
    return type == CSimpleAnimPath::GetObjectType() || this->CSimpleAnim::IsObjectType(type);
}

// ref: FUN_140607640
const char* CSimpleAnimPath::GetObjectTypeName() {
    return "Path";
}

// ref: FUN_140607450
uint32_t CSimpleAnimPath::GetObjectSize() {
    return 0xd0;
}

// ref: FUN_14060a0d0
void CSimpleAnimPath::LoadXML(XMLOBJECT* node, CStatus* status) {
    this->CSimpleAnim::LoadXML(node, status);
    auto path = reinterpret_cast<XMLANIMATIONPATH*>(node);
    if (path->m_pathFlags & 1) {
        uint8_t curve;
        if (StringToAnimCurveType(path->m_curve.m_text, &curve)) {
            this->m_curve = curve;
        } else {
            status->Add(1, "%s %s: Invalid curve value: %s", this->GetObjectTypeName(), LoadName(this),
                path->m_curve.m_text);
        }
    }
    XMLOBJECT* points = XMLObjectFindChild(node, XMLTYPE_CONTROLPOINTS);
    if (points == nullptr) {
        return;
    }
    for (XMLOBJECT* child = points->m_child; child != nullptr; child = child->m_next) {
        if (child->m_type != XMLTYPE_CONTROLPOINT) {
            status->Add(1, "%s %s: Error loading Origin element", this->GetObjectTypeName(), LoadName(this));
            continue;
        }
        auto point = new (AllocAnim(0x40, 0x515)) CSimpleControlPoint();
        point->SetParentPath(this);
        if (child->m_attributes & 0x40) {
            point->SetName(child->m_name.m_text);
        }
        point->FrameScript_Object::LoadXML(child, status, nullptr);
        point->LoadXML(child, status);
    }
}

// ref: FUN_14060b4d0
void CSimpleAnimPath::PostLoadXML(XMLOBJECT* node, CStatus* status) {
    (void)node;
    (void)status;
    this->m_animFlags &= ~ANIM_LOADING;
    this->SortControlPoints();
    this->UpdateControlPointTimes();
    this->RunScript(ANIMSCRIPT_ONLOAD, nullptr);
}

// The points are walked from the origin: straight from one to the next, or through a
// Catmull-Rom spline (the origin mirrored before the first point, the last segment
// continued past the last).
// ref: FUN_140604ca0
void CSimpleAnimPath::Apply(float progress) {
    uint32_t count = this->m_sorted.m_count;
    if (count == 0 || this->m_target == nullptr) {
        return;
    }
    float t = std::fabs(progress);
    int16_t segment = TruncateToShort(static_cast<float>(count) * t);
    auto index = static_cast<uint32_t>(static_cast<int32_t>(segment));
    CSimpleControlPoint** points = this->m_sorted.m_data;
    float x;
    float y;
    if (count <= index) {
        x = points[count - 1]->m_offset[0];
        y = points[count - 1]->m_offset[1];
    } else if (this->m_curve == ANIMCURVE_NONE || count < 2) {
        float ax = 0.0f;
        float ay = 0.0f;
        float at = 0.0f;
        CSimpleControlPoint* b = points[index];
        if (segment != 0) {
            CSimpleControlPoint* a = points[index - 1];
            ax = a->m_offset[0];
            ay = a->m_offset[1];
            at = a->m_time;
        }
        float f = (t - at) / (b->m_time - at);
        x = (b->m_offset[0] - ax) * f + ax;
        y = (b->m_offset[1] - ay) * f + ay;
    } else {
        float p0x;
        float p0y;
        float p1x;
        float p1y;
        float ta;
        float p2x;
        float p2y;
        float tb;
        float p3x;
        float p3y;
        if (segment == 0) {
            p1x = 0.0f;
            p1y = 0.0f;
            ta = 0.0f;
            p2x = points[0]->m_offset[0];
            p2y = points[0]->m_offset[1];
            tb = points[0]->m_time;
            p0x = -p2x;
            p0y = -p2y;
            p3x = points[1]->m_offset[0];
            p3y = points[1]->m_offset[1];
        } else {
            CSimpleControlPoint* b = points[index];
            CSimpleControlPoint* a = points[index - 1];
            p2x = b->m_offset[0];
            p2y = b->m_offset[1];
            p1x = a->m_offset[0];
            p1y = a->m_offset[1];
            ta = a->m_time;
            tb = b->m_time;
            if (index == 1) {
                p0x = 0.0f;
                p0y = 0.0f;
            } else {
                p0x = points[index - 2]->m_offset[0];
                p0y = points[index - 2]->m_offset[1];
            }
            if (index != count - 1) {
                p3x = points[index + 1]->m_offset[0];
                p3y = points[index + 1]->m_offset[1];
            } else {
                p3x = (p2x + p2x) - p1x;
                p3y = (p2y + p2y) - p1y;
            }
        }
        float f = (t - ta) / (tb - ta);
        float f2 = f * f;
        x = ((((-p0x + p1x * 3.0f) - p2x * 3.0f) + p3x) * f2 * f + (-p0x + p2x) * f
                + ((p2x * 4.0f + ((p0x + p0x) - p1x * 5.0f)) - p3x) * f2 + p1x + p1x)
            * 0.5f;
        y = (((p2y * 4.0f + ((p0y + p0y) - p1y * 5.0f)) - p3y) * f2 + (((-p0y + p1y * 3.0f) - p2y * 3.0f) + p3y) * f2 * f)
                * 0.5f
            + p1y * 0.5f + p1y * 0.5f + (-p0y + p2y) * f * 0.5f;
    }
    if (progress < 0.0f) {
        x = -x;
        y = -y;
    }
    float offset[2] = {x, y};
    this->m_target->Unknown40(offset);
}

// ref: FUN_1406061e0
CSimpleControlPoint* CSimpleAnimPath::CreateControlPoint(const char* name, const char* inherits, const int32_t* order) {
    char inheritsName[0x80] = {};
    XMLOBJECT* inherited = nullptr;
    const char* taint = nullptr;
    bool busy = false;
    if (inherits != nullptr) {
        busy = true;
        CopyInheritsName(inheritsName, inherits);
        inherited = FrameXML_AcquireVirtual(inheritsName, &taint, &busy);
        if (inherited == nullptr) {
            FrameScript_ActionError("Couldn't find inherited node \"%s\"", inheritsName);
            return nullptr;
        }
        if (busy) {
            FrameScript_ActionError("Recursively inherited node \"%s\"", inheritsName);
            return nullptr;
        }
    }
    auto point = new (AllocAnim(0x40, 0x55d)) CSimpleControlPoint();
    point->SetParentPath(this);
    if (name != nullptr && *name != '\0') {
        point->SetName(name);
    }
    if (inherited != nullptr) {
        CScriptStatus status;
        status.m_warn = true;
        point->LoadXML(inherited, &status);
    }
    if (order != nullptr) {
        int32_t value = *order;
        point->m_order = static_cast<int8_t>(value < 0 ? 0 : (value > 99 ? 99 : value));
    }
    if (point->m_order == -1) {
        int8_t value = 99;
        if (this->m_sorted.m_count != 0) {
            int8_t last = this->m_sorted.m_data[this->m_sorted.m_count - 1]->m_order;
            if (last >= 0) {
                value = last > 99 ? 0 : last;
            }
        }
        point->m_order = value;
    }
    if (point->m_order != -1 && this->HasOrder(point->m_order)) {
        this->SortControlPoints();
        this->UpdateControlPointTimes();
    } else if (this->InsertSorted(point) >= 0) {
        this->UpdateControlPointTimes();
    }
    if (inherited != nullptr && !busy) {
        FrameXML_ReleaseVirtual(inheritsName);
    }
    return point;
}

// Points are taken in creation order; a point without an order, or with one below the
// order the previous point reached, takes the next free order. A point whose order is
// already taken is left out.
// ref: FUN_140605260
void CSimpleAnimPath::SortControlPoints() {
    this->Stop(false);
    CSimpleControlPoint* point = this->m_points.Head();
    if (point == nullptr) {
        this->m_sorted.Clear();
        return;
    }
    for (uint32_t i = 0; i < this->m_sorted.m_count; i++) {
        this->m_sorted.m_data[i] = nullptr;
    }
    int32_t next = 0;
    for (; point != nullptr; point = this->m_points.Link(point)->Next()) {
        int32_t order = point->m_order;
        if (order == -1 || order < next) {
            int32_t value = next < 0 ? 0 : (next > 99 ? 99 : next);
            point->m_order = static_cast<int8_t>(value);
            order = next;
        }
        next = order + 1 < 100 ? order + 1 : 0;
        int8_t key = point->m_order;
        uint32_t count = this->m_sorted.m_count;
        if (count == 0) {
            this->m_sorted.Reserve(1);
            this->m_sorted.m_data[this->m_sorted.m_count++] = point;
            continue;
        }
        uint32_t slot = count;
        bool placed = false;
        for (uint32_t i = 0; i < count; i++) {
            CSimpleControlPoint* other = this->m_sorted.m_data[i];
            if (other == nullptr) {
                this->m_sorted.m_data[i] = point;
                placed = true;
                break;
            }
            if (key == other->m_order) {
                placed = true;
                break;
            }
            if (key < other->m_order) {
                slot = i;
                break;
            }
        }
        if (placed) {
            continue;
        }
        this->m_sorted.Reserve(1);
        uint32_t last = this->m_sorted.m_count++;
        for (uint32_t i = last; i > slot; i--) {
            this->m_sorted.m_data[i] = this->m_sorted.m_data[i - 1];
        }
        this->m_sorted.m_data[slot] = point;
    }
    // Trailing empty slots (from points that were left out) go.
    int32_t last = static_cast<int32_t>(this->m_sorted.m_count) - 1;
    while (last >= 0 && this->m_sorted.m_data[last] == nullptr) {
        last--;
    }
    if (last < 0) {
        return;
    }
    uint32_t count = static_cast<uint32_t>(last + 1);
    if (count >= this->m_sorted.m_count) {
        return;
    }
    this->m_sorted.m_count = count;
    auto data = static_cast<CSimpleControlPoint**>(SMemAlloc(count * sizeof(CSimpleControlPoint*), 8,
        s_fixedArrayTag, SStrHashFnv1a(s_fixedArrayTag), -7, 0));
    for (uint32_t i = 0; i < count; i++) {
        data[i] = this->m_sorted.m_data[i];
    }
    SMemFree(this->m_sorted.m_data, s_growableArrayTag, -2, 0);
    this->m_sorted.m_data = data;
    this->m_sorted.m_capacity = count;
}

// ref: FUN_140605980
void CSimpleAnimPath::UpdateControlPointTimes() {
    uint32_t count = this->m_sorted.m_count;
    if (count == 0) {
        return;
    }
    float step = 1.0f / static_cast<float>(count);
    for (uint32_t i = 0; i < count; i++) {
        float time = static_cast<float>(i + 1) * step;
        if (time < 0.0f) {
            time = 0.0f;
        } else if (1.0f <= time) {
            time = 1.0f;
        }
        this->m_sorted.m_data[i]->m_time = time;
    }
}

bool CSimpleAnimPath::HasOrder(int8_t order) {
    int32_t low = 0;
    int32_t high = static_cast<int32_t>(this->m_sorted.m_count) - 1;
    while (low <= high) {
        auto middle = static_cast<uint32_t>(low + high) >> 1;
        CSimpleControlPoint* point = this->m_sorted.m_data[middle];
        if (point != nullptr) {
            if (point->m_order < order) {
                low = static_cast<int32_t>(middle) + 1;
            } else if (point->m_order <= order) {
                return true;
            } else {
                high = static_cast<int32_t>(middle) - 1;
            }
        }
    }
    return false;
}

// A point whose new order is another point's moves just before that point in the list, so
// the re-sort keeps them in that order.
// ref: FUN_14060a9f0
void CSimpleAnimPath::OnControlPointOrderChanged(CSimpleControlPoint* point) {
    if (point == nullptr) {
        return;
    }
    int8_t order = point->m_order;
    if (order != -1) {
        int32_t low = 0;
        int32_t high = static_cast<int32_t>(this->m_sorted.m_count) - 1;
        while (low <= high) {
            auto middle = static_cast<uint32_t>(low + high) >> 1;
            CSimpleControlPoint* other = this->m_sorted.m_data[middle];
            if (other == nullptr) {
                continue;
            }
            if (other->m_order < order) {
                low = static_cast<int32_t>(middle) + 1;
            } else if (other->m_order <= order) {
                if (other != point) {
                    TSLink<CSimpleControlPoint>* link = &point->m_link;
                    link->Unlink();
                    TSLink<CSimpleControlPoint>* otherLink = &other->m_link;
                    link->m_prevlink = otherLink->m_prevlink;
                    link->m_next = link->m_prevlink->m_next;
                    link->m_prevlink->m_next = point;
                    otherLink->m_prevlink = link;
                }
                break;
            } else {
                high = static_cast<int32_t>(middle) - 1;
            }
        }
    }
    this->SortControlPoints();
    this->UpdateControlPointTimes();
}

// ref: FUN_14060b520
int32_t CSimpleAnimPath::InsertSorted(CSimpleControlPoint* point) {
    uint32_t count = this->m_sorted.m_count;
    if (count == 0) {
        this->m_sorted.Reserve(1);
        this->m_sorted.m_data[this->m_sorted.m_count++] = point;
        return 0;
    }
    uint32_t slot = count;
    for (uint32_t i = 0; i < count; i++) {
        CSimpleControlPoint* other = this->m_sorted.m_data[i];
        if (other == nullptr) {
            this->m_sorted.m_data[i] = point;
            return static_cast<int32_t>(i);
        }
        if (point->m_order == other->m_order) {
            return static_cast<int32_t>(i);
        }
        if (point->m_order < other->m_order) {
            slot = i;
            break;
        }
    }
    this->m_sorted.Reserve(1);
    uint32_t last = this->m_sorted.m_count++;
    for (uint32_t i = last; i > slot; i--) {
        this->m_sorted.m_data[i] = this->m_sorted.m_data[i - 1];
    }
    this->m_sorted.m_data[slot] = point;
    return static_cast<int32_t>(slot);
}

// ref: FUN_1406073e0
int32_t CSimpleAnimPath::GetMaxControlPointOrder() {
    if (this->m_sorted.m_count == 0) {
        return -1;
    }
    return this->m_sorted.m_data[this->m_sorted.m_count - 1]->m_order;
}

// ref: FUN_140607270
CONTROLPOINTLIST* CSimpleAnimPath::GetScriptControlPoints(CONTROLPOINTLIST* points) {
    uint32_t count = this->m_sorted.m_count;
    points->reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        CSimpleControlPoint* point = i < this->m_sorted.m_count ? this->m_sorted.m_data[i] : nullptr;
        if ((point->m_flags & FSOBJ_ACCESSIBLE) && !(point->m_flags & FSOBJ_HIDDEN)) {
            points->push_back(point);
        }
    }
    return points;
}

// ---- creation by name ------------------------------------------------------------------------

// ref: FUN_140605be0
CSimpleAnim* CSimpleAnim_Create(CSimpleAnimGroup* group, const char* type) {
    if (type == nullptr || *type == '\0') {
        return new (AllocAnim(0x90, 0x8f7)) CSimpleAnim(group);
    }
    if (_stricmp(type, "Animation") == 0) {
        return new (AllocAnim(0x90, 0x8df)) CSimpleAnim(group);
    }
    if (_stricmp(type, "Translation") == 0) {
        return new (AllocAnim(0x98, 0x8e1)) CSimpleTranslationAnim(group);
    }
    if (_stricmp(type, "LineTranslation") == 0) {
        return new (AllocAnim(0x98, 0x8e3)) CSimpleLineTranslationAnim(group);
    }
    if (_stricmp(type, "TextureCoord") == 0) {
        return new (AllocAnim(0x98, 0x8e5)) CSimpleTexCoordTranslationAnim(group);
    }
    if (_stricmp(type, "Rotation") == 0) {
        return new (AllocAnim(0xa0, 0x8e7)) CSimpleRotationAnim(group);
    }
    if (_stricmp(type, "Scale") == 0) {
        return new (AllocAnim(0xb0, 0x8e9)) CSimpleScaleAnim(group);
    }
    if (_stricmp(type, "LineScale") == 0) {
        return new (AllocAnim(0xb0, 0x8eb)) CSimpleLineScaleAnim(group);
    }
    if (_stricmp(type, "Path") == 0) {
        return new (AllocAnim(0xd0, 0x8ed)) CSimpleAnimPath(group);
    }
    if (_stricmp(type, "Alpha") == 0) {
        return new (AllocAnim(0x98, 0x8ef)) CSimpleAlphaAnim(group);
    }
    if (_stricmp(type, "FlipBook") == 0) {
        return new (AllocAnim(0xa8, 0x8f1)) CSimpleFlipBookAnim(group);
    }
    return new (AllocAnim(0x90, 0x8f3)) CSimpleAnim(group);
}
