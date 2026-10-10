// CSimpleEditBox: the wrapped lines, scrolling, the string it shows, the cursor and the
// highlight quads.

#include "ui/CSimpleEditBox.hpp"

#include <cstring>

#include "base/Coordinate.hpp"
#include "storm/Memory.hpp"
#include "ui/CRenderBatch.hpp"
#include "ui/CSimpleEditBoxText.hpp"
#include "ui/CSimpleFontString.hpp"
#include "ui/CSimpleTexture.hpp"
#include "ui/CSimpleTop.hpp"
#include "ui/FrameTime.hpp"
#include "ui/UITypes.hpp"

using namespace EditBoxText;

extern CSimpleFrame* g_editBoxFocus;
float SnapToPixels(float height);

namespace {

const char s_editBoxFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Frame\\CSimpleEditBox.cpp";
const char s_lineArrayTag[] = ".?AV?$TSGrowableArray@I@@";
const char s_highlightTag[] = ".?AUHighlightTextureInfo@CSimpleEditBox@@";

enum : uint8_t {
    SCRIPT_ONTEXTCHANGED = 0x2c,
    SCRIPT_ONCURSORCHANGED = 0x43,
};

// Where a scroll leaves the cursor: a quarter in from the left when it went off the left, three
// quarters when it went off the right (DAT_1426193c4, DAT_14261a994).
constexpr float SCROLL_LEFT = 0.25f;
constexpr float SCROLL_RIGHT = 0.75f;

// The highlight quads' texture coordinates (DAT_142d5e4b0) and indices (DAT_14297d7d8).
const float s_highlightTexCoords[8] = { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f };
const uint16_t s_quadIndices[6] = { 0, 1, 2, 2, 1, 3 };

// Screen pixels as layout units.
float PixelsToLayout(float pixels) {
    const COORDINATESPACE& space = g_coordinateSpace;
    return (pixels / (space.m_aspectCompensation * 1024.0f)) * space.m_diagonalWidth;
}

// Layout units as UI units.
float LayoutToUI(float value) {
    const COORDINATESPACE& space = g_coordinateSpace;
    return space.m_aspectCompensation * 1024.0f * value * (1.0f / space.m_diagonalWidth);
}

// TSGrowableArray<unsigned int, 31>::SetCount (FUN_1424b5a40).
void SetLineCount(CSimpleEditBox* box, uint32_t count) {
    if (box->m_lineCount < count && box->m_lineCapacity < count) {
        uint32_t granularity = box->m_lineGranularity;
        if (granularity == 0) {
            if (count < 0x40) {
                granularity = count;
                for (uint32_t low = (count - 1) & count; low != 0; low = (low - 1) & low) {
                    granularity = low;
                }
                if (granularity == 0) {
                    granularity = 1;
                }
            } else {
                box->m_lineGranularity = 0x40;
                granularity = 0x40;
            }
        }
        uint32_t capacity = count;
        if (count % granularity != 0) {
            capacity = granularity - count % granularity + count;
        }
        uint32_t* old = box->m_lineStarts;
        box->m_lineCapacity = capacity;
        box->m_lineStarts = capacity != 0
            ? static_cast<uint32_t*>(SMemAlloc(static_cast<uint64_t>(capacity) * 4, 8, s_lineArrayTag, 0, -7, 0))
            : nullptr;
        if (old != nullptr) {
            uint32_t keep = box->m_lineCount <= capacity ? box->m_lineCount : capacity;
            for (uint32_t i = 0; i < keep; i++) {
                box->m_lineStarts[i] = old[i];
            }
            SMemFree(old, s_lineArrayTag, -2, 0);
        }
    }
    box->m_lineCount = count;
}

// The highlight quads, grown or shrunk (FUN_1424b8460).
void SetHighlightCount(CSimpleEditBox* box, uint64_t count) {
    uint64_t current = box->m_highlightCount;
    if (count < current) {
        box->m_highlightCount = count;
        return;
    }
    if (count == current) {
        return;
    }
    uint64_t capacity = box->m_highlightCapacity;
    if ((capacity & 0x7fffffffffffffffull) < count) {
        uint64_t grown = ((capacity >> 1) & 0x3fffffffffffffffull) + (capacity & 0x7fffffffffffffffull);
        if (grown < count) {
            grown = count;
        }
        box->m_highlightCapacity = (capacity & 0x8000000000000000ull) | grown;
        auto quads = static_cast<CSimpleEditBox::HighlightTextureInfo*>(
            SMemAlloc(grown * sizeof(CSimpleEditBox::HighlightTextureInfo), 0xb, s_highlightTag, 0, -6, 0));
        if (box->m_highlightCount != 0) {
            std::memcpy(quads, box->m_highlights, static_cast<size_t>(box->m_highlightCount) * sizeof(CSimpleEditBox::HighlightTextureInfo));
        }
        if (static_cast<int64_t>(box->m_highlightCapacity) >= 0 && box->m_highlights != nullptr) {
            SMemFree(box->m_highlights, s_highlightTag, -6, 0);
        }
        box->m_highlightCapacity &= 0x7fffffffffffffffull;
        box->m_highlights = quads;
    }
    for (uint64_t i = box->m_highlightCount; i < count; i++) {
        box->m_highlights[i] = {};
    }
    box->m_highlightCount = count;
}

}

