#ifndef UI_C_SIMPLE_FONTED_FRAME_HPP
#define UI_C_SIMPLE_FONTED_FRAME_HPP

// The font object a text-owning frame keeps for itself (edit boxes, HTML and message frames):
// a CSimpleFont whose updates are passed on to the frame through the CSimpleFontedFrame
// interface. And the IME candidate list host an edit box carries.

#include <cstdint>

#include "ui/CSimpleFont.hpp"

class CSimpleFontedFrameFont;
class CSimpleFrame;

// What a fonted frame hears from its font object (vtable 0x14297d5e0 in CSimpleEditBox).
class CSimpleFontedFrame {
    public:
    // The font object's settings changed; `flags` are its CSimpleFontAttributes flags as they
    // were before the update cleared the changed bits.
    virtual void FontObjectChanged(CSimpleFontedFrameFont* font, uint16_t flags) = 0;
};

// Engine/Source/Frame/CSimpleFontedFrameFont (0xc8 bytes; vtable 0x142981420 with the
// CSimpleFontable vtable 0x142981498 at +0x10).
class CSimpleFontedFrameFont : public CSimpleFont {
    public:
    CSimpleFontedFrame* m_owner;            // 0xc0

    // ref: FUN_1424d3790
    explicit CSimpleFontedFrameFont(CSimpleFontedFrame* owner);
    // ref: FUN_1424d37e0
    ~CSimpleFontedFrameFont() override;

    // Tells the fontables, then the owner (FUN_1424d3820).
    void UpdateFontables() override;
};
static_assert(sizeof(CSimpleFontedFrameFont) == 0xc8, "CSimpleFontedFrameFont is 0xc8 bytes");

// The IME candidate list of an edit box (vtable 0x14297d5e8 in CSimpleEditBox): a frame built
// from the IME's candidates under the composition, and what the host tells it about itself.
class CSimpleIMEHost {
    public:
    CSimpleFrame* m_candidateFrame = nullptr;   // +0x08

    // Places the candidate list under the composition (CSimpleEditBox: FUN_1424b7410).
    virtual void PositionCandidates() = 0;
    // The host's font height (CSimpleEditBox: FUN_1424b1830).
    virtual float GetIMEFontHeight() = 0;
    // The host's layout scale (CSimpleEditBox: FUN_1424b1840).
    virtual float GetIMELayoutScale() = 0;

    // ref: FUN_1424d29d0
    void HideCandidates();
    // Builds the list from the IME's current candidates (FUN_1424d2a00); 0 when there are none.
    int32_t UpdateCandidates(bool page);
    // ref: FUN_1424d3770
    void ShowCandidates();
};

#endif
