#ifndef UI_C_SIMPLE_FRAME_HPP
#define UI_C_SIMPLE_FRAME_HPP

// CSimpleFrame (Engine/Source/Frame/CSimpleFrame.cpp): the frame every UI widget is built on.
// Only what FrameScript_Object reaches is declared so far; the class is not ported yet.

#include <cstdint>

#include "ui/CScriptRegion.hpp"

class CSimpleFrame : public CScriptRegion {
    public:
    // The frame whose XML is being loaded (0x142e9a070, set by FUN_140614c20).
    static CSimpleFrame* s_loading;

    // This class's type id (0x142e979f4).
    static int32_t GetObjectType();

    // Turns on delivery of one kind of frame event (m_eventsEnabled, +0x1c8).
    void EnableEvent(int32_t type);
    void SetMouseMotionEnabled(bool enabled);
    void SetMouseClickEnabled(bool enabled);
};

#endif
