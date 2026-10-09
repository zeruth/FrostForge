#ifndef UI_C_LAYOUT_FRAME_HPP
#define UI_C_LAYOUT_FRAME_HPP

// CLayoutFrame (Engine/Source/Frame/CLayoutFrame.cpp): the anchoring and sizing every UI
// region inherits. Not ported yet.

class CLayoutFrame {
    public:
    // Resolves every frame whose layout is waiting on a resize.
    static void ResizePending();
};

#endif