// ---- layout -----------------------------------------------------------------------------------

// ref: FUN_1424b6c60
void CSimpleEditBox::Update(bool cursor) {
    uint32_t changed = this->m_dirtyFlags;
    if (changed & DIRTY_TEXT) {
        this->m_dirtyFlags &= ~DIRTY_TEXT;
        this->UpdateText();
    }
    uint32_t dirty = this->m_dirtyFlags;
    if (dirty & DIRTY_DRAG) {
        this->UpdateDrag();
        this->m_dirtyFlags &= ~DIRTY_DRAG;
        dirty = this->m_dirtyFlags;
    }
    if (dirty & DIRTY_CURSOR) {
        int32_t start = this->m_visibleStart;
        int32_t end = start + this->m_visibleLength;
        if (((this->m_cursorPos < start || end < this->m_cursorPos) && this == g_editBoxFocus)
            || (this->m_imeComposing && this->m_imeHighlight != nullptr && end < this->m_imeEnd)) {
            dirty |= DIRTY_TEXT | DIRTY_LINES;
            this->m_dirtyFlags = dirty;
        }
    }
    if (dirty & DIRTY_TEXT) {
        this->m_dirtyFlags = dirty & ~DIRTY_TEXT;
        this->UpdateText();
    }
    if (this->m_dirtyFlags & DIRTY_HIGHLIGHT) {
        this->m_dirtyFlags &= ~DIRTY_HIGHLIGHT;
        this->UpdateHighlight();
    }
    if (cursor && (this->m_dirtyFlags & DIRTY_CURSOR)) {
        this->m_dirtyFlags &= ~DIRTY_CURSOR;
        this->UpdateCursor();
        this->m_cursorContext = 0;
    }
    if (changed & DIRTY_TEXT) {
        // FUN_1406039a0
        this->RunScript(SCRIPT_ONTEXTCHANGED, nullptr, ((this->m_dirtyFlags >> 4) & 1) != 0);
        this->Notify(this->m_textChangedListener);
        this->m_dirtyFlags &= ~DIRTY_USER_INPUT;
    }
}

