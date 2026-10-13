// CSimpleAnimGroup (Engine/Source/Frame/CSimpleAnimGroup.cpp): an animation group, the orders
// of animations it plays one after another, and the objects it tells it is playing.

#include "ui/CSimpleAnim.hpp"

#include <cstring>
#include <new>

#include "base/Status.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "ui/BinaryXML.hpp"
#include "ui/CAnimatableObject.hpp"
#include "ui/CScriptRegion.hpp"
#include "ui/FrameXML.hpp"

extern const char s_animTag[] = ".PEAVCSimpleAnim@@";
extern const char s_animOrderTag[] = ".?AV?$vector@PEAVCSimpleAnim@@V?$allocator@PEAVCSimpleAnim@@@blz@@@blz@@";
extern const char s_animatableTag[] = ".PEAVCAnimatableObject@@";
extern const char s_controlPointTag[] = ".PEAVCSimpleControlPoint@@";

int32_t CSimpleAnimGroup::s_metaTable = -2;
int32_t CSimpleAnimGroup::s_restrictedMetaTable = -2;

namespace {

const char s_animSource[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Frame\\CSimpleAnim.cpp";

// The loop states OnLoop and GetLoopState report.
const char* LoopState(bool playing, bool reverse) {
    if (!playing) {
        return "NONE";
    }
    return reverse ? "REVERSE" : "FORWARD";
}

// Copies a template name as the loaders do (at most 127 characters).
void CopyInheritsName(char* name, const char* source) {
    uint32_t i = 0;
    for (; i < 0x7f && source[i] != '\0'; i++) {
        name[i] = source[i];
    }
    name[i] = '\0';
}

// An animation of each element kind, made as the group's loader makes them.
CSimpleAnim* NewAnim(uint32_t type, CSimpleAnimGroup* group) {
    uint32_t hash = SStrHashFnv1a(s_animSource);
    switch (type) {
    case XMLTYPE_ANIMATION:
        return new (SMemAlloc(0x90, 0x1f, s_animSource, hash, 0x7b9, 0)) CSimpleAnim(group);
    case XMLTYPE_ALPHA:
        return new (SMemAlloc(0x98, 0x1f, s_animSource, hash, 0x7e9, 0)) CSimpleAlphaAnim(group);
    case XMLTYPE_FLIPBOOK:
        return new (SMemAlloc(0xa8, 0x1f, s_animSource, hash, 0x7ef, 0)) CSimpleFlipBookAnim(group);
    case XMLTYPE_TRANSLATION:
        return new (SMemAlloc(0x98, 0x1f, s_animSource, hash, 0x7bf, 0)) CSimpleTranslationAnim(group);
    case XMLTYPE_LINETRANSLATION:
        return new (SMemAlloc(0x98, 0x1f, s_animSource, hash, 0x7c5, 0)) CSimpleLineTranslationAnim(group);
    case XMLTYPE_TEXCOORDTRANSLATION:
        return new (SMemAlloc(0x98, 0x1f, s_animSource, hash, 0x7cb, 0)) CSimpleTexCoordTranslationAnim(group);
    case XMLTYPE_ROTATION:
        return new (SMemAlloc(0xa0, 0x1f, s_animSource, hash, 0x7d1, 0)) CSimpleRotationAnim(group);
    case XMLTYPE_PATH:
        return new (SMemAlloc(0xd0, 0x1f, s_animSource, hash, 0x7e3, 0)) CSimpleAnimPath(group);
    case XMLTYPE_SCALE:
        return new (SMemAlloc(0xb0, 0x1f, s_animSource, hash, 0x7d7, 0)) CSimpleScaleAnim(group);
    case XMLTYPE_LINESCALE:
        return new (SMemAlloc(0xb0, 0x1f, s_animSource, hash, 0x7dd, 0)) CSimpleLineScaleAnim(group);
    default:
        return nullptr;
    }
}

// An animation element read into a new animation, which then joins the group.
// ref: FUN_14060a950
void LoadAnimXML(CSimpleAnimGroup* group, XMLOBJECT* node, CStatus* status, CSimpleAnim* anim) {
    if (anim == nullptr) {
        return;
    }
    uint16_t flags = group->m_flags;
    if (((flags & FSOBJ_FORBIDDEN) || (flags & FSOBJ_PROTECTED_METATABLE)) && (flags & FSOBJ_PROTECTED)) {
        anim->SetProtected();
    }
    anim->m_animFlags |= CSimpleAnim::ANIM_LOADING;
    if (node->m_attributes & 0x40) {
        anim->SetName(node->m_name.m_text);
    }
    anim->FrameScript_Object::LoadXML(node, status, nullptr);
    anim->LoadXML(node, status);
    group->AddAnim(anim);
}

// The longest animation of an order.
float OrderDuration(ANIMLIST& anims) {
    float longest = 0.0f;
    for (CSimpleAnim* anim : anims) {
        float total = anim->GetTotalTime();
        if (total > longest) {
            longest = total;
        }
    }
    return longest;
}

}

// ref: FUN_140667690
bool StringToAnimLoopType(const char* name, uint8_t* type) {
    static const struct {
        uint8_t m_type;
        const char* m_name;
    } s_loops[] = {{ANIMLOOP_NONE, "NONE"}, {ANIMLOOP_REPEAT, "REPEAT"}, {ANIMLOOP_BOUNCE, "BOUNCE"}};
    for (const auto& loop : s_loops) {
        if (_stricmp(loop.m_name, name) == 0) {
            *type = loop.m_type;
            return true;
        }
    }
    return false;
}

// ref: FUN_140667660
const char* AnimLoopTypeToString(uint8_t type) {
    switch (type) {
    case ANIMLOOP_NONE:
        return "NONE";
    case ANIMLOOP_REPEAT:
        return "REPEAT";
    case ANIMLOOP_BOUNCE:
        return "BOUNCE";
    default:
        return "UNKNOWN";
    }
}

// ref: FUN_140603b90
CSimpleAnimGroup::CSimpleAnimGroup(CAnimatableObject* owner) {
    if (this->m_owner != owner) {
        if (this->m_owner != nullptr) {
            this->m_owner->RemoveGroup(this);
        }
        this->m_owner = owner;
        owner->AddGroup(this);
        uint16_t flags = static_cast<CScriptObject*>(this->m_owner->GetScriptRegion())->m_flags;
        if (((flags & FSOBJ_FORBIDDEN) || (flags & FSOBJ_PROTECTED_METATABLE)) && (flags & FSOBJ_PROTECTED)) {
            this->SetProtected();
        }
    }
}

// ref: FUN_140604010
CSimpleAnimGroup::~CSimpleAnimGroup() {
    for (ANIMLIST& anims : this->m_orders) {
        for (CSimpleAnim* anim : anims) {
            delete anim;
        }
    }
    this->m_playing = false;
    this->m_animated.clear();
    this->m_orders.clear();
    this->m_playingLink.Unlink();
    this->m_groupLink.Unlink();
}

// The reference's type id and its magic-static guard (CSimpleAnimGroup), held here so translated code
// and the port number the class once, alike.
int32_t g_cSimpleAnimGroupType;  // DAT_142e97afc
int32_t g_cSimpleAnimGroupTypeGuard;  // DAT_142e97b00

int32_t CSimpleAnimGroup::GetObjectType() {
    return FrameScript_TypeId(&g_cSimpleAnimGroupType, &g_cSimpleAnimGroupTypeGuard);
}

// ref: FUN_140607820
int32_t CSimpleAnimGroup::GetScriptMetaTable() {
    if (!(this->m_flags & FSOBJ_FORBIDDEN) && !(this->m_flags & FSOBJ_PROTECTED_METATABLE)) {
        return s_metaTable;
    }
    return s_restrictedMetaTable;
}

// ref: FUN_14060c3b0
bool CSimpleAnimGroup::GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) {
    switch (index) {
    case ANIMSCRIPT_ONLOAD:
    case ANIMSCRIPT_ONPLAY:
    case ANIMSCRIPT_ONPAUSE:
        return true;
    case ANIMSCRIPT_ONFINISHED:
    case ANIMSCRIPT_ONSTOP:
        data->m_wrapper = "return function(self,requested) %s end";
        return true;
    case ANIMSCRIPT_ONUPDATE:
        data->m_wrapper = "return function(self,elapsed) %s end";
        return true;
    case ANIMSCRIPT_ONLOOP:
        data->m_wrapper = "return function(self,loopState) %s end";
        return true;
    default:
        return false;
    }
}

