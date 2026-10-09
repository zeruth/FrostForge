#ifndef UI_C_SIMPLE_FRAME_HPP
#define UI_C_SIMPLE_FRAME_HPP

// CSimpleFrame (Engine/Source/Frame/CSimpleFrame.cpp): the frame every UI widget is built on.
// Only what FrameScript_Object reaches is declared so far; the class is not ported yet.

#include <cstdint>

#include "ui/CScriptRegion.hpp"

// CAnimatableObject (Engine/Source/Frame/CAnimatableObject.cpp): the animation host every frame
// is (vtable 0x1426ca820 at CSimpleFrame +0x1d8). Not ported yet.
class CAnimatableObject {
    public:
    void* m_unknown08 = nullptr;

    virtual ~CAnimatableObject() = default;
};

class CSimpleFrame : public CScriptRegion, public CAnimatableObject {
    public:
    // The frame whose XML is being loaded (0x142e9a070, set by FUN_140614c20).
    static CSimpleFrame* s_loading;

    // This class's type id (0x142e979f4).
    static int32_t GetObjectType();

    uint32_t m_unknown1E8;          // 0x1e8
    uint32_t m_frameFlags;          // 0x1ec 0x40 clips its children, 0x2000 batch dirty,
                                    //       0x4000 draws in its own batch
};

#endif