// ref: FUN_1424afa90
void CSimpleEditBox::UpdateLines() {
    if (!(this->m_dirtyFlags & DIRTY_LINES)) {
        return;
    }
    this->m_dirtyFlags &= ~DIRTY_LINES;
    float width = this->m_string->GetWidth();
    char* text = this->DisplayBuffer();
    if (!(this->m_editFlags & EDIT_MULTI_LINE)) {
        float available = width - 2.0f * PixelsToLayout(1.0f);
        if (available <= 0.0f) {
            available = width;
        }
        int32_t count = static_cast<int32_t>(this->m_string->GetTextExtentCount(text + this->m_visibleStart, 0, available));
        this->m_visibleLength = count;
        if (text == this->m_text) {
            this->m_visibleLength = Walk(this->m_textInfo, this->m_visibleStart, count, false);
        }
        this->m_lineStarts[0] = static_cast<uint32_t>(this->m_visibleStart);
        this->m_lineStarts[1] = static_cast<uint32_t>(this->m_visibleStart + this->m_visibleLength);
        return;
    }
    const char* from = text + this->m_visibleStart;
    uint32_t lines = this->m_string->GetLineStarts(from, width, reinterpret_cast<int32_t*>(this->m_lineStarts), this->m_lineCount);
    if (lines == 0) {
        SetLineCount(this, 2);
        this->m_lineStarts[0] = 0;
        lines = 1;
    } else if (lines < this->m_lineCount) {
        SetLineCount(this, lines + 1);
    } else {
        SetLineCount(this, lines + 1);
        this->m_string->GetLineStarts(from, width, reinterpret_cast<int32_t*>(this->m_lineStarts), this->m_lineCount);
    }
    if (0 < this->m_visibleStart && lines != 0) {
        for (uint32_t i = 0; i < lines; i++) {
            this->m_lineStarts[i] += static_cast<uint32_t>(this->m_visibleStart);
        }
    }
    this->m_lineStarts[lines] = static_cast<uint32_t>(std::strlen(text));
    this->m_visibleLength = static_cast<int32_t>(this->m_lineStarts[lines]);
}

// ref: FUN_1424b1a50
int32_t CSimpleEditBox::GetLineIndex(uint32_t position) {
    this->UpdateLines();
    int32_t line = 0;
    const uint32_t* start = this->m_lineStarts + 1;
    while (*start <= position) {
        if (static_cast<int32_t>(this->m_lineCount) - 2 <= line) {
            return line;
        }
        start++;
        line++;
    }
    return line;
}

// ref: FUN_1424b2b10
void CSimpleEditBox::ScrollTo(int32_t position, float offset, float width) {
    if (this->m_editFlags & EDIT_MULTI_LINE) {
        return;
    }
    int32_t count = static_cast<int32_t>(this->m_string->GetTrailingCharCount(this->m_text, static_cast<uint32_t>(position), offset));
    int32_t start = position - Walk(this->m_textInfo, position, -count, false);
    this->m_visibleStart = start;
    if (start < 0) {
        this->m_visibleStart = 0;
        start = 0;
    }
    int32_t fits = static_cast<int32_t>(this->m_string->GetTextExtentCount(this->m_text + start, 0, width));
    this->m_visibleLength = fits;
    this->m_visibleLength = Walk(this->m_textInfo, this->m_visibleStart, fits, false);
    this->m_lineStarts[0] = static_cast<uint32_t>(this->m_visibleStart);
    this->m_lineStarts[1] = static_cast<uint32_t>(this->m_visibleLength + this->m_visibleStart);
}