// ref: FUN_14060ae00
void CSimpleAnimGroup::OnProtected() {
    for (ANIMLIST& anims : this->m_orders) {
        for (CSimpleAnim* anim : anims) {
            anim->SetProtected();
        }
    }
}

// ref: FUN_140608220
bool CSimpleAnimGroup::IsObjectTypeName(const char* name) {
    return _stricmp(name, "AnimationGroup") == 0 || _stricmp(name, "Object") == 0
        || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_140608180
bool CSimpleAnimGroup::IsObjectType(int32_t type) {
    return type == CSimpleAnimGroup::GetObjectType() || this->CScriptObject::IsObjectType(type);
}

// ref: FUN_1406075f0
const char* CSimpleAnimGroup::GetObjectTypeName() {
    return "AnimationGroup";
}

// ref: FUN_140607420
uint32_t CSimpleAnimGroup::GetObjectSize() {
    return 0x90;
}

// ref: FUN_140607a10
CScriptObject* CSimpleAnimGroup::GetParent() {
    if (this->m_owner != nullptr) {
        return this->m_owner->GetScriptRegion();
    }
    return nullptr;
}

// ref: FUN_14060b500
void CSimpleAnimGroup::BeginLoadXML(XMLOBJECT* node) {
    this->m_groupFlags |= GROUP_LOADING;
    if (node->m_attributes & 0x40) {
        this->SetName(node->m_name.m_text);
    }
}

// ref: FUN_140609520
void CSimpleAnimGroup::LoadXML(XMLOBJECT* node, CStatus* status) {
    this->m_groupFlags |= GROUP_LOADING;
    if (node->m_attributes & 0x100) {
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
            this->LoadXML(inherited, status);
        }
        if (inherited != nullptr && !busy) {
            FrameXML_ReleaseVirtual(name);
        }
    }
    auto group = reinterpret_cast<XMLANIMATIONGROUP*>(node);
    if (group->m_groupFlags & 1) {
        this->SetParentKey(group->m_parentKey.m_text);
    }
    if (group->m_groupFlags & 2) {
        FrameXML_AddToParentArray(this, group->m_parentArray.m_text, status);
    }
    if (group->m_groupFlags & 4) {
        const char* value = group->m_looping.m_text;
        uint8_t looping;
        if (StringToAnimLoopType(value, &looping)) {
            this->m_looping = looping;
        } else {
            const char* name = this->GetName();
            if (name == nullptr) {
                name = this->GetObjectTypeName();
            }
            status->Add(1, "%s %s: Invalid looping value: %s", this->GetObjectTypeName(), name, value);
        }
    }
    if (group->m_groupFlags & 8) {
        if (group->m_groupFlags & 0x10) {
            this->m_groupFlags |= GROUP_SETTOFINALALPHA;
        } else {
            this->m_groupFlags &= ~GROUP_SETTOFINALALPHA;
        }
    }
    if (group->m_groupFlags & 0x20) {
        this->m_speed = group->m_speedMultiplier;
    }
    for (XMLOBJECT* child = node->m_child; child != nullptr; child = child->m_next) {
        if (child->m_type == XMLTYPE_SCRIPTS) {
            lua_State* L = FrameScript_GetContext();
            for (XMLOBJECT* handler = child->m_child; handler != nullptr; handler = handler->m_next) {
                if (handler->m_flags & 0x400) {
                    this->LoadScriptXML(L, reinterpret_cast<XMLSCRIPTHANDLER*>(handler), status, 1);
                }
            }
            continue;
        }
        LoadAnimXML(this, child, status, NewAnim(child->m_type, this));
    }
}

