#ifndef UI_C_SIMPLE_ANIM_HPP
#define UI_C_SIMPLE_ANIM_HPP

// The UI animation system (Engine/Source/Frame/CSimpleAnim.cpp, CSimpleAnimGroup.cpp): an
// animation group (<AnimationGroup>) belongs to an animatable object and holds animations
// in numbered orders; the orders play one after another, the animations of one order
// together. Each animation drives one target (its group's owner unless told otherwise)
// through the target's animation hooks: alpha, translation, rotation, scale, texture
// coordinates, flipbook frames, or a path of control points.

#include <cstdint>

#include "blz/Vector.hpp"
#include "storm/hash/TSHashTable.hpp"
#include "storm/list/TSLink.hpp"
#include "ui/CScriptObject.hpp"

class CAnimatableObject;
class CSimpleAnim;
class CSimpleAnimGroup;
class CSimpleAnimPath;
class CSimpleControlPoint;
class CStatus;
struct XMLOBJECT;
struct lua_State;

extern const char s_animTag[];
extern const char s_animOrderTag[];
extern const char s_animatableTag[];
extern const char s_controlPointTag[];
typedef blz::vector<CSimpleAnim*, s_animTag> ANIMLIST;
typedef blz::vector<CSimpleControlPoint*, s_controlPointTag> CONTROLPOINTLIST;
typedef blz::vector<ANIMLIST, s_animOrderTag> ANIMORDERLIST;
typedef blz::vector<CAnimatableObject*, s_animatableTag> ANIMATABLELIST;

// The script handlers animations and groups run.
enum : uint8_t {
    ANIMSCRIPT_ONLOAD = 4,
    ANIMSCRIPT_ONFINISHED = 5,
    ANIMSCRIPT_ONPLAY = 6,
    ANIMSCRIPT_ONPAUSE = 7,
    ANIMSCRIPT_ONSTOP = 8,
    ANIMSCRIPT_ONUPDATE = 10,
    ANIMSCRIPT_ONLOOP = 11,
};

// The looping kinds ("NONE", "REPEAT", "BOUNCE") and path curves ("NONE", "SMOOTH").
enum : uint8_t {
    ANIMLOOP_NONE = 0,
    ANIMLOOP_REPEAT = 1,
    ANIMLOOP_BOUNCE = 2,
};
enum : uint8_t {
    ANIMCURVE_NONE = 0,
    ANIMCURVE_SMOOTH = 1,
};

// Name lookups for the XML and the script methods (false or "UNKNOWN" when not found).
// ref: FUN_140667690
bool StringToAnimLoopType(const char* name, uint8_t* type);
// ref: FUN_140667660
const char* AnimLoopTypeToString(uint8_t type);
// ref: FUN_1406675f0
bool StringToAnimCurveType(const char* name, uint8_t* type);
// ref: FUN_1406675c0
const char* AnimCurveTypeToString(uint8_t type);
// The in and out weights of a smoothing name ("NONE", "IN", "OUT", "IN_OUT", "OUT_IN").
// ref: FUN_140667770
bool StringToAnimSmoothing(const char* name, float* weights);
// ref: FUN_140667700
const char* AnimSmoothingToString(const float* weights);

// The easing an animation's progress goes through (0x10, vtable 0x1426c92e0).
class CSimpleAnimSmoothing {
    public:
    float m_in = 0.0f;              // 0x08
    float m_out = 0.0f;             // 0x0C

    virtual ~CSimpleAnimSmoothing() = default;
    // Each weight clamped to [0, 1].
    // ref: FUN_14060c1c0
    virtual void SetWeights(float in, float out);
    // ref: FUN_140607a40
    virtual float Apply(float progress);
};