// ref: FUN_1424aff80
bool CSimpleEditBox::GetIndexAtPoint(float x, float y, int32_t* index, int32_t* side) {
    CRect rect = {};
    if (!this->m_string->GetRect(&rect)) {
        return false;
    }
    if (rect.minX < x) {
        *side = rect.maxX <= x ? 2 : 0;
    } else {
        *side = 1;
    }
    const char* text = this->DisplayBuffer();
    float lineHeight = this->m_string->GetScaledLineHeight();
    uint32_t line = 0;
    this->UpdateLines();
    if (this->m_editFlags & EDIT_MULTI_LINE) {
        float below = (rect.maxY - rect.minY) - (y - rect.minY);
        while (lineHeight < below && line < this->m_lineCount - 2) {
            below -= lineHeight;
            line++;
        }
    }
    CSimpleFontString* string = this->m_string;
    uint32_t start = this->m_lineStarts[line];
    uint32_t next = this->m_lineStarts[line + 1];
    float left = rect.minX;
    float right = rect.maxX;
    uint32_t justify = string->m_stringFlags & 7;
    if (justify == 2) {
        float half = ((rect.maxX - rect.minX) - string->GetTextWidth(text + start, next - start) * string->m_layoutScale) * 0.5f;
        left = rect.minX + half;
        right = rect.maxX - half;
    }
    x = x - left;
    float clamped = 0.0f;
    if (0.0f <= x) {
        clamped = x <= right ? x : right;
    }
    if (justify == 1 || justify == 2) {
        int32_t count = static_cast<int32_t>(string->GetTextExtentCount(text + start, next - start, clamped / string->m_layoutScale));
        if (text == this->m_text) {
            count = Walk(this->m_textInfo, static_cast<int32_t>(start), count, false);
        }
        *index = static_cast<int32_t>(start) + count;
    } else if (justify == 4) {
        int32_t count = static_cast<int32_t>(
            string->GetTrailingCharCount(text + start, next - start, ((right - left) - clamped) / string->m_layoutScale));
        if (text == this->m_text) {
            count = Walk(this->m_textInfo, static_cast<int32_t>(start), count, false);
        }
        *index = static_cast<int32_t>(next) - count;
    }
    return true;
}

// ref: FUN_1424b6df0
void CSimpleEditBox::GetHighlightQuad(HighlightTextureInfo* quad, int32_t line, float lineHeight, int32_t start, int32_t end, bool local) {
    this->UpdateLines();
    int32_t lineStart = static_cast<int32_t>(this->m_lineStarts[line]);
    int32_t first = this->SkipHidden(lineStart <= start ? start : lineStart);
    int32_t last = this->SkipHidden(lineStart <= end ? end : lineStart);
    const char* text = this->DisplayBuffer();
    CSimpleFontString* string = this->m_string;
    float x = 0.0f;
    uint32_t justify = string->m_stringFlags & 7;
    if (justify == 4) {
        x = string->GetWidth();
        int32_t next = static_cast<int32_t>(this->m_lineStarts[line + 1]);
        if (next == first || (next - first == 1 && text[first] == '\n')) {
            x -= string->GetTextWidth(" ", 1);
        } else {
            x -= string->GetTextWidth(text + first, static_cast<uint32_t>(next - first));
        }
    } else if (justify == 1 || justify == 2) {
        if (justify == 2) {
            float lineWidth = string->GetTextWidth(text + lineStart, this->m_lineStarts[line + 1] - static_cast<uint32_t>(lineStart));
            x = (string->GetWidth() - lineWidth) * 0.5f;
        }
        if (first == lineStart) {
            x += string->GetWrappedWidth(text + lineStart);
        } else {
            x += string->GetTextWidth(text + lineStart, static_cast<uint32_t>(first - lineStart));
        }
    }
    float scale = this->m_layoutScale;
    float width = string->GetRangeWidth(text, static_cast<uint32_t>(first), static_cast<uint32_t>(last));
    float top = -(static_cast<float>(line) * lineHeight);
    float left;
    if (!local) {
        left = string->CalculateLeft() + scale * x;
        float base;
        if (!(this->m_editFlags & EDIT_MULTI_LINE)) {
            base = string->CalculateCenterY() + lineHeight * 0.5f;
        } else {
            base = string->CalculateTop();
        }
        top = base + top;
    } else {
        left = scale * x;
    }
    float right = width * scale + left;
    float depth = this->m_layoutDepth;
    quad->m_vertices[0][0] = left;
    quad->m_vertices[0][1] = top;
    quad->m_vertices[0][2] = depth;
    quad->m_vertices[1][0] = left;
    quad->m_vertices[1][1] = top - lineHeight;
    quad->m_vertices[1][2] = depth;
    quad->m_vertices[2][0] = right;
    quad->m_vertices[2][1] = top;
    quad->m_vertices[2][2] = depth;
    quad->m_vertices[3][0] = right;
    quad->m_vertices[3][1] = top - lineHeight;
    quad->m_vertices[3][2] = depth;
}

