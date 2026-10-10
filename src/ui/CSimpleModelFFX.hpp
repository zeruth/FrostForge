#ifndef UI_C_SIMPLE_MODEL_FFX_HPP
#define UI_C_SIMPLE_MODEL_FFX_HPP

// Glue/CSimpleModelFFX ("ModelFFX", 0x13d8 bytes; vtable 0x1427e5d68, CLayoutFrame vtable
// 0x1427e60b8 at +0x18, CAnimatableObject vtable 0x1427e61b0 at +0x1d8): the glue's model frame.
// It keeps three sets of extra lights (scene, character and pet lights, two banks each of up to
// four) and draws through the full-screen-effect path with a glow.

#include <cstdint>

#include "model2/CM2Lighting.hpp"
#include "ui/CSimpleModel.hpp"

// A bank of lights (0x280).
struct MODELFFXLIGHTS {
    uint32_t m_count;               // 0x000
    uint32_t m_pad004;
    CM2Lighting m_lights[4];        // 0x008
    uint8_t m_dirty;                // 0x268
    uint8_t m_pad269[7];
    void* m_owner;                  // 0x270 the frame
    uint8_t m_278;                  // 0x278
    uint8_t m_pad279[7];
};
static_assert(sizeof(MODELFFXLIGHTS) == 0x280, "a light bank is 0x280 bytes");

class CSimpleModelFFX : public CSimpleModel {
    public:
    MODELFFXLIGHTS m_sceneLights[2];        // 0x4d0 AddLight
    MODELFFXLIGHTS m_characterLights[2];    // 0x9d0 AddCharacterLight
    MODELFFXLIGHTS m_petLights[2];          // 0xed0 AddPetLight
    float m_glow;                           // 0x13d0
    uint8_t m_pad13d4[4];

    // ref: FUN_141697530 (the construction half)
    explicit CSimpleModelFFX(CSimpleFrame* parent);
    // ref: FUN_1416970f0
    ~CSimpleModelFFX() override;
    // ModelFFX frames come from their own pool (0x143122788, 5 per block).
    static void* operator new(size_t size);
    static void operator delete(void* frame);

    // This class's type id (DAT_14309fee4).
    static int32_t GetObjectType();
    // ref: FUN_141697780
    static void CreateScriptMetaTables();
    static int32_t s_metaTable;             // DAT_142d1b678
    static int32_t s_restrictedMetaTable;   // DAT_142d1b67c

    int32_t GetScriptMetaTable() override;
    bool IsObjectTypeName(const char* name) override;
    bool IsObjectType(int32_t type) override;
    const char* GetObjectTypeName() override;
    uint32_t GetObjectSize() override;
    void LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) override;
    void QueueOnShow() override;
    void AddRegionsToBatch(CRenderBatch* batch) override;
    void SetModel(CM2Model* model) override;
    void SetGlow(float glow) override;

    // ref: FUN_141697290: a light into one of a set's two banks.
    void AddLight(MODELFFXLIGHTS* set, const uint32_t* index, const void* light);
    // ref: FUN_141697230 / FUN_141697260 / FUN_1416973c0
    void AddSceneLight(const uint32_t* index, const void* light);
    void AddCharacterLight(const uint32_t* index, const void* light);
    void AddPetLight(const uint32_t* index, const void* light);
    void ResetLights();
};

static_assert(sizeof(CSimpleModelFFX) == 0x13d8, "CSimpleModelFFX is 0x13d8 bytes");

void CSimpleModelFFX_FillScriptMethods(lua_State* L);


#endif
