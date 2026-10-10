#include "ui/CAnimatableObject.hpp"

#include <new>

#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "ui/CSimpleTop.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "ui/CSimpleAnim.hpp"
#include "ui/FrameXML.hpp"

extern const char s_animGroupVectorTag[] = ".PEAVCSimpleAnimGroup@@";

namespace {

const char s_source[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Frame\\CAnimatableObject.cpp";
const char s_groupListTag[] = ".?AV?$TSList@VCSimpleAnimGroup@@V?$TSGetExplicitLink@VCSimpleAnimGroup@@@@$07@@";

// Deletes each group of a list (each one leaves every list it is in as it goes).
// ref: FUN_14062f4e0
void DeleteGroups(TSExplicitList<CSimpleAnimGroup>* list) {
    while (CSimpleAnimGroup* group = list->Head()) {
        CSimpleAnimGroup_Delete(group);
    }
}

// TSExplicitList's destructor (FUN_1402f9a70) and the list's own memory.
void FreeGroupList(TSExplicitList<CSimpleAnimGroup>* list) {
    list->UnlinkAll();
    list->m_terminator.Unlink();
    SMemFree(list, s_groupListTag, -2, 0);
}

TSExplicitList<CSimpleAnimGroup>* NewGroupList(int32_t linkoffset, int32_t line) {
    auto list = static_cast<TSExplicitList<CSimpleAnimGroup>*>(
        SMemAlloc(sizeof(TSExplicitList<CSimpleAnimGroup>), 0x1f, s_source, SStrHashFnv1a(s_source), line, 0));
    list->m_terminator.m_next = nullptr;
    list->InitEmpty(linkoffset);
    return list;
}

ANIMATABLEGROUPS* GetGroups(CAnimatableObject* object, int32_t line) {
    if (object->m_groups == nullptr) {
        auto groups = static_cast<ANIMATABLEGROUPS*>(
            SMemAlloc(sizeof(ANIMATABLEGROUPS), 0x1f, s_source, SStrHashFnv1a(s_source), line, 0));
        groups->m_groups = nullptr;
        groups->m_playing = nullptr;
        object->m_groups = groups;
    }
    return object->m_groups;
}

// Whether a group in the playing lists of the object or its animation parents passes `test`.
template <class Test>
bool AnyPlaying(CAnimatableObject* object, Test test) {
    for (; object != nullptr; object = object->GetAnimParent()) {
        if (object->m_groups == nullptr || object->m_groups->m_playing == nullptr) {
            continue;
        }
        TSExplicitList<CSimpleAnimGroup>* playing = object->m_groups->m_playing;
        for (CSimpleAnimGroup* group = playing->Head(); group != nullptr; group = playing->Link(group)->Next()) {
            if (test(group)) {
                return true;
            }
        }
    }
    return false;
}

}

// ref: FUN_14062f3d0
CAnimatableObject::~CAnimatableObject() {
    ANIMATABLEGROUPS* groups = this->m_groups;
    if (groups == nullptr) {
        return;
    }
    if (groups->m_playing != nullptr) {
        DeleteGroups(groups->m_playing);
        FreeGroupList(groups->m_playing);
    }
    if (groups->m_groups != nullptr) {
        DeleteGroups(groups->m_groups);
        FreeGroupList(groups->m_groups);
    }
    SMemFree(groups, ".?AUANIMATABLEGROUPS@@", -2, 0);
}

void CAnimatableObject::Unknown08() {
}

void CAnimatableObject::Unknown10() {
}

// A group started: the owner learns which kinds it newly animates, then the group joins the
// playing list and the object the UI's animating set.
// ref: FUN_14062fca0
void CAnimatableObject::AddPlayingGroup(CSimpleAnimGroup* group) {
    ANIMATABLEGROUPS* groups = GetGroups(this, 0x8a);
    if (groups->m_playing == nullptr) {
        groups->m_playing = NewGroupList(0x28, 0x8e);
    }
    bool alpha = CSimpleAnimGroup_AnimatesAlpha(group) && !this->IsAnimatingAlpha();
    bool transform = CSimpleAnimGroup_AnimatesTransform(group) && !this->IsAnimatingTransform();
    bool rectOffset = CSimpleAnimGroup_AnimatesRectOffset(group) && !this->IsAnimatingRectOffset();
    this->OnAnimStarted(alpha, transform, rectOffset);
    groups->m_playing->LinkToTail(group);
    // FUN_1405ed9f0
    CSimpleTop::s_instance->AddAnimating(this);
}

// ref: FUN_14062feb0
void CAnimatableObject::RemovePlayingGroup(CSimpleAnimGroup* group) {
    if (this->m_groups == nullptr || this->m_groups->m_playing == nullptr) {
        return;
    }
    TSExplicitList<CSimpleAnimGroup>* playing = this->m_groups->m_playing;
    playing->Link(group)->Unlink();
    bool alpha = CSimpleAnimGroup_AnimatesAlpha(group) && !this->IsAnimatingAlpha();
    bool transform = CSimpleAnimGroup_AnimatesTransform(group) && !this->IsAnimatingTransform();
    bool rectOffset = CSimpleAnimGroup_AnimatesRectOffset(group) && !this->IsAnimatingRectOffset();
    this->OnAnimStopped(group, alpha, transform, rectOffset);
    if (this->m_groups->m_playing->Head() == nullptr) {
        // FUN_1405f3120
        CSimpleTop::s_instance->RemoveAnimating(this);
    }
}

// ref: FUN_1406302d0
void CAnimatableObject::StopAnimating() {
    if (this->m_groups == nullptr || this->m_groups->m_playing == nullptr) {
        return;
    }
    TSExplicitList<CSimpleAnimGroup>* playing = this->m_groups->m_playing;
    for (CSimpleAnimGroup* group = playing->Head(); group != nullptr;) {
        CSimpleAnimGroup* next = playing->Link(group)->Next();
        CSimpleAnimGroup_Stop(group, true);
        group = next;
    }
}

void CAnimatableObject::OnAnimStarted(bool alpha, bool transform, bool rectOffset) {
    (void)alpha;
    (void)transform;
    (void)rectOffset;
}

void CAnimatableObject::OnAnimStopped(CSimpleAnimGroup* group, bool alpha, bool transform, bool rectOffset) {
    (void)group;
    (void)alpha;
    (void)transform;
    (void)rectOffset;
}

void CAnimatableObject::Unknown40(void* value) {
    (void)value;
}

void CAnimatableObject::Unknown48(void* value) {
    (void)value;
}

void CAnimatableObject::Unknown50(void* value) {
    (void)value;
}

void CAnimatableObject::Unknown58(int32_t index, void* value, float radians) {
    (void)index;
    (void)value;
    (void)radians;
}

void CAnimatableObject::Unknown60(int32_t index, void* first, void* second) {
    (void)index;
    (void)first;
    (void)second;
}

void CAnimatableObject::Unknown68(int32_t index, void* first, void* second) {
    (void)index;
    (void)first;
    (void)second;
}

void CAnimatableObject::SetAnimAlpha(uint8_t alpha) {
    (void)alpha;
}

void CAnimatableObject::Unknown78(float progress, uint32_t rows, uint32_t columns, uint32_t frames, uint32_t frameWidth,
    uint32_t frameHeight) {
    (void)progress;
    (void)rows;
    (void)columns;
    (void)frames;
    (void)frameWidth;
    (void)frameHeight;
}

CAnimatableObject* CAnimatableObject::GetAnimParent() {
    return nullptr;
}

void* CAnimatableObject::Unknown88() {
    return nullptr;
}

// ref: FUN_140630140
void CAnimatableObject::AddGroup(CSimpleAnimGroup* group) {
    ANIMATABLEGROUPS* groups = GetGroups(this, 0x2c);
    if (groups->m_groups == nullptr) {
        groups->m_groups = NewGroupList(0x18, 0x30);
    }
    groups->m_groups->LinkToTail(group);
}

// ref: FUN_140630340
void CAnimatableObject::RemoveGroup(CSimpleAnimGroup* group) {
    if (this->m_groups != nullptr && this->m_groups->m_groups != nullptr) {
        this->m_groups->m_groups->Link(group)->Unlink();
    }
}

// ref: FUN_14062f9d0
bool CAnimatableObject::IsAnimatingAlpha() {
    return AnyPlaying(this, CSimpleAnimGroup_AnimatesAlpha);
}

// ref: FUN_14062fac0
bool CAnimatableObject::IsAnimatingTransform() {
    return AnyPlaying(this, CSimpleAnimGroup_AnimatesTransform);
}

// ref: FUN_14062fb60
bool CAnimatableObject::IsAnimatingRectOffset() {
    return AnyPlaying(this, CSimpleAnimGroup_AnimatesRectOffset);
}

// ref: FUN_14062fa70
bool CAnimatableObject::IsPlaying() {
    for (CAnimatableObject* object = this; object != nullptr; object = object->GetAnimParent()) {
        if (object->m_groups != nullptr && object->m_groups->m_playing != nullptr
            && object->m_groups->m_playing->Head() != nullptr) {
            return true;
        }
    }
    return false;
}

// A group made by the CreateAnimationGroup script method, from the template `inherits`
// when given; nullptr when the template is missing or already being loaded.
// ref: FUN_14062f540
FrameScript_Object* CAnimatableObject::CreateAnimationGroup(const char* name, const char* inherits) {
    char inheritsName[0x80] = {};
    XMLOBJECT* inherited = nullptr;
    const char* taint = nullptr;
    bool busy = false;
    if (inherits != nullptr) {
        busy = true;
        uint32_t i = 0;
        for (; i < 0x7f && inherits[i] != '\0'; i++) {
            inheritsName[i] = inherits[i];
        }
        inheritsName[i] = '\0';
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
    void* memory = SMemAlloc(0x90, 0x1f, s_source, SStrHashFnv1a(s_source), 0xb5, 0);
    auto group = new (memory) CSimpleAnimGroup(this);
    if (name != nullptr && *name != '\0') {
        group->SetName(name);
    }
    if (inherited != nullptr) {
        CScriptStatus status;
        status.m_warn = true;
        group->LoadXML(inherited, &status);
        group->PostLoadXML(inherited, &status);
    }
    if (inherited != nullptr && !busy) {
        FrameXML_ReleaseVirtual(inheritsName);
    }
    return group;
}

// The groups scripts may see: accessible and not hidden, in creation order.
// ref: FUN_14062f770
ANIMGROUPLIST* CAnimatableObject::GetScriptAnimationGroups(ANIMGROUPLIST* groups) {
    if (this->m_groups == nullptr || this->m_groups->m_groups == nullptr) {
        return groups;
    }
    TSExplicitList<CSimpleAnimGroup>* list = this->m_groups->m_groups;
    for (CSimpleAnimGroup* group = list->Head(); group != nullptr; group = list->Link(group)->Next()) {
        if ((group->m_flags & FSOBJ_ACCESSIBLE) && !(group->m_flags & FSOBJ_HIDDEN)) {
            groups->push_back(group);
        }
    }
    return groups;
}

// ---- what the groups report (CSimpleAnimGroup) -------------------------------------------

// ref: FUN_140607cb0
bool CSimpleAnimGroup_AnimatesAlpha(CSimpleAnimGroup* group) {
    return group->AnimatesAlpha();
}

// ref: FUN_140607d70
bool CSimpleAnimGroup_AnimatesTransform(CSimpleAnimGroup* group) {
    return group->AnimatesTransform();
}

// The third kind is the texture coordinates (TextureCoordTranslation animations).
// ref: FUN_140607ee0
bool CSimpleAnimGroup_AnimatesRectOffset(CSimpleAnimGroup* group) {
    return group->AnimatesTexCoord();
}

// ref: FUN_14060c230
void CSimpleAnimGroup_Stop(CSimpleAnimGroup* group, bool reset) {
    group->Stop(reset);
}

uint8_t CSimpleAnimGroup_GetFlags(CSimpleAnimGroup* group) {
    return group->m_groupFlags;
}

// ref: FUN_1406055d0
uint8_t CSimpleAnimGroup_GetFinalAlpha(CSimpleAnimGroup* group, CAnimatableObject* object, uint8_t alpha) {
    return group->GetFinalAlpha(object, alpha);
}

void CSimpleAnimGroup_Delete(CSimpleAnimGroup* group) {
    delete group;
}

// ref: FUN_14060ae70
void CSimpleAnimGroup_OnUpdate(CSimpleAnimGroup* group, float elapsed) {
    group->OnUpdate(elapsed);
}