// ref: FUN_1424b7190
void CSimpleEditBox::UpdateDrag() {
    if (this->m_dragTime == 0) {
        return;
    }
    CSimpleTop* top = CSimpleTop::s_instance;
    int32_t index = 0;
    int32_t side = 0;
    if (!this->GetIndexAtPoint(top->m_mouseEvent.m_x, top->m_mouseEvent.m_y, &index, &side)) {
        return;
    }
    uint32_t now = FrameTime_GetTime();
    if (this->m_dragTime <= now && side != 0 && !(this->m_editFlags & EDIT_MULTI_LINE)) {
        if (side == 1) {
            index = index - Walk(this->m_textInfo, index, -1, true);
        } else if (side == 2) {
            index = index + Walk(this->m_textInfo, index, 1, true);
        }
        uint32_t later = FrameTime_GetTime();
        this->m_dirtyFlags |= DIRTY_TEXT | DIRTY_LINES;
        this->m_dragTime = later + 200;
    }
    int32_t position = this->SkipHidden(index);
    this->ExtendHighlight(position - this->m_cursorPos);
    this->SetCursorPosition(position);
}

// ref: FUN_1424b75d0
void CSimpleEditBox::UpdateSizes() {
    if (!(this->CLayoutFrame::m_flags & FLAG_RECT_VALID)) {
        return;
    }
    CSimpleFontString* string = this->m_string;
    string->ClearAllPointsVirtual();
    float left = this->m_textInsets[1];
    float right = this->m_textInsets[3];
    float top = this->m_textInsets[2];
    float bottom = this->m_textInsets[0];
    string->SetWidth((this->m_rect.maxX - this->m_rect.minX) / this->m_layoutScale - (right + left));
    float height = 0.0f;
    if (!(this->m_editFlags & EDIT_MULTI_LINE)) {
        height = (this->m_rect.maxY - this->m_rect.minY) / this->m_layoutScale - (bottom + top);
    }
    string->SetHeight(height);
    string->SetPoint(FRAMEPOINT_TOPLEFT, this, FRAMEPOINT_TOPLEFT, left, -top, true);
    float fontHeight = string->GetFontHeight(true);
    if (this->m_imeHighlight != nullptr) {
        this->m_imeHighlight->SetHeight(fontHeight);
    }
    float cursorWidth = PixelsToLayout(2.0f);
    this->m_cursor->ClearAllPointsVirtual();
    this->m_cursor->SetWidth(cursorWidth);
    this->m_cursor->SetHeight(fontHeight);
    this->m_dirtyFlags |= DIRTY_TEXT | DIRTY_HIGHLIGHT | DIRTY_CURSOR | DIRTY_LINES;
    if (CSimpleTop::s_instance != nullptr) {
        this->m_cursorContext = CSimpleTop::s_instance->m_inputPhase;
    }
}

