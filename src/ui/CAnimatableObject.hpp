#ifndef UI_C_ANIMATABLE_OBJECT_HPP
#define UI_C_ANIMATABLE_OBJECT_HPP

// CAnimatableObject (Engine/Source/Frame/CAnimatableObject.cpp): the animation host every
// frame and region carries (0x10, vtable 0x1426ca358). It keeps the animation groups created
// on it and the ones playing, and tells its owner when a playing group starts or stops
// animating the translation, scale or alpha, so the owner can drop the overrides.

#include <cstdint>

#include "storm/hash/TSHashTable.hpp"

class CScriptRegion;
class CSimpleAnimGroup;

// The two group lists of an object, made on first use (0x10).
struct ANIMATABLEGROUPS {
    TSExplicitList<CSimpleAnimGroup>* m_groups;     // 0x00 every group, linked at +0x18
    TSExplicitList<CSimpleAnimGroup>* m_playing;    // 0x08 the playing ones, linked at +0x28
};

class CAnimatableObject {
    public:
    ANIMATABLEGROUPS* m_groups = nullptr;   // 0x08

    // ref: FUN_14062f3d0
    virtual ~CAnimatableObject();
    virtual void Unknown08();
    virtual void Unknown10();
    // A group started or stopped playing.
    virtual void AddPlayingGroup(CSimpleAnimGroup* group);
    virtual void RemovePlayingGroup(CSimpleAnimGroup* group);
    virtual void StopAnimating();
    // A playing group began animating these (and no other group already does): the alpha,
    // the transform (scale and rotation), the rect offset (translation).
    virtual void OnAnimStarted(bool alpha, bool transform, bool rectOffset);
    // A group that animated these stopped (and no other playing group does).
    virtual void OnAnimStopped(CSimpleAnimGroup* group, bool alpha, bool transform, bool rectOffset);
    virtual void Unknown40(void* value);
    virtual void Unknown48(void* value);
    virtual void Unknown50(void* value);
    virtual void Unknown58(int32_t index, void* value, int32_t extra);
    virtual void Unknown60(int32_t index, void* first, void* second);
    virtual void Unknown68(int32_t index, void* first, void* second);
    // An alpha animation set the object's alpha.
    virtual void SetAnimAlpha(uint8_t alpha);
    virtual void Unknown78();
    // The object whose animations play on this one too (none by default).
    virtual CAnimatableObject* GetAnimParent();
    virtual void* Unknown88();
    virtual CScriptRegion* GetScriptRegion() = 0;

    // Adds a group to the object's list of groups.
    void AddGroup(CSimpleAnimGroup* group);
    void RemoveGroup(CSimpleAnimGroup* group);
    // Whether a playing group here or on an animation parent animates each kind.
    bool IsAnimatingAlpha();
    bool IsAnimatingTransform();
    bool IsAnimatingRectOffset();
    bool IsPlaying();
};
static_assert(sizeof(CAnimatableObject) == 0x10, "CAnimatableObject is 0x10 bytes");

// What the animation groups report about themselves (CSimpleAnimGroup, not ported yet).
bool CSimpleAnimGroup_AnimatesAlpha(CSimpleAnimGroup* group);
bool CSimpleAnimGroup_AnimatesTransform(CSimpleAnimGroup* group);
bool CSimpleAnimGroup_AnimatesRectOffset(CSimpleAnimGroup* group);
void CSimpleAnimGroup_Stop(CSimpleAnimGroup* group, bool reset);
// The group's flags (+0x80): 8 ..., 0x10 setToFinalAlpha.
uint8_t CSimpleAnimGroup_GetFlags(CSimpleAnimGroup* group);
// The alpha the object ends at when the group stops, from the one it has.
uint8_t CSimpleAnimGroup_GetFinalAlpha(CSimpleAnimGroup* group, CAnimatableObject* object, uint8_t alpha);
void CSimpleAnimGroup_Delete(CSimpleAnimGroup* group);

#endif