// ref: FUN_14060b3a0
void CSimpleAnimGroup::PostLoadXML(XMLOBJECT* node, CStatus* status) {
    this->m_groupFlags &= ~GROUP_LOADING;
    for (ANIMLIST& anims : this->m_orders) {
        for (CSimpleAnim* anim : anims) {
            anim->PostLoadXML(node, status);
        }
    }
    this->UpdateDuration();
    this->RunScript(ANIMSCRIPT_ONLOAD, nullptr);
}

// The orders stay sorted; an animation joins the list of its order, or a new list where
// its order belongs.
// ref: FUN_140604670
void CSimpleAnimGroup::AddAnim(CSimpleAnim* anim) {
    uint64_t index = 0;
    for (; index < this->m_orders.size(); index++) {
        int8_t order = this->m_orders[index][0]->m_order;
        if (order == anim->m_order) {
            this->m_orders[index].push_back(anim);
            return;
        }
        if (anim->m_order <= order) {
            break;
        }
    }
    this->m_orders.emplace_back();
    for (uint64_t i = this->m_orders.size() - 1; i > index; i--) {
        this->m_orders[i].swap(this->m_orders[i - 1]);
    }
    this->m_orders[index].push_back(anim);
}

// ref: FUN_14060b6d0
void CSimpleAnimGroup::RemoveAnim(CSimpleAnim* anim) {
    for (uint64_t index = 0; index < this->m_orders.size(); index++) {
        ANIMLIST& anims = this->m_orders[index];
        if (anims[0] == nullptr || anims[0]->m_order != anim->m_order) {
            continue;
        }
        for (uint64_t i = 0; i < anims.size(); i++) {
            if (anims[i] != anim) {
                continue;
            }
            anims[i] = anims[anims.size() - 1];
            anims[anims.size() - 1] = anim;
            anims.m_size--;
            if (anims.size() != 0) {
                return;
            }
            for (uint64_t j = index; j + 1 < this->m_orders.size(); j++) {
                this->m_orders[j].swap(this->m_orders[j + 1]);
            }
            this->m_orders.pop_back();
            if (!this->m_playing) {
                return;
            }
            if (this->m_currentOrder >= 0 && static_cast<uint64_t>(this->m_currentOrder) < this->m_orders.size()) {
                return;
            }
            this->Stop(false);
            return;
        }
    }
}

