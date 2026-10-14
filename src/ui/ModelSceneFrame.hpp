#ifndef UI_MODEL_SCENE_FRAME_HPP
#define UI_MODEL_SCENE_FRAME_HPP

// Game/Source/UI/ModelSceneFrame ("ModelScene", 0x4d0 bytes; vtable 0x142828738, CLayoutFrame
// vtable 0x142828a50 at +0x18, CAnimatableObject vtable 0x142828b48 at +0x1d8): a frame that
// draws its own small world scene, a camera, fog and lights, and the actors (ModelSceneActor,
// script objects of their own) placed in it.
//
// Past CSimpleFrame the object keeps the reference's layout: its Lua methods (45,
// src/ui/ModelSceneFrameScript.cpp) and its larger overrides are the reference's own, translated,
// and read it at the reference's offsets. Its fog and lighting are the port's own MODELFOG and
// CM2Lighting, built in place where the reference's are.

#include <cstdint>

#include "ui/CSimpleFrame.hpp"

class CM2Lighting;
class CWorldScene;
struct MODELFOG;
struct lua_State;

class ModelSceneFrame : public CSimpleFrame {
    public:
    CWorldScene* m_scene;                   // 0x300 created by the scene methods
    uint8_t m_fields[0x370 - 0x308];        // 0x308 camera and view settings
    uint8_t m_fog[0xa0];                    // 0x370 MODELFOG
    uint8_t m_lighting[0x98];               // 0x410 CM2Lighting
    uint8_t m_tail[0x4d0 - 0x4a8];          // 0x4a8 flags, the scale-tracked insets

    // ref: FUN_141a960c0
    explicit ModelSceneFrame(CSimpleFrame* parent);
    // ref: FUN_141a962e0
    ~ModelSceneFrame() override;
    // Model scenes come from their own pool (DAT_143208508, 10 a chunk).
    static void* operator new(size_t size);
    static void operator delete(void* frame);

    // This class's type id: the reference's own function-local static (DAT_1430a0b0c, guard
    // DAT_1430a0b10), so the translated code agrees.
    static int32_t GetObjectType();
    // The model scene a script method was called on (FUN_140d11f10).
    static FrameScript_Object* GetObjectFromLua(const char** error, lua_State* L, int32_t index, bool allowForbidden,
        bool allowLockdown);
    // ref: FUN_141a96680
    static void CreateScriptMetaTables();
    // The metatables (DAT_142d2f568, DAT_142d2f56c).
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;

    int32_t GetScriptMetaTable() override;
    bool GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) override;
    bool IsObjectTypeName(const char* name) override;
    bool IsObjectType(int32_t type) override;
    const char* GetObjectTypeName() override;
    uint32_t GetObjectSize() override;
    void LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) override;
    void RunOnUpdate(float elapsed) override;
    bool WantsUpdate() override;
    void AddRegionsToBatch(CRenderBatch* batch) override;
    void ScaleBy(float* scale) override;
    void OnFrameSizeChanged(const CRect* rect, const CRect* previous) override;
    void Unknown08() override;

    MODELFOG* Fog() {
        return reinterpret_cast<MODELFOG*>(this->m_fog);
    }
    CM2Lighting* Lighting() {
        return reinterpret_cast<CM2Lighting*>(this->m_lighting);
    }

    private:
    template <class T>
    T& At(uint32_t offset) {
        return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(this) + offset);
    }
};
static_assert(sizeof(ModelSceneFrame) == 0x4d0, "ModelSceneFrame is 0x4d0 bytes");

// The ModelScene methods after the Frame ones (FUN_14107ea20).
void ModelSceneFrame_FillScriptMethods(lua_State* L);

#endif