// ref: FUN_1424b7790
void CSimpleEditBox::UpdateCursor() {
    int32_t cursor = this->m_cursorPos;
    if (cursor < this->m_visibleStart || this->m_visibleStart + this->m_visibleLength < cursor) {
        this->m_cursor->Hide();
        return;
    }
    const char* text = this->DisplayBuffer();
    float x = 0.0f;
    this->UpdateLines();
    uint32_t position = static_cast<uint32_t>(this->m_cursorPos);
    uint32_t last = this->m_lineCount - 2;
    uint32_t line = 0;
    uint32_t next = this->m_lineStarts[1];
    while (next <= position && line < last) {
        next = this->m_lineStarts[line + 2];
        line++;
    }
    CSimpleFontString* string = this->m_string;
    float height = string->GetFontHeight(true);
    float y = SnapToPixels(-((height + string->m_spacing) * static_cast<float>(line)));
    this->m_cursor->ClearAllPointsVirtual();
    bool multiLine = (this->m_editFlags & EDIT_MULTI_LINE) != 0;
    int32_t point = 0;
    int32_t relative = 0;
    switch (string->m_stringFlags & 7) {
        case 1: {
            uint32_t lineStart = this->m_lineStarts[line];
            if (position != lineStart) {
                x = string->GetTextWidth(text + lineStart, position - lineStart);
            }
            point = multiLine ? FRAMEPOINT_TOPLEFT : FRAMEPOINT_LEFT;
            relative = point;
            break;
        }
        case 2: {
            uint32_t lineStart = this->m_lineStarts[line];
            if (position != lineStart) {
                x = string->GetTextWidth(text + lineStart, position - lineStart);
            }
            x -= string->GetTextWidth(text + lineStart, this->m_lineStarts[line + 1] - lineStart) * 0.5f;
            point = multiLine ? FRAMEPOINT_TOPLEFT : FRAMEPOINT_LEFT;
            relative = multiLine ? FRAMEPOINT_TOP : FRAMEPOINT_CENTER;
            break;
        }
        case 4: {
            uint32_t lineEnd = this->m_lineStarts[line + 1];
            if (position != lineEnd) {
                x = -string->GetTextWidth(text + position, lineEnd - position);
            }
            point = multiLine ? FRAMEPOINT_TOPLEFT : FRAMEPOINT_LEFT;
            relative = multiLine ? FRAMEPOINT_TOPRIGHT : FRAMEPOINT_RIGHT;
            break;
        }
        default:
            goto placed;
    }
    this->m_cursor->SetPoint(point, string, relative, x, y, true);
placed:
    uint8_t context = this->m_cursorContext;
    this->m_blinkElapsed = 0.0f;
    float cursorHeight = this->m_cursor->GetHeight();
    float cursorWidth = this->m_cursor->GetWidth();
    // FUN_1424aefc0
    this->RunScript(SCRIPT_ONCURSORCHANGED, nullptr, LayoutToUI(x), LayoutToUI(y), LayoutToUI(cursorWidth), LayoutToUI(cursorHeight),
        static_cast<int32_t>(context));
    if (this == g_editBoxFocus) {
        this->m_cursor->Show();
        return;
    }
    this->m_cursor->Hide();
}

// ref: FUN_1424b7b70
void CSimpleEditBox::UpdateHighlight() {
    if (this->CLayoutFrame::m_flags & FLAG_DIRTY) {
        this->Resize(true);
    }
    float lineHeight = this->m_string->GetScaledLineHeight();
    CSimpleTexture* ime = this->m_imeHighlight;
    if (ime != nullptr) {
        if (!this->m_imeComposing) {
            ime->Hide();
        } else {
            int32_t start = this->m_imeStart;
            int32_t end = this->m_imeEnd;
            HighlightTextureInfo quad = {};
            if (start == end) {
                start = this->m_highlightStart;
                end = this->m_highlightEnd;
                ime->Hide();
            } else {
                ime->Show();
            }
            int32_t line = this->GetLineIndex(static_cast<uint32_t>(start));
            this->GetHighlightQuad(&quad, line, lineHeight, start, end, true);
            float inverse = 1.0f / this->m_layoutScale;
            for (auto& vertex : quad.m_vertices) {
                vertex[0] *= inverse;
                vertex[1] *= inverse;
                vertex[2] *= inverse;
            }
            int32_t point = (this->m_editFlags & EDIT_MULTI_LINE) ? FRAMEPOINT_TOPLEFT : FRAMEPOINT_LEFT;
            ime->ClearAllPointsVirtual();
            ime->SetPoint(point, this->m_string, point, quad.m_vertices[0][0], quad.m_vertices[0][1], true);
            ime->SetWidth(quad.m_vertices[2][0] - quad.m_vertices[0][0]);
        }
    }
    if (this->m_highlightStart == this->m_highlightEnd) {
        this->m_highlightCount = 0;
    } else {
        int32_t firstLine = this->GetLineIndex(static_cast<uint32_t>(this->m_highlightStart));
        int32_t lastLine = this->GetLineIndex(static_cast<uint32_t>(this->m_highlightEnd));
        SetHighlightCount(this, static_cast<uint64_t>(lastLine - firstLine + 1));
        this->UpdateLines();
        for (int32_t line = firstLine; line <= lastLine; line++) {
            int32_t lineEnd = static_cast<int32_t>(this->m_lineStarts[line + 1]);
            int32_t lineStart = static_cast<int32_t>(this->m_lineStarts[line]);
            int32_t end = lineEnd < this->m_highlightEnd ? lineEnd : this->m_highlightEnd;
            int32_t start = lineStart <= this->m_highlightStart ? this->m_highlightStart : lineStart;
            this->GetHighlightQuad(&this->m_highlights[line - firstLine], line, lineHeight, start, end, false);
        }
    }
    this->MarkBatchDirty();
}