// ref: FUN_140605890
void CSimpleAnimGroup::SetAnimOrder(CSimpleAnim* anim, int32_t order) {
    if (anim->m_order == order) {
        return;
    }
    this->RemoveAnim(anim);
    if (order < 0) {
        order = 0;
    } else if (order > 99) {
        order = 99;
    }
    anim->m_order = static_cast<int8_t>(order);
    this->AddAnim(anim);
    this->UpdateDuration();
}

// ref: FUN_140605900
void CSimpleAnimGroup::UpdateDuration() {
    this->m_duration = 0.0f;
    float duration = 0.0f;
    for (ANIMLIST& anims : this->m_orders) {
        duration += OrderDuration(anims);
        this->m_duration = duration;
    }
}

// ref: FUN_1406059f0
CSimpleAnim* CSimpleAnimGroup::CreateAnimation(const char* type, const char* name, const char* inherits) {
    char inheritsName[0x80] = {};
    XMLOBJECT* inherited = nullptr;
    const char* taint = nullptr;
    bool busy = false;
    CSimpleAnim* anim = nullptr;
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
    anim = CSimpleAnim_Create(this, type != nullptr ? type : "Animation");
    if (anim != nullptr) {
        if (name != nullptr && *name != '\0') {
            anim->SetName(name);
        }
        if (inherited != nullptr) {
            CScriptStatus status;
            status.m_warn = true;
            anim->LoadXML(inherited, &status);
            anim->PostLoadXML(inherited, &status);
        }
        this->AddAnim(anim);
        this->UpdateDuration();
    }
    if (inherited != nullptr && !busy) {
        FrameXML_ReleaseVirtual(inheritsName);
    }
    return anim;
}

// ref: FUN_14060b650
void CSimpleAnimGroup::RemoveAnimations() {
    this->m_orders.clear();
    this->UpdateDuration();
}

// ref: FUN_140607130
ANIMLIST* CSimpleAnimGroup::GetScriptAnimations(ANIMLIST* anims) {
    uint64_t count = 0;
    for (ANIMLIST& order : this->m_orders) {
        count += order.size();
    }
    anims->reserve(count);
    for (ANIMLIST& order : this->m_orders) {
        for (CSimpleAnim* anim : order) {
            if ((anim->m_flags & FSOBJ_ACCESSIBLE) && !(anim->m_flags & FSOBJ_HIDDEN)) {
                anims->push_back(anim);
            }
        }
    }
    return anims;
}

