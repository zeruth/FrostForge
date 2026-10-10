#include "ui/CSimpleFontedFrame.hpp"

#include "ui/CSimpleFrame.hpp"
#include "util/Unimplemented.hpp"

// ---- CSimpleFontedFrameFont ------------------------------------------------------------------

// ref: FUN_1424d3790
CSimpleFontedFrameFont::CSimpleFontedFrameFont(CSimpleFontedFrame* owner) : CSimpleFont() {
    this->m_owner = owner;
}

// ref: FUN_1424d37e0
CSimpleFontedFrameFont::~CSimpleFontedFrameFont() = default;

// ref: FUN_1424d3820
void CSimpleFontedFrameFont::UpdateFontables() {
    uint16_t flags = this->m_attributes.m_flags;
    this->CSimpleFont::UpdateFontables();
    if (this->m_owner != nullptr) {
        this->m_owner->FontObjectChanged(this, flags);
    }
}

// ---- CSimpleIMEHost --------------------------------------------------------------------------

// ref: FUN_1424d29d0
void CSimpleIMEHost::HideCandidates() {
    if (this->m_candidateFrame != nullptr) {
        this->m_candidateFrame->Hide();
        this->m_candidateFrame = nullptr;
    }
}

// The candidate list frame is built from the IME's candidate strings, a button per candidate
// with the page and selection, and placed through PositionCandidates. Only an IME's
// candidate notifications reach it.
// ref: FUN_1424d2a00
int32_t CSimpleIMEHost::UpdateCandidates(bool page) {
    (void)page;
    FROST_UNIMPLEMENTED();
    return 0;
}

// ref: FUN_1424d3770
void CSimpleIMEHost::ShowCandidates() {
    if (this->m_candidateFrame != nullptr) {
        this->m_candidateFrame->Show();
    }
}