// ref: FUN_1424b7e80
void CSimpleEditBox::UpdateText() {
    int32_t cursor = this->m_cursorPos < 1 ? 0 : this->m_cursorPos;
    this->m_cursorPos = cursor < this->m_textLength ? cursor : this->m_textLength;
    float width = this->m_string->GetWidth();
    if (this->m_editFlags & EDIT_PASSWORD) {
        uint64_t length = std::strlen(this->m_text);
        this->m_passwordText = static_cast<char*>(SMemReAlloc(this->m_passwordText, length + 1, 0x1f, s_editBoxFile, 0, 0x9c1, 0));
        std::memset(this->m_passwordText, '*', static_cast<size_t>(length));
        this->m_passwordText[length] = '\0';
    }
    this->UpdateLines();
    if (!(this->m_editFlags & EDIT_PASSWORD)) {
        if (this->m_imeComposing && this->m_imeHighlight != nullptr && this->m_visibleLength + this->m_visibleStart < this->m_imeEnd) {
            this->ScrollTo(this->m_imeEnd, width, width);
        }
        int32_t position = this->m_cursorPos;
        if (position < this->m_visibleStart) {
            this->ScrollTo(position, SCROLL_LEFT * width, width);
        } else if (this->m_visibleStart + this->m_visibleLength < position) {
            this->ScrollTo(position, SCROLL_RIGHT * width, width);
        }
    }
    char* display = this->DisplayBuffer();
    // A visible start inside a colour shows that colour: find the |c before a |r that comes
    // first.
    const char* color = nullptr;
    for (char* p = display + this->m_visibleStart; *p != '\0'; p++) {
        if (*p != '|') {
            continue;
        }
        char code = p[1];
        if (code == '|') {
            p++;
            continue;
        }
        if (code == 'c') {
            break;
        }
        if (code == 'r') {
            for (char* back = p; display <= back; back--) {
                if (*back == '|' && !(display < back && back[-1] == '|') && back[1] == 'c') {
                    color = back;
                    break;
                }
            }
            break;
        }
    }
    CSimpleFontString* string = this->m_string;
    string->UpdateFamilyMember(this->m_text, false, true);
    int32_t length = this->m_visibleLength;
    if (color == nullptr) {
        int32_t end = this->m_visibleStart + length;
        char saved = display[end];
        display[end] = '\0';
        string->SetText(display + this->m_visibleStart, false);
        display[end] = saved;
    } else {
        char* buffer = static_cast<char*>(_alloca((static_cast<uint64_t>(length) + 0x1a) & ~0xfull));
        std::memcpy(buffer, color, 10);
        std::memcpy(buffer + 10, display + this->m_visibleStart, static_cast<size_t>(this->m_visibleLength));
        buffer[this->m_visibleLength + 10] = '\0';
        string->SetText(buffer, false);
    }
    if (this->m_editFlags & EDIT_MULTI_LINE) {
        float height = string->GetStringHeight();
        if (height == 0.0f) {
            height = string->GetFontHeight(true);
        }
        this->SetHeight(this->m_textInsets[2] + this->m_textInsets[0] + height);
    }
}

// ---- frame overrides ---------------------------------------------------------------------------