// ref: FUN_140603620
void CSimpleAnimGroup::PlayCurrentOrder() {
    ANIMLIST& anims = this->m_orders[this->m_currentOrder];
    for (CSimpleAnim** it = anims.begin(), **end = anims.end(); it != end; it++) {
        CSimpleAnim* anim = *it;
        anim->m_target = anim->GetTargetObject();
        if (anim->m_state != CSimpleAnim::STATE_PLAYING) {
            anim->m_state = CSimpleAnim::STATE_PLAYING;
            anim->RunScript(ANIMSCRIPT_ONPLAY, nullptr);
        }
    }
}

void CSimpleAnimGroup::Finished() {
    this->RemoveAnimatedObjects();
    uint8_t flags = this->m_groupFlags;
    this->m_groupFlags = flags & 0xb0;
    this->m_playing = false;
    this->m_currentOrder = 0;
    this->m_elapsed = 0.0f;
    this->RunScript(ANIMSCRIPT_ONFINISHED, nullptr, static_cast<bool>((flags >> 1) & 1));
}

// ref: FUN_14060b0b0
bool CSimpleAnimGroup::Play(bool reverse, float offset) {
    if (this->m_orders.size() == 0) {
        return false;
    }
    uint8_t flags = this->m_groupFlags;
    if (!(flags & GROUP_PAUSED)) {
        if (this->m_playing) {
            bool flip = (flags >> 6) & 1;
            if (reverse == (this->m_reverse != flip)) {
                return true;
            }
            if (this->m_looping != ANIMLOOP_NONE) {
                return true;
            }
            this->m_groupFlags = flags ^ GROUP_FLIPPED;
            return true;
        }
        this->m_groupFlags = flags & 0xb0;
        this->m_playing = false;
        this->m_currentOrder = reverse ? static_cast<int8_t>(this->m_orders.size() - 1) : 0;
        this->m_elapsed = 0.0f;
        this->m_reverse = reverse;
        this->m_playing = true;
        for (ANIMLIST& anims : this->m_orders) {
            for (CSimpleAnim* anim : anims) {
                if (!reverse) {
                    anim->m_progress = 0.0f;
                    anim->m_smoothProgress = 0.0f;
                    anim->m_elapsed = 0.0f;
                    anim->m_state = CSimpleAnim::STATE_STOPPED;
                } else {
                    anim->m_smoothProgress = 1.0f;
                    anim->m_progress = 1.0f;
                    anim->m_state = CSimpleAnim::STATE_STOPPED;
                    anim->m_elapsed = anim->GetTotalTime();
                }
            }
        }
    } else {
        this->m_groupFlags = flags & ~GROUP_PAUSED;
    }
    this->m_groupFlags |= GROUP_STARTING;
    this->RunScript(ANIMSCRIPT_ONPLAY, nullptr);
    this->PlayCurrentOrder();
    this->AddAnimatedObjects();
    this->m_groupFlags &= ~GROUP_STARTING;
    if (this->m_playing) {
        bool done = this->Update(offset);
        this->RunScript(ANIMSCRIPT_ONUPDATE, nullptr, offset);
        if (done) {
            this->Finished();
        }
        if (this->m_groupFlags & GROUP_PAUSED) {
            this->Pause();
        }
        return true;
    }
    this->Stop(true);
    return false;
}

// ref: FUN_14060afb0
void CSimpleAnimGroup::Pause() {
    if (!this->m_playing || (this->m_groupFlags & GROUP_PAUSED)) {
        return;
    }
    this->m_groupFlags |= GROUP_PAUSED;
    if (this->m_groupFlags & GROUP_STARTING) {
        return;
    }
    ANIMLIST& anims = this->m_orders[this->m_currentOrder];
    for (CSimpleAnim** it = anims.begin(), **end = anims.end(); it != end; it++) {
        (*it)->Pause();
    }
    this->RunScript(ANIMSCRIPT_ONPAUSE, nullptr);
}

// ref: FUN_14060c230
void CSimpleAnimGroup::Stop(bool requested) {
    if (!this->m_playing) {
        return;
    }
    uint8_t flags = this->m_groupFlags;
    if (flags & GROUP_STOPPING) {
        return;
    }
    this->m_groupFlags = flags | GROUP_STOPPING;
    if (flags & GROUP_STARTING) {
        return;
    }
    for (ANIMLIST& anims : this->m_orders) {
        for (CSimpleAnim* anim : anims) {
            anim->Stop(requested);
        }
    }
    this->RemoveAnimatedObjects();
    this->m_groupFlags &= 0xb0;
    this->m_playing = false;
    this->m_currentOrder = 0;
    this->m_elapsed = 0.0f;
    this->RunScript(ANIMSCRIPT_ONSTOP, nullptr, requested);
}