// An animation group (0x90, vtable 0x1426c9198).
class CSimpleAnimGroup : public CScriptObject {
    public:
    // m_flags2 (+0x80).
    enum : uint8_t {
        GROUP_LOADING = 0x1,
        GROUP_FINISHING = 0x2,      // Finish() was asked: stop at the end of this pass
        GROUP_PAUSED = 0x4,
        GROUP_STOPPING = 0x8,
        GROUP_SETTOFINALALPHA = 0x10,
        GROUP_STARTING = 0x20,      // inside Play(): stop and pause leave the animations be
        GROUP_FLIPPED = 0x40,       // reversed while playing
    };

    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;

    TSLink<CSimpleAnimGroup> m_groupLink;   // 0x18 the owner's list of groups
    TSLink<CSimpleAnimGroup> m_playingLink; // 0x28 the owner's list of playing groups
    CAnimatableObject* m_owner = nullptr;   // 0x38
    ANIMORDERLIST m_orders;                 // 0x40 one list of animations per order, ascending
    ANIMATABLELIST m_animated;              // 0x58 the targets told the group is playing, sorted
    uint64_t m_pad70 = 0;
    uint8_t m_looping = ANIMLOOP_NONE;      // 0x78
    bool m_reverse = false;                 // 0x79
    bool m_playing = false;                 // 0x7A
    uint8_t m_pad7b = 0;
    int8_t m_currentOrder = 0;              // 0x7C index into m_orders
    uint8_t m_pad7d[3] = {};
    uint8_t m_groupFlags = 0;               // 0x80
    uint8_t m_pad81[3] = {};
    float m_speed = 1.0f;                   // 0x84
    float m_elapsed = 0.0f;                 // 0x88
    float m_duration = 0.0f;                // 0x8C the orders' longest animations, summed

    static int32_t GetObjectType();
    // ref: FUN_140606580
    static void CreateScriptMetaTables();

    // ref: FUN_140603b90
    explicit CSimpleAnimGroup(CAnimatableObject* owner);
    // ref: FUN_140604010
    ~CSimpleAnimGroup() override;
    // ref: FUN_140607820
    int32_t GetScriptMetaTable() override;
    // ref: FUN_14060c3b0
    bool GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) override;
    // ref: FUN_14060ae00
    void OnProtected() override;
    // ref: FUN_140608220
    bool IsObjectTypeName(const char* name) override;
    // ref: FUN_140608180
    bool IsObjectType(int32_t type) override;
    // ref: FUN_1406075f0
    const char* GetObjectTypeName() override;
    // ref: FUN_140607420
    uint32_t GetObjectSize() override;
    // ref: FUN_140607a10
    CScriptObject* GetParent() override;

    // XML: marks the group loading and names it, before FrameScript_Object::LoadXML.
    // ref: FUN_14060b500
    void BeginLoadXML(XMLOBJECT* node);
    // The inherited templates, then the group's own settings and its animations.
    // ref: FUN_140609520
    void LoadXML(XMLOBJECT* node, CStatus* status);
    // Runs OnLoad for each animation and the group, once the whole element is read.
    // ref: FUN_14060b3a0
    void PostLoadXML(XMLOBJECT* node, CStatus* status);

    // Membership: an animation joins the list of its order.
    // ref: FUN_140604670
    void AddAnim(CSimpleAnim* anim);
    // ref: FUN_14060b6d0
    void RemoveAnim(CSimpleAnim* anim);
    // ref: FUN_140605890
    void SetAnimOrder(CSimpleAnim* anim, int32_t order);
    // ref: FUN_140605900
    void UpdateDuration();
    // ref: FUN_1406059f0
    CSimpleAnim* CreateAnimation(const char* type, const char* name, const char* inherits);
    // ref: FUN_14060b650
    void RemoveAnimations();
    // The accessible animations, in order.
    // ref: FUN_140607130
    ANIMLIST* GetScriptAnimations(ANIMLIST* anims);

    // Playback.
    // ref: FUN_14060b0b0
    bool Play(bool reverse, float offset);
    // ref: FUN_14060afb0
    void Pause();
    // ref: FUN_14060c230
    void Stop(bool requested);
    // ref: FUN_14060b8e0
    void Restart(bool reverse, float offset);
    // ref: FUN_14060bda0
    void SetPlaying(bool play);
    // Asks the group to stop at the end of the current pass.
    // ref: FUN_1406070e0
    void Finish();
    // Advances by `elapsed` seconds; true when the group finished.
    // ref: FUN_140606c60
    bool Update(float elapsed);
    // The frame update CSimpleTop runs for each playing group; true once it is done.
    // ref: FUN_14060ae70
    bool OnUpdate(float elapsed);
    // ref: FUN_140608e00
    bool IsDone();
    // Whether the animations run backwards now (the play direction against a flip).
    // ref: FUN_140608d20
    bool IsPlayingReverse();
    // ref: FUN_1406073b0
    const char* GetLoopState();

    // What the group's animations change.
    // ref: FUN_140607cb0
    bool AnimatesAlpha();
    // ref: FUN_140607d70
    bool AnimatesTransform();
    // ref: FUN_140607ee0
    bool AnimatesTexCoord();
    // The alpha the object `target` ends at when the group stops, `alpha` if no animation
    // of the group sets it.
    // ref: FUN_1406055d0
    uint8_t GetFinalAlpha(CAnimatableObject* target, uint8_t alpha);

    private:
    // ref: FUN_140607cd0
    bool AnimatesType(int32_t type);
    // The next order; true when the group is through all of them (and not looping).
    // ref: FUN_140604870
    bool AdvanceOrder();
    // Starts the animations of the current order.
    void PlayCurrentOrder();
    // ref: FUN_14060ab50
    void AddAnimatedObjects();
    // ref: FUN_14060ad90
    void RemoveAnimatedObjects();
    // OnFinished after the last order or a Finish().
    void Finished();
};
static_assert(sizeof(CSimpleAnimGroup) == 0x90, "CSimpleAnimGroup is 0x90 bytes");

