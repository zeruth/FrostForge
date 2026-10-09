#include "ui/CSimpleFrame.hpp"

#include "util/Unimplemented.hpp"

CSimpleFrame* CSimpleFrame::s_loading;

// ref: FUN_1405ee7c0
int32_t CSimpleFrame::GetObjectType() {
    static int32_t s_type = ++FrameScript_Object::s_objectTypeCount;
    return s_type;
}