// ref: FUN_14060b8e0
void CSimpleAnimGroup::Restart(bool reverse, float offset) {
    this->Stop(true);
    this->Play(reverse, offset);
}

// ref: FUN_14060bda0
void CSimpleAnimGroup::SetPlaying(bool play) {
    if (play) {
        this->Play(false, 0.0f);
        return;
    }
    this->Stop(true);
}

// ref: FUN_1406070e0
void CSimpleAnimGroup::Finish() {
    if (this->m_duration > 0.0001f
        && (this->m_looping == ANIMLOOP_REPEAT || this->m_looping == ANIMLOOP_BOUNCE
            || this->m_elapsed / this->m_duration < 1.0f)
        && this->m_playing) {
        this->m_groupFlags |= GROUP_FINISHING;
    }
}

// ref: FUN_140608d20
bool CSimpleAnimGroup::IsPlayingReverse() {
    bool flip = (this->m_groupFlags >> 6) & 1;
    if (this->m_playing) {
        return this->m_reverse != flip;
    }
    return flip;
}

// ref: FUN_140608e00
bool CSimpleAnimGroup::IsDone() {
    if (this->m_duration <= 0.0001f) {
        return true;
    }
    if (this->m_looping != ANIMLOOP_REPEAT && this->m_looping != ANIMLOOP_BOUNCE) {
        return 1.0f <= this->m_elapsed / this->m_duration;
    }
    return false;
}

// ref: FUN_1406073b0
const char* CSimpleAnimGroup::GetLoopState() {
    return LoopState(this->m_playing, this->m_reverse);
}

// ref: FUN_140604870
bool CSimpleAnimGroup::AdvanceOrder() {
    int32_t count = static_cast<int32_t>(this->m_orders.size());
    bool flip = (this->m_groupFlags >> 6) & 1;
    bool reverse = this->m_playing ? this->m_reverse : false;
    int8_t wrap;
    int32_t end;
    int8_t step;
    if (reverse == flip) {
        wrap = 0;
        end = count;
        step = 1;
    } else {
        wrap = static_cast<int8_t>(count - 1);
        end = -1;
        step = -1;
    }
    this->m_currentOrder = static_cast<int8_t>(this->m_currentOrder + step);
    if (this->m_currentOrder != end) {
        this->PlayCurrentOrder();
        return false;
    }
    if (this->m_looping == ANIMLOOP_NONE || (this->m_groupFlags & GROUP_FINISHING)) {
        return true;
    }
    if (this->m_looping == ANIMLOOP_BOUNCE) {
        this->m_reverse = reverse == flip;
        if (!this->m_playing) {
            this->m_playing = true;
        }
        this->m_currentOrder = static_cast<int8_t>(this->m_currentOrder - step);
    } else {
        this->m_currentOrder = wrap;
    }
    this->m_elapsed = 0.0f;
    for (ANIMLIST& anims : this->m_orders) {
        for (CSimpleAnim* anim : anims) {
            // FUN_140608e40
            bool backwards = this->m_playing ? this->m_reverse : false;
            if (backwards) {
                anim->m_elapsed = anim->GetTotalTime();
                anim->m_smoothProgress = 1.0f;
                anim->m_progress = 1.0f;
            } else {
                anim->m_elapsed = 0.0f;
                anim->m_smoothProgress = 0.0f;
                anim->m_progress = 0.0f;
            }
            anim->m_state = CSimpleAnim::STATE_STOPPED;
        }
    }
    this->RunScript(ANIMSCRIPT_ONLOOP, nullptr, LoopState(this->m_playing, this->m_reverse));
    if (this->m_playing && !(this->m_groupFlags & GROUP_PAUSED)) {
        this->PlayCurrentOrder();
        return false;
    }
    return true;
}