// An animation (0x90, vtable 0x1426c88d8): the base class, which animates nothing.
class CSimpleAnim : public CScriptObject {
    public:
    // m_animFlags (+0x89).
    enum : uint8_t {
        ANIM_LOADING = 0x1,
        ANIM_SCALEFROMTO = 0x4,     // scale from one scale to another, not by one
    };
    // How the target is found (+0x8a).
    enum : uint8_t {
        TARGET_OWNER = 0,           // the group's owner
        TARGET_NAME = 1,            // a global name
        TARGET_KEY = 2,             // a path from the animation ("$parent.$parent.Child")
        TARGET_CHILDKEY = 3,        // a path from the group's owner
        TARGET_OBJECT = 4,          // an object given directly
    };
    // m_state (+0x8b).
    enum : uint8_t {
        STATE_STOPPED = 0,
        STATE_PLAYING = 1,
        STATE_PAUSED = 2,
        STATE_DONE = 3,
    };

    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;

    float m_startDelay = 0.0f;              // 0x18
    float m_endDelay = 0.0f;                // 0x1C
    float m_duration = 0.0f;                // 0x20
    float m_elapsed = 0.0f;                 // 0x24 delays included
    float m_progress = 0.0f;                // 0x28
    float m_smoothProgress = 0.0f;          // 0x2C
    CSimpleAnimGroup* m_group = nullptr;    // 0x30
    CAnimatableObject* m_target = nullptr;  // 0x38
    char m_targetName[64] = {};             // 0x40
    CSimpleAnimSmoothing* m_smoothing = nullptr;    // 0x80
    int8_t m_order = 0;                     // 0x88
    uint8_t m_animFlags = 0;                // 0x89
    uint8_t m_targetKind = TARGET_OWNER;    // 0x8A
    uint8_t m_state = STATE_STOPPED;        // 0x8B
    uint32_t m_pad8c = 0;

