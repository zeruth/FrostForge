#ifndef UI_C_G_TOOLTIP_HPP
#define UI_C_G_TOOLTIP_HPP

// Game/Source/UI/CGTooltip ("GameTooltip", 0x8d0 bytes; vtable 0x142844960, CLayoutFrame vtable
// 0x142844c78 at +0x18, CAnimatableObject vtable 0x142844d70 at +0x1d8): a frame of font-string
// lines, sized to them, owned by another frame and anchored to it, with the game's item, unit
// and spell descriptions filled in by its methods.
//
// Past CSimpleFrame the object keeps the reference's layout byte for byte: its Lua methods (85,
// src/ui/CGTooltipScript.cpp) and its larger overrides are the reference's own, translated
// (src/gen), and read it at the reference's offsets. The port declares what it touches itself.

#include <cstdint>

#include "ui/CSimpleFrame.hpp"

struct lua_State;

class CGTooltip : public CSimpleFrame {
    public:
    // The tooltip's own fields, at the reference's offsets (0x300..0x8d0). Named where the port
    // reads them; the rest is the translated methods'.
    uint8_t m_tooltip[0x8d0 - 0x300];

    // ref: FUN_141cbffe0
    explicit CGTooltip(CSimpleFrame* parent);
    // ref: FUN_141cc1060
    ~CGTooltip() override;
    // Tooltips come from their own pool (DAT_143251cf0, 10 a chunk).
    static void* operator new(size_t size);
    static void operator delete(void* tooltip);

    // This class's type id (the reference's own, FUN_141cd5b60, so translated code agrees).
    static int32_t GetObjectType();
    // ref: FUN_141cd13c0
    static void CreateScriptMetaTables();
    // ref: FUN_141cd1410
    static void DestroyScriptMetaTables();
    // The metatables (DAT_142d37e70, DAT_142d37e74).
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;

    int32_t GetScriptMetaTable() override;
    bool GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) override;
    bool IsObjectTypeName(const char* name) override;
    bool IsObjectType(int32_t type) override;
    const char* GetObjectTypeName() override;
    uint32_t GetObjectSize() override;
    void PostLoadXML(XMLOBJECT* node, CStatus* status) override;
    void RunOnUpdate(float elapsed) override;
    bool WantsUpdate() override;
    void ApplyHide() override;
    void ApplyShow() override;

    private:
    template <class T>
    T& At(uint32_t offset) {
        return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(this) + offset);
    }
};
static_assert(sizeof(CGTooltip) == 0x8d0, "CGTooltip is 0x8d0 bytes");

// The GameTooltip methods after the Frame ones (FUN_141cd7ee0).
void CGTooltip_FillScriptMethods(lua_State* L);

#endif