// Orders behind the current one are held at their end (at their start when playing
// backwards); the current order's animations advance, each taking as much of the elapsed
// time as it needs, and when all of them are through the next order starts with what is
// left.
// ref: FUN_140606c60
bool CSimpleAnimGroup::Update(float elapsed) {
    if (this->m_orders.size() == 0) {
        return true;
    }
    if (this->m_playing && this->m_reverse) {
        int32_t last = static_cast<int32_t>(this->m_orders.size()) - 1;
        for (int32_t index = last; this->m_currentOrder < index; index--) {
            ANIMLIST& anims = this->m_orders[index];
            for (CSimpleAnim** it = anims.begin(), **end = anims.end(); it != end; it++) {
                (*it)->Apply(0.0f);
            }
        }
    } else {
        for (int32_t index = 0; index < this->m_currentOrder; index++) {
            ANIMLIST& anims = this->m_orders[index];
            for (CSimpleAnim** it = anims.begin(), **end = anims.end(); it != end; it++) {
                (*it)->Apply(1.0f);
            }
        }
    }
    if (elapsed < 0.0f) {
        elapsed = 0.0f;
    } else if (elapsed >= 60.0f) {
        elapsed = 60.0f;
    }
    while (true) {
        bool allDone = true;
        float used = 0.0f;
        ANIMLIST& anims = this->m_orders[this->m_currentOrder];
        for (CSimpleAnim** it = anims.begin(), **end = anims.end(); it != end; it++) {
            CSimpleAnim* anim = *it;
            bool reverse = this->IsPlayingReverse();
            float animUsed = 0.0f;
            switch (anim->m_state) {
            case CSimpleAnim::STATE_STOPPED:
                break;
            case CSimpleAnim::STATE_PAUSED:
                anim->Apply(anim->m_smoothProgress);
                allDone = false;
                break;
            case CSimpleAnim::STATE_DONE:
                anim->Apply(reverse ? 0.0f : 1.0f);
                break;
            default:
                animUsed = anim->Update(elapsed, reverse);
                anim->RunScript(ANIMSCRIPT_ONUPDATE, nullptr, elapsed);
                anim->Apply(anim->m_smoothProgress);
                if (anim->m_state != CSimpleAnim::STATE_DONE) {
                    allDone = false;
                    break;
                }
                anim->RunScript(ANIMSCRIPT_ONFINISHED, nullptr);
                if (anim->m_state != CSimpleAnim::STATE_DONE) {
                    allDone = false;
                }
                break;
            }
            if (animUsed > used) {
                used = animUsed;
            }
        }
        if (!(this->m_groupFlags & GROUP_PAUSED) && this->m_playing) {
            elapsed -= used;
            this->m_elapsed += used;
            if (allDone && this->AdvanceOrder()) {
                return true;
            }
        }
        if (elapsed <= 0.0001f || this->IsDone() || !this->m_playing || (this->m_groupFlags & GROUP_PAUSED)) {
            return false;
        }
    }
}

// ref: FUN_14060ae70
bool CSimpleAnimGroup::OnUpdate(float elapsed) {
    if (!this->m_playing) {
        return true;
    }
    bool done = this->Update(elapsed);
    this->RunScript(ANIMSCRIPT_ONUPDATE, nullptr, elapsed);
    if (done) {
        this->Finished();
    }
    return this->IsDone();
}

// ref: FUN_14060ad90
void CSimpleAnimGroup::RemoveAnimatedObjects() {
    for (CAnimatableObject** it = this->m_animated.begin(), **end = this->m_animated.end(); it != end; it++) {
        (*it)->RemovePlayingGroup(this);
    }
    this->m_animated.m_size = 0;
}

// ref: FUN_14060ab50
void CSimpleAnimGroup::AddAnimatedObjects() {
    this->RemoveAnimatedObjects();
    for (ANIMLIST& anims : this->m_orders) {
        for (CSimpleAnim* anim : anims) {
            CAnimatableObject* target = anim->GetTargetObject();
            if (target == nullptr) {
                continue;
            }
            // A lower bound by address.
            uint64_t index = 0;
            uint64_t count = this->m_animated.size();
            while (index < count) {
                uint64_t middle = index + (count - index) / 2;
                if (reinterpret_cast<uintptr_t>(this->m_animated[middle]) < reinterpret_cast<uintptr_t>(target)) {
                    index = middle + 1;
                } else {
                    count = middle;
                }
            }
            if (index == this->m_animated.size()) {
                this->m_animated.push_back(target);
            } else if (reinterpret_cast<uintptr_t>(target) < reinterpret_cast<uintptr_t>(this->m_animated[index])) {
                this->m_animated.push_back(target);
                for (uint64_t i = this->m_animated.size() - 1; i > index; i--) {
                    this->m_animated[i] = this->m_animated[i - 1];
                }
                this->m_animated[index] = target;
            }
        }
    }
    for (CAnimatableObject** it = this->m_animated.begin(), **end = this->m_animated.end(); it != end; it++) {
        (*it)->AddPlayingGroup(this);
    }
}