    static int32_t GetObjectType();
    // ref: FUN_140606530
    static void CreateScriptMetaTables();

    // ref: FUN_140603b30
    explicit CSimpleAnim(CSimpleAnimGroup* group);
    // ref: FUN_140604280
    ~CSimpleAnim() override;
    // ref: FUN_1406077f0
    int32_t GetScriptMetaTable() override;
    // ref: FUN_14060c350
    bool GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) override;
    // ref: FUN_140608120
    bool IsObjectTypeName(const char* name) override;
    // ref: FUN_140608080
    bool IsObjectType(int32_t type) override;
    // ref: FUN_1406075e0
    const char* GetObjectTypeName() override;
    // ref: FUN_140607420
    uint32_t GetObjectSize() override;
    // ref: FUN_140607a00
    CScriptObject* GetParent() override;
    // The animation's own XML (inherited templates first).
    // ref: FUN_140608fc0
    virtual void LoadXML(XMLOBJECT* node, CStatus* status);
    // ref: FUN_14060b380
    virtual void PostLoadXML(XMLOBJECT* node, CStatus* status);
    // Puts the target where `progress` (0..1, after smoothing) says.
    virtual void Apply(float progress);
    // The object a child key names, from the group's owner.
    // ref: FUN_1406076b0
    virtual CScriptObject* GetChildKeyObject(const char* key);

    // The total of the delays and the duration.
    float GetTotalTime() const {
        return this->m_startDelay + this->m_endDelay + this->m_duration;
    }
    // ref: FUN_140607b30
    CAnimatableObject* GetTargetObject();
    // The target's script region.
    // ref: FUN_140607c60
    CScriptObject* GetTarget();
    // The group's owner's script region.
    // ref: FUN_140607690
    CScriptObject* GetRegionParent();
    // Advances by `elapsed` seconds in the given direction; the time used.
    // ref: FUN_140606f60
    float Update(float elapsed, bool reverse);
    // ref: FUN_14060b070
    bool Play();
    // ref: FUN_14060af70
    void Pause();
    // ref: FUN_14060c200
    void Stop(bool requested);
    // ref: FUN_140608dc0
    bool IsDelaying();
    // Delaying in the direction the group plays now (nothing to draw yet).
    // ref: FUN_140608d50
    bool IsDelayingNow();
    // ref: FUN_14060bfa0
    void SetStartDelay(float delay, bool recompute);
    // ref: FUN_14060b9b0
    void SetEndDelay(float delay, bool recompute);
    // ref: FUN_14060b980
    void SetDuration(float duration, bool recompute);
    // ref: FUN_14060bae0
    void SetOrder(int32_t order);
    // ref: FUN_14060bb00
    void SetParentGroup(CSimpleAnimGroup* group, int32_t order);
    // ref: FUN_14060bec0
    void SetSmoothing(const float* weights);
    // ref: FUN_140607af0
    void GetSmoothing(float* weights);
    // ref: FUN_14060beb0
    void SetSmoothProgress(float progress);
    // ref: FUN_14060bfd0
    bool SetTarget(CScriptObject* target);
    // ref: FUN_14060c040
    bool SetTargetKey(const char* key);
    // ref: FUN_14060b920
    bool SetChildKey(const char* key);
    // ref: FUN_14060c090
    bool SetTargetName(const char* name);
    // ref: FUN_14060c170
    bool SetTargetParent();

    protected:
    // Reads the target name kinds into m_targetName (FUN_140608fc0's middle).
    void CopyTargetName(const char* name, uint8_t kind);
};
static_assert(sizeof(CSimpleAnim) == 0x90, "CSimpleAnim is 0x90 bytes");

// Alpha (0x98, vtable 0x1426c9000).
class CSimpleAlphaAnim : public CSimpleAnim {
    public:
    uint8_t m_fromAlpha = 0;    // 0x90
    uint8_t m_toAlpha = 0;      // 0x91
    uint8_t m_pad92[6] = {};