// ref: FUN_1424b4340
void CSimpleEditBox::AddRegionsToBatch(CRenderBatch* batch) {
    this->CSimpleFrame::AddRegionsToBatch(batch);
    uint16_t blend = g_renderBatchBlends[2];
    uint64_t drawOrder = this->GetDrawOrderKey(DRAWLAYER_ARTWORK, 0);
    for (uint64_t i = 0; i < this->m_highlightCount; i++) {
        // FUN_1405ee720: the snapping bias, 0.3 (DAT_142618d5c).
        batch->AddGeometry(this, drawOrder, this->m_highlightTexture, blend, 4, &this->m_highlights[i].m_vertices[0][0], s_highlightTexCoords, 0,
            reinterpret_cast<const uint8_t*>(&this->m_highlightColor), 6, s_quadIndices, 0.0f, (this->m_flags2 & 0x780) == 0x480, 0.3f,
            true, 0, nullptr, nullptr);
    }
}

// ref: FUN_1424b44c0
void CSimpleEditBox::OnSizeChanged(float width, float height) {
    this->CSimpleFrame::OnSizeChanged(width, height);
    this->UpdateSizes();
}

// ref: FUN_1424b4180
void CSimpleEditBox::ScaleBy(float* scale) {
    this->m_textInsets[0] *= *scale;
    this->m_textInsets[1] *= *scale;
    this->m_textInsets[2] *= *scale;
    this->m_textInsets[3] *= *scale;
    this->CSimpleFrame::ScaleBy(scale);
}

// ref: FUN_1424b56d0
void CSimpleEditBox::OnLayoutDepthChanged() {
    for (uint64_t i = 0; i < this->m_highlightCount; i++) {
        for (auto& vertex : this->m_highlights[i].m_vertices) {
            vertex[2] = this->m_layoutDepth;
        }
    }
}

// ref: FUN_1424b44a0
void CSimpleEditBox::OnFrameSizeChanged(const CRect* rect, const CRect* previous) {
    this->CSimpleFrame::OnFrameSizeChanged(rect, previous);
    this->m_dirtyFlags |= DIRTY_HIGHLIGHT;
}

// ---- the IME -----------------------------------------------------------------------------------

// ref: FUN_1424b05d0
void CSimpleEditBox::CreateImeHighlight() {
    if (this->m_imeHighlight != nullptr) {
        return;
    }
    CSimpleTexture* texture = new CSimpleTexture(this, DRAWLAYER_OVERLAY, 0, true);
    this->m_imeHighlight = texture;
    uint32_t color = 0xff202010;
    texture->SetColorTexture(&color);
    texture->SetBlendMode(3);
    texture->SetHeight(this->m_string->GetFontHeight(true));
    texture->SetWidth(PixelsToLayout(2.0f));
    texture->Hide();
    texture->SetPoint(FRAMEPOINT_TOPLEFT, this->m_string, FRAMEPOINT_TOPLEFT, 0.0f, 0.0f, true);
}

// ref: FUN_1424b7410
void CSimpleEditBox::PositionCandidates() {
    if (this->m_candidateFrame == nullptr) {
        return;
    }
    this->CreateImeHighlight();
    float offset = 2.0f;
    int32_t point = FRAMEPOINT_BOTTOMLEFT;
    int32_t relative = FRAMEPOINT_TOPLEFT;
    CRect rect = {};
    if (this->m_imeHighlight->GetRect(&rect)) {
        float height = PixelsToLayout(15.0f);
        CScriptObject* first = this->m_candidateFrame->GetObjectByPath("c1");
        if (first != nullptr && first->IsObjectType(CSimpleFrame::GetObjectType())) {
            height = static_cast<CSimpleFrame*>(first)->GetHeight();
        }
        relative = FRAMEPOINT_TOPLEFT;
        if (0.0f <= rect.maxY - height * 10.0f) {
            relative = FRAMEPOINT_BOTTOMLEFT;
            point = FRAMEPOINT_TOPLEFT;
            offset = -2.0f;
        }
    }
    this->m_candidateFrame->ClearAllPointsVirtual();
    this->m_candidateFrame->SetPoint(point, this->m_imeHighlight, relative, 0.0f, PixelsToLayout(offset), true);
}
