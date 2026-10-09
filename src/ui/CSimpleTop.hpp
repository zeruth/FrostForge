#ifndef UI_C_SIMPLE_TOP_HPP
#define UI_C_SIMPLE_TOP_HPP

// CSimpleTop (Engine/Source/Frame/CSimpleTop.cpp): the root of the UI, which owns the frame
// strata and routes input. Not ported yet.

class CSimpleTop {
    public:
    // The one root (0x142e979d8).
    static CSimpleTop* s_instance;

    // Whether templates load under their own taint even without a frame being loaded (+0xf60).
    bool IsSecureLoad();
};

#endif