    static int32_t GetObjectType();
    // ref: FUN_1406064e0
    static void CreateScriptMetaTables();
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;

    // ref: FUN_140603ad0
    explicit CSimpleAlphaAnim(CSimpleAnimGroup* group);
    // ref: FUN_1406077c0
    int32_t GetScriptMetaTable() override;
    // ref: FUN_140608010
    bool IsObjectTypeName(const char* name) override;
    // ref: FUN_140607fd0
    bool IsObjectType(int32_t type) override;
    // ref: FUN_1406075d0
    const char* GetObjectTypeName() override;
    // ref: FUN_140607410
    uint32_t GetObjectSize() override;
    // ref: FUN_140608e50
    void LoadXML(XMLOBJECT* node, CStatus* status) override;
    // ref: FUN_140604ab0
    void Apply(float progress) override;
    // ref: FUN_14060b9e0
    void SetFromAlpha(float alpha);
    // ref: FUN_14060c190
    void SetToAlpha(float alpha);
    // ref: FUN_140607390
    float GetFromAlpha();
    // ref: FUN_140607c90
    float GetToAlpha();
};
static_assert(sizeof(CSimpleAlphaAnim) == 0x98, "CSimpleAlphaAnim is 0x98 bytes");

// Translation (0x98, vtable 0x1426c89a8): an offset in layout units.
class CSimpleTranslationAnim : public CSimpleAnim {
    public:
    float m_offset[2] = {};     // 0x90

    static int32_t GetObjectType();
    // ref: FUN_140606850
    static void CreateScriptMetaTables();
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;

    // ref: FUN_140603f20
    explicit CSimpleTranslationAnim(CSimpleAnimGroup* group);
    // ref: FUN_1406079d0
    int32_t GetScriptMetaTable() override;
    // ref: FUN_140608c10
    bool IsObjectTypeName(const char* name) override;
    // ref: FUN_140608b70
    bool IsObjectType(int32_t type) override;
    // ref: FUN_140607680
    const char* GetObjectTypeName() override;
    // ref: FUN_140607410
    uint32_t GetObjectSize() override;
    // ref: FUN_14060a8c0
    void LoadXML(XMLOBJECT* node, CStatus* status) override;
    // ref: FUN_140605220
    void Apply(float progress) override;
    // ref: FUN_14060ba40
    void SetOffset(const float* offset);
};
static_assert(sizeof(CSimpleTranslationAnim) == 0x98, "CSimpleTranslationAnim is 0x98 bytes");

// LineTranslation (0x98, vtable 0x1426c8a78): a translation of a line's end points.
class CSimpleLineTranslationAnim : public CSimpleTranslationAnim {
    public:
    static int32_t GetObjectType();
    // ref: FUN_1406066c0
    static void CreateScriptMetaTables();
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;

    // ref: FUN_140603cd0
    explicit CSimpleLineTranslationAnim(CSimpleAnimGroup* group);
    // ref: FUN_1406078e0
    int32_t GetScriptMetaTable() override;
    // ref: FUN_140608710
    bool IsObjectTypeName(const char* name) override;
    // ref: FUN_140608610
    bool IsObjectType(int32_t type) override;
    // ref: FUN_140607630
    const char* GetObjectTypeName() override;
    // ref: FUN_140604c50
    void Apply(float progress) override;
};

// TextureCoordTranslation (0x98, vtable 0x1426c8b48): an offset of the texture coordinates.
class CSimpleTexCoordTranslationAnim : public CSimpleAnim {
    public:
    float m_offset[2] = {};     // 0x90

    static int32_t GetObjectType();
    // ref: FUN_140606800
    static void CreateScriptMetaTables();
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;

