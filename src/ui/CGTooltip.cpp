#include "ui/CGTooltip.hpp"

#include <cstring>
#include <new>

#include <windows.h>

#include "base/DataAllocator.hpp"
#include "framescript/FrameScript.hpp"
#include "storm/Memory.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

// The reference's own functions this class calls, translated (src/gen); each line below makes
// tools/translate.py take it as a root.
extern "C" {
int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);
// translated: FUN_141c299b0
unsigned long long FUN_141c299b0(unsigned long long data);
// translated: FUN_140bb47b0
unsigned long long FUN_140bb47b0(unsigned long long data);
// translated: FUN_14057ac30
unsigned long long FUN_14057ac30(unsigned long long list, unsigned long long object, unsigned long long a3);
// translated: FUN_1418e9ec0
unsigned long long FUN_1418e9ec0();
// translated: FUN_141cd0aa0
unsigned long long FUN_141cd0aa0(unsigned long long tooltip);
// translated: FUN_141cd5b60
unsigned long long FUN_141cd5b60();
// translated: FUN_141cd7720
unsigned long long FUN_141cd7720(unsigned long long tooltip, unsigned long long node, unsigned long long status);
// translated: FUN_141cd6810
unsigned long long FUN_141cd6810(unsigned long long tooltip, double elapsed);
// translated: FUN_141ce01f0
unsigned long long FUN_141ce01f0(unsigned long long tooltip, unsigned long long owner, unsigned long long anchor,
                                 float x, float y);
// translated: FUN_141ce96e0
unsigned long long FUN_141ce96e0(unsigned long long layout);
}

int32_t CGTooltip::s_metaTable = -2;           // DAT_142d37e70
int32_t CGTooltip::s_restrictedMetaTable = -2; // DAT_142d37e74

namespace {

// The tooltips' pool (DAT_143251cf0).
CDataAllocator s_tooltipHeap(0x8d0, 10);  // DAT_143251cf0

// The reference's scripts past the Frame ones a tooltip has (0x34..0x3d: OnTooltipSetDefaultAnchor
// .. OnTooltipSetFramesToHide); only OnTooltipAddMoney takes arguments.
enum : uint8_t {
    SCRIPT_TOOLTIP_FIRST = 0x34,
    SCRIPT_ONTOOLTIPADDMONEY = 0x36,
    SCRIPT_TOOLTIP_COUNT = 10,
};

template <class F>
void Guarded(uint64_t address, F&& body) {
    __try {
        body();
    } __except (GenFault(address, GetExceptionInformation())) {
    }
}

}  // namespace

// ref: FUN_14167b220 (the pool half)
void* CGTooltip::operator new(size_t size) {
    (void)size;
    return s_tooltipHeap.GetData(false, ".?AVCGTooltip@@", -2, 0x1f);
}

void CGTooltip::operator delete(void* tooltip) {
    if (tooltip != nullptr) {
        s_tooltipHeap.PutData(tooltip);
    }
}

// ref: FUN_141cbffe0
CGTooltip::CGTooltip(CSimpleFrame* parent) : CSimpleFrame(parent) {
    // The reference sets each field; what it leaves unset is the pool's leftovers, zero here.
    std::memset(this->m_tooltip, 0, sizeof(this->m_tooltip));
    this->At<uint32_t>(0x478) = 0xffffffff;
    this->At<uint64_t>(0x5d8) = ~0ull;
    this->At<uint64_t>(0x5e0) = ~0ull;
    this->At<uint64_t>(0x5e8) = ~0ull;
    // a blz::function's manager: the reference's empty one (0x1402112e0, a bare ret)
    this->At<uint64_t>(0x420) = 0x1402112e0ull;
    // the tooltip's data block (+0x638): its lines' and the money frames' state
    auto self = reinterpret_cast<unsigned long long>(this);
    Guarded(0x141c299b0, [&] { FUN_141c299b0(self + 0x638); });
    this->At<uint8_t>(0x8cd) &= 0xf8;
    this->At<uint8_t>(0x8ce) &= 0xf9;
    this->At<uint32_t>(0x8c8) = 0;
    this->At<uint8_t>(0x8cc) = 0;
}

