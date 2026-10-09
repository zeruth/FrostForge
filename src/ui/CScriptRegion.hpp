#ifndef UI_C_SCRIPT_REGION_HPP
#define UI_C_SCRIPT_REGION_HPP

// CScriptObject (Engine/Source/Frame/CScriptObject.cpp) and CScriptRegion
// (Engine/Source/Frame/CScriptRegion.cpp): a scripted object with a name, and one with a
// layout frame -- every frame, texture and font string. Only the layout is in place so far:
// the CLayoutFrame base sits at +0x18 as in the reference.

#include "framescript/FrameScriptObject.hpp"
#include "ui/CLayoutFrame.hpp"

class CScriptObject : public FrameScript_Object {
    public:
    void* m_unknown10 = nullptr;    // 0x10
};
static_assert(sizeof(CScriptObject) == 0x18, "CScriptObject is 0x18 bytes");

class CScriptRegion : public CScriptObject, public CLayoutFrame {
};

#endif