    // ref: FUN_140603ec0
    explicit CSimpleTexCoordTranslationAnim(CSimpleAnimGroup* group);
    // ref: FUN_1406079a0
    int32_t GetScriptMetaTable() override;
    // ref: FUN_140608b00
    bool IsObjectTypeName(const char* name) override;
    // ref: FUN_140608ac0
    bool IsObjectType(int32_t type) override;
    // ref: FUN_140607670
    const char* GetObjectTypeName() override;
    // ref: FUN_140607410
    uint32_t GetObjectSize() override;
    // ref: FUN_14060a860
    void LoadXML(XMLOBJECT* node, CStatus* status) override;
    // ref: FUN_1406051e0
    void Apply(float progress) override;
};
static_assert(sizeof(CSimpleTexCoordTranslationAnim) == 0x98, "CSimpleTexCoordTranslationAnim is 0x98 bytes");

// Rotation (0xa0, vtable 0x1426c8c20): an angle about an origin point.
class CSimpleRotationAnim : public CSimpleAnim {
    public:
    uint32_t m_originPoint = 4; // 0x90 a FRAMEPOINT (CENTER)
    float m_origin[2] = {};     // 0x94
    float m_radians = 0.0f;     // 0x9C

    static int32_t GetObjectType();
    // ref: FUN_140606760
    static void CreateScriptMetaTables();
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;

    // ref: FUN_140603dd0
    explicit CSimpleRotationAnim(CSimpleAnimGroup* group);
    // ref: FUN_140607940
    int32_t GetScriptMetaTable() override;
    // ref: FUN_140608940
    bool IsObjectTypeName(const char* name) override;
    // ref: FUN_1406088a0
    bool IsObjectType(int32_t type) override;
    // ref: FUN_140607650
    const char* GetObjectTypeName() override;
    // ref: FUN_140607460
    uint32_t GetObjectSize() override;
    // ref: FUN_14060a2f0
    void LoadXML(XMLOBJECT* node, CStatus* status) override;
    // ref: FUN_140605100
    void Apply(float progress) override;
};
static_assert(sizeof(CSimpleRotationAnim) == 0xa0, "CSimpleRotationAnim is 0xa0 bytes");

// Scale (0xb0, vtable 0x1426c8cf0). The scales are kept as 1 - scale.
class CSimpleScaleAnim : public CSimpleAnim {
    public:
    uint32_t m_originPoint = 4; // 0x90
    float m_origin[2] = {};     // 0x94
    float m_scale[2] = {};      // 0x9C the "by" scale, or the "to" scale with ANIM_SCALEFROMTO
    float m_fromScale[2] = {};  // 0xA4
    uint32_t m_padAc = 0;

    static int32_t GetObjectType();
    // ref: FUN_1406067b0
    static void CreateScriptMetaTables();
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;

    // ref: FUN_140603e40
    explicit CSimpleScaleAnim(CSimpleAnimGroup* group);
    // ref: FUN_140607970
    int32_t GetScriptMetaTable() override;
    // ref: FUN_140608a50
    bool IsObjectTypeName(const char* name) override;
    // ref: FUN_1406089b0
    bool IsObjectType(int32_t type) override;
    // ref: FUN_140607660
    const char* GetObjectTypeName() override;
    // ref: FUN_140607440
    uint32_t GetObjectSize() override;
    // ref: FUN_14060a3f0
    void LoadXML(XMLOBJECT* node, CStatus* status) override;
    // ref: FUN_140605130
    void Apply(float progress) override;
    // ref: FUN_14060bdc0
    void SetScale(const float* scale);
    // ref: FUN_14060be10
    void SetScaleFrom(const float* scale);
    // ref: FUN_14060be60
    void SetScaleTo(const float* scale);
    // ref: FUN_140607700
    void GetScale(float* scale);
    // ref: FUN_140607740
    void GetScaleFrom(float* scale);
    // ref: FUN_140607780
    void GetScaleTo(float* scale);
};
static_assert(sizeof(CSimpleScaleAnim) == 0xb0, "CSimpleScaleAnim is 0xb0 bytes");