// ref: FUN_140607cd0
bool CSimpleAnimGroup::AnimatesType(int32_t type) {
    for (ANIMLIST& anims : this->m_orders) {
        for (CSimpleAnim* anim : anims) {
            if (anim->IsObjectType(type)) {
                return true;
            }
        }
    }
    return false;
}

// ref: FUN_140607cb0
bool CSimpleAnimGroup::AnimatesAlpha() {
    return this->AnimatesType(CSimpleAlphaAnim::GetObjectType());
}

// Anything but alpha and texture coordinates moves the target.
// ref: FUN_140607d70
bool CSimpleAnimGroup::AnimatesTransform() {
    for (ANIMLIST& anims : this->m_orders) {
        for (CSimpleAnim* anim : anims) {
            if (!anim->IsObjectType(CSimpleAlphaAnim::GetObjectType())
                && !anim->IsObjectType(CSimpleTexCoordTranslationAnim::GetObjectType())) {
                return true;
            }
        }
    }
    return false;
}

// ref: FUN_140607ee0
bool CSimpleAnimGroup::AnimatesTexCoord() {
    return this->AnimatesType(CSimpleTexCoordTranslationAnim::GetObjectType());
}

// From the order the group ends on back towards the one it starts on, the longest alpha
// animation of `target` in the first order that has one decides: its "to" alpha, or its
// "from" alpha when the group ends at the start.
// ref: FUN_1406055d0
uint8_t CSimpleAnimGroup::GetFinalAlpha(CAnimatableObject* target, uint8_t alpha) {
    int32_t count = static_cast<int32_t>(this->m_orders.size());
    if (count == 0) {
        return alpha;
    }
    bool flip = (this->m_groupFlags >> 6) & 1;
    bool reverse = this->m_playing ? this->m_reverse : false;
    int32_t step = reverse != flip ? 1 : -1;
    int32_t index = reverse == flip ? count - 1 : 0;
    int32_t end = reverse != flip ? count : -1;
    float longest = 0.0f;
    for (; index != end; index += step) {
        ANIMLIST& anims = this->m_orders[index];
        if (anims.size() == 0) {
            continue;
        }
        for (CSimpleAnim** it = anims.begin(), **last = anims.end(); it != last; it++) {
            CSimpleAnim* anim = *it;
            if (!anim->IsObjectType(CSimpleAlphaAnim::GetObjectType()) || anim->GetTargetObject() != target) {
                continue;
            }
            float total = anim->GetTotalTime();
            if (longest < total) {
                longest = total;
                bool backwards = this->m_playing ? this->m_reverse : false;
                auto alphaAnim = static_cast<CSimpleAlphaAnim*>(anim);
                alpha = backwards == flip ? alphaAnim->m_toAlpha : alphaAnim->m_fromAlpha;
            }
        }
        if (longest > 0.0f) {
            return alpha;
        }
    }
    return alpha;
}

// ref: FUN_140606580
void CSimpleAnimGroup::CreateScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();

    s_metaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleAnimGroup_FillScriptMethods, false);
    s_restrictedMetaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleAnimGroup_FillScriptMethods, true);
}

// A group read from an <AnimationGroup> element.
// ref: FUN_140667a30
CSimpleAnimGroup* FrameXML_CreateAnimGroup(XMLOBJECT* object, CScriptRegion* owner, CStatus* status) {
    static const char s_loadSource[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\FrameXML\\LoadXML.cpp";
    void* memory = SMemAlloc(0x90, 1, s_loadSource, SStrHashFnv1a(s_loadSource), 0xda, 0);
    auto group = new (memory) CSimpleAnimGroup(owner->GetAnimatableObject());
    group->BeginLoadXML(object);
    group->FrameScript_Object::LoadXML(object, status, nullptr);
    group->LoadXML(object, status);
    group->PostLoadXML(object, status);
    return group;
}