// ref: FUN_141cc1060
CGTooltip::~CGTooltip() {
    auto self = reinterpret_cast<unsigned long long>(this);
    // off the two lists that point at it, then the money frames when the money system is up
    Guarded(0x14057ac30, [&] {
        FUN_14057ac30(0x142e7c6a0ull, self, 0);
        FUN_14057ac30(0x142e5ed20ull, self, 0);
        if (FUN_1418e9ec0() != 0) {
            FUN_141cd0aa0(self);
        }
        FUN_140bb47b0(self + 0x638);
    });
    this->At<uint8_t>(0x508) = 0;
    // the stored callback's manager destroys it
    auto manager = reinterpret_cast<uint64_t (*)(void*, uint64_t, uint64_t)>(this->At<uint64_t>(0x420));
    Guarded(0x141cc1060, [&] { manager(&this->At<uint8_t>(0x428), 0, 0); });
    if (static_cast<int64_t>(this->At<uint64_t>(0x378)) >= 0) {
        SMemFree(reinterpret_cast<void*>(this->At<uint64_t>(0x368)), ".M", -6, 0);
        this->At<uint64_t>(0x368) = 0;
    }
    if (this->At<uint8_t>(0x365) != 0 && this->At<uint8_t>(0x364) != 0) {
        ::operator delete[](reinterpret_cast<void*>(this->At<uint64_t>(0x358)));
    }
    const char* lineTag = ".?AV?$TSFixedArray@PEAVCSimpleFontString@@$07@@";
    if (this->At<uint64_t>(0x340) != 0) {
        SMemFree(reinterpret_cast<void*>(this->At<uint64_t>(0x340)), lineTag, -2, 0);
    }
    if (this->At<uint64_t>(0x320) != 0) {
        SMemFree(reinterpret_cast<void*>(this->At<uint64_t>(0x320)), lineTag, -2, 0);
    }
}

int32_t CGTooltip::GetObjectType() {
    return static_cast<int32_t>(FUN_141cd5b60());
}

// ref: FUN_141cd13c0
void CGTooltip::CreateScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    s_metaTable = FrameScript_Object::CreateScriptMetaTable(L, CGTooltip_FillScriptMethods, false);
    s_restrictedMetaTable = FrameScript_Object::CreateScriptMetaTable(L, CGTooltip_FillScriptMethods, true);
}

// ref: FUN_141cd1410
void CGTooltip::DestroyScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    luaL_unref(L, LUA_REGISTRYINDEX, s_metaTable);
    luaL_unref(L, LUA_REGISTRYINDEX, s_restrictedMetaTable);
    s_metaTable = -2;
    s_restrictedMetaTable = -2;
}

// ref: FUN_141cd5c90
int32_t CGTooltip::GetScriptMetaTable() {
    if (!(this->FrameScript_Object::m_flags & FSOBJ_FORBIDDEN)
        && !(this->FrameScript_Object::m_flags & FSOBJ_PROTECTED_METATABLE)) {
        return s_metaTable;
    }
    return s_restrictedMetaTable;
}

// ref: FUN_141ce9b80
bool CGTooltip::GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) {
    if (static_cast<uint8_t>(index - SCRIPT_TOOLTIP_FIRST) < SCRIPT_TOOLTIP_COUNT) {
        if (index == SCRIPT_ONTOOLTIPADDMONEY) {
            data->m_wrapper = "return function(self,cost,maxcost) %s end";
        }
        return true;
    }
    return this->CSimpleFrame::GetScriptData(index, data);
}

// ref: FUN_141cd6640
bool CGTooltip::IsObjectTypeName(const char* name) {
    return _stricmp(name, "GameTooltip") == 0 || this->CSimpleFrame::IsObjectTypeName(name);
}

// ref: FUN_141cd6540
bool CGTooltip::IsObjectType(int32_t type) {
    return type == CGTooltip::GetObjectType() || this->CSimpleFrame::IsObjectType(type);
}

// ref: FUN_141cd5bd0
const char* CGTooltip::GetObjectTypeName() {
    return "GameTooltip";
}

// ref: FUN_141cd5b50
uint32_t CGTooltip::GetObjectSize() {
    return 0x8d0;
}

// ref: FUN_141cd7720
void CGTooltip::PostLoadXML(XMLOBJECT* node, CStatus* status) {
    auto self = reinterpret_cast<unsigned long long>(this);
    Guarded(0x141cd7720, [&] {
        FUN_141cd7720(self, reinterpret_cast<unsigned long long>(node), reinterpret_cast<unsigned long long>(status));
    });
}

// ref: FUN_141cd6810
void CGTooltip::RunOnUpdate(float elapsed) {
    auto self = reinterpret_cast<unsigned long long>(this);
    Guarded(0x141cd6810, [&] { FUN_141cd6810(self, elapsed); });
}

// ref: FUN_14026bea0
bool CGTooltip::WantsUpdate() {
    return true;
}

// Hidden: the owner let go of (FUN_141ce01f0 with none, anchor 9), the frame hidden, and the
// shown-for-owner bit (0x10 at +0x8cc) cleared.
// ref: FUN_141cd6500
void CGTooltip::ApplyHide() {
    auto self = reinterpret_cast<unsigned long long>(this);
    Guarded(0x141ce01f0, [&] { FUN_141ce01f0(self, 0, 9, 0.0f, 0.0f); });
    this->CSimpleFrame::ApplyHide();
    this->m_tooltip[0x8cc - 0x300] &= ~0x10;
}

// ref: FUN_141ce96e0
void CGTooltip::ApplyShow() {
    auto layout = reinterpret_cast<unsigned long long>(static_cast<CLayoutFrame*>(this));
    Guarded(0x141ce96e0, [&] { FUN_141ce96e0(layout); });
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_143251cf0 = (void*)&s_tooltipHeap;
// ---- end of generated globals ----