// LineScale (0xb0, vtable 0x1426c8db8): a scale of a line's end points.
class CSimpleLineScaleAnim : public CSimpleScaleAnim {
    public:
    static int32_t GetObjectType();
    // ref: FUN_140606670
    static void CreateScriptMetaTables();
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;

    explicit CSimpleLineScaleAnim(CSimpleAnimGroup* group);
    // ref: FUN_1406078b0
    int32_t GetScriptMetaTable() override;
    // ref: FUN_140608590
    bool IsObjectTypeName(const char* name) override;
    // ref: FUN_140608490
    bool IsObjectType(int32_t type) override;
    // ref: FUN_140607620
    const char* GetObjectTypeName() override;
    // ref: FUN_140604ba0
    void Apply(float progress) override;
};

// FlipBook (0xa8, vtable 0x1426c90c8): frames of a texture laid out in rows and columns.
class CSimpleFlipBookAnim : public CSimpleAnim {
    public:
    uint32_t m_rows = 0;        // 0x90
    uint32_t m_columns = 0;     // 0x94
    uint32_t m_frames = 0;      // 0x98
    uint32_t m_frameWidth = 0;  // 0x9C
    uint32_t m_frameHeight = 0; // 0xA0
    uint32_t m_padA4 = 0;

    static int32_t GetObjectType();
    // ref: FUN_140606620
    static void CreateScriptMetaTables();
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;

    // ref: FUN_140603c60
    explicit CSimpleFlipBookAnim(CSimpleAnimGroup* group);
    // ref: FUN_140607880
    int32_t GetScriptMetaTable() override;
    // ref: FUN_140608420
    bool IsObjectTypeName(const char* name) override;
    // ref: FUN_140608380
    bool IsObjectType(int32_t type) override;
    // ref: FUN_140607610
    const char* GetObjectTypeName() override;
    // ref: FUN_140607430
    uint32_t GetObjectSize() override;
    // ref: FUN_140609fb0
    void LoadXML(XMLOBJECT* node, CStatus* status) override;
    // ref: FUN_140604b30
    void Apply(float progress) override;
};
static_assert(sizeof(CSimpleFlipBookAnim) == 0xa8, "CSimpleFlipBookAnim is 0xa8 bytes");

// A point of a path (0x40, vtable 0x1426c8e88).
class CSimpleControlPoint : public CScriptObject {
    public:
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;

    TSLink<CSimpleControlPoint> m_link;     // 0x18 the path's list
    CSimpleAnimPath* m_path = nullptr;      // 0x28
    float m_offset[2] = {};                 // 0x30 layout units
    float m_time = 0.0f;                    // 0x38 where on the path (0..1) it is reached
    int8_t m_order = -1;                    // 0x3C
    uint8_t m_pad3d[3] = {};

    static int32_t GetObjectType();
    // ref: FUN_1406065d0
    static void CreateScriptMetaTables();

    CSimpleControlPoint() = default;
    // ref: FUN_140604350
    ~CSimpleControlPoint() override;
    // ref: FUN_140607850
    int32_t GetScriptMetaTable() override;
    // ref: FUN_140608320
    bool IsObjectTypeName(const char* name) override;
    // ref: FUN_140608280
    bool IsObjectType(int32_t type) override;
    // ref: FUN_140607600
    const char* GetObjectTypeName() override;
    // ref: FUN_14038ab70
    uint32_t GetObjectSize() override;
    // ref: FUN_140607a30
    CScriptObject* GetParent() override;
    // ref: FUN_140609dd0
    virtual void LoadXML(XMLOBJECT* node, CStatus* status);

    // ref: FUN_14060ba10
    void SetOffset(const float* offset);
    // An order of -1 or more than 99 means the default (99, or 0) when `isDefault`.
    // ref: FUN_14060ba80
    void SetOrder(int32_t order, bool notify, bool isDefault);
    // Moves to another path; false when it is already there.
    // ref: FUN_14060bbb0
    bool SetParentPath(CSimpleAnimPath* path);
};
static_assert(sizeof(CSimpleControlPoint) == 0x40, "CSimpleControlPoint is 0x40 bytes");

