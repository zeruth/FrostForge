#include "ui/CSimpleFrame.hpp"

#include "util/Unimplemented.hpp"

CSimpleFrame* CSimpleFrame::s_loading;

// ref: FUN_1405ee7c0
int32_t CSimpleFrame::GetObjectType() {
    static int32_t s_type = ++FrameScript_Object::s_objectTypeCount;
    return s_type;
}

// FUN_14060dee0
void CSimpleFrame::EnableEvent(int32_t type) {
    (void)type;
    FROST_UNIMPLEMENTED();
}

// FUN_14060f500
void CSimpleFrame::SetMouseMotionEnabled(bool enabled) {
    (void)enabled;
    FROST_UNIMPLEMENTED();
}

// FUN_14060dfd0
void CSimpleFrame::SetMouseClickEnabled(bool enabled) {
    (void)enabled;
    FROST_UNIMPLEMENTED();
}
