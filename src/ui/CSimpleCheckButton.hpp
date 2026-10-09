#ifndef UI_C_SIMPLE_CHECK_BUTTON_HPP
#define UI_C_SIMPLE_CHECK_BUTTON_HPP

// Engine/Source/Frame/CSimpleCheckButton ("CheckButton", 0x380 bytes; vtable 0x14297c758,
// CLayoutFrame vtable 0x14297caa0 at +0x18, CAnimatableObject vtable 0x14297cb98 at +0x1d8): a
// button that toggles a checked state on each click and shows a checked texture over its own
// (or the disabled-checked one while disabled).

#include <cstdint>

#include "ui/CSimpleButton.hpp"

class CSimpleCheckButton : public CSimpleButton {
    public:
    bool m_checked;                             // 0x368
    uint8_t m_pad369[7];
    CSimpleTexture* m_checkedTexture;           // 0x370
    CSimpleTexture* m_disabledCheckedTexture;   // 0x378

    // ref: FUN_1424acbe0
    explicit CSimpleCheckButton(CSimpleFrame* parent);
    // ref: FUN_1424acc50
    ~CSimpleCheckButton() override;
    // Check buttons come from their own pool (0x1447aed58, 0x32 per block).
    static void* operator new(size_t size);
    static void operator delete(void* button);

    // This class's type id (DAT_14309fec4).
    static int32_t GetObjectType();
    // ref: FUN_1424acce0
    static void CreateScriptMetaTables();
    // ref: FUN_1424acd30
    static void DestroyScriptMetaTables();
    // The metatables (DAT_142d4e478, DAT_142d4e47c).
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;

    int32_t GetScriptMetaTable() override;
    bool IsObjectTypeName(const char* name) override;
    bool IsObjectType(int32_t type) override;
    const char* GetObjectTypeName() override;
    uint32_t GetObjectSize() override;
    void LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) override;
    void RemoveRegion(CSimpleRegion* region) override;
    void SetEnabled(bool enabled) override;
    void OnClick(const char* button, bool down, bool fromInput, bool secure, const char* taint) override;

    // Sets the checked state and shows the texture it calls for; `force` refreshes the
    // textures even when the state is unchanged (FUN_1424ad160).
    void SetChecked(bool checked, bool force);
    // ref: FUN_1424ad200
    bool SetCheckedTexture(const TEXTURESOURCE* source);
    // ref: FUN_1424ad3c0
    bool SetDisabledCheckedTexture(const TEXTURESOURCE* source);
};
static_assert(sizeof(CSimpleCheckButton) == 0x380, "CSimpleCheckButton is 0x380 bytes");

void CSimpleCheckButton_FillScriptMethods(lua_State* L);

#endif