// TSGrowableArray<CSimpleControlPoint*, 31> (0x20).
struct CONTROLPOINTARRAY {
    uint32_t m_count = 0;                   // 0x00
    uint32_t m_pad04 = 0;
    CSimpleControlPoint** m_data = nullptr; // 0x08
    uint32_t m_capacity = 0;                // 0x10
    uint32_t m_pad14 = 0;
    uint32_t m_granularity = 0;             // 0x18
    uint32_t m_pad1c = 0;

    // Room for `count` more.
    // ref: FUN_14060b860
    void Reserve(uint32_t count);
    void Clear();
};

// Path (0xd0, vtable 0x1426c8f40): a walk through control points, straight or smoothed.
class CSimpleAnimPath : public CSimpleAnim {
    public:
    TSExplicitList<CSimpleControlPoint> m_points;   // 0x90 in creation order
    CONTROLPOINTARRAY m_sorted;                     // 0xA8 by order
    uint8_t m_curve = ANIMCURVE_NONE;               // 0xC8
    uint8_t m_padC9[7] = {};

    static int32_t GetObjectType();
    // ref: FUN_140606710
    static void CreateScriptMetaTables();
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;

    // ref: FUN_140603d30
    explicit CSimpleAnimPath(CSimpleAnimGroup* group);
    // ref: FUN_1406044a0
    ~CSimpleAnimPath() override;
    // ref: FUN_140607910
    int32_t GetScriptMetaTable() override;
    // ref: FUN_140608830
    bool IsObjectTypeName(const char* name) override;
    // ref: FUN_140608790
    bool IsObjectType(int32_t type) override;
    // ref: FUN_140607640
    const char* GetObjectTypeName() override;
    // ref: FUN_140607450
    uint32_t GetObjectSize() override;
    // ref: FUN_14060a0d0
    void LoadXML(XMLOBJECT* node, CStatus* status) override;
    // ref: FUN_14060b4d0
    void PostLoadXML(XMLOBJECT* node, CStatus* status) override;
    // ref: FUN_140604ca0
    void Apply(float progress) override;

    // ref: FUN_1406061e0
    CSimpleControlPoint* CreateControlPoint(const char* name, const char* inherits, const int32_t* order);
    // Re-sorts the points by order, renumbering points that collide.
    // ref: FUN_140605260
    void SortControlPoints();
    // Spreads the points' times evenly.
    // ref: FUN_140605980
    void UpdateControlPointTimes();
    // A point's order changed.
    // ref: FUN_14060a9f0
    void OnControlPointOrderChanged(CSimpleControlPoint* point);
    // Inserts a point by order; its index, or the index of a point with the same order.
    // ref: FUN_14060b520
    int32_t InsertSorted(CSimpleControlPoint* point);
    // Whether a point with this order is in the sorted array (a binary search).
    bool HasOrder(int8_t order);
    // ref: FUN_1406073e0
    int32_t GetMaxControlPointOrder();
    // ref: FUN_140607270
    CONTROLPOINTLIST* GetScriptControlPoints(CONTROLPOINTLIST* points);
};
static_assert(sizeof(CSimpleAnimPath) == 0xd0, "CSimpleAnimPath is 0xd0 bytes");

// The AnimationGroup methods (CScriptObject's, then the group's own).
// ref: FUN_1408a8400
void CSimpleAnimGroup_FillScriptMethods(lua_State* L);

// Makes an animation of the named kind ("Animation" or empty for the base class); an unknown
// kind makes the base class too.
// ref: FUN_140605be0
CSimpleAnim* CSimpleAnim_Create(CSimpleAnimGroup* group, const char* type);

#endif
