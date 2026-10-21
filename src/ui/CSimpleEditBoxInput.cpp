// CSimpleEditBox: the keyboard focus, keys, the mouse, the IME and the per-frame update.

#include "ui/CSimpleEditBox.hpp"

#include <cstring>

#include "base/CharClass.hpp"
#include "client/ClientInput.hpp"
#include "framescript/FrameScript.hpp"
#include "os/Window.hpp"
#include "storm/Memory.hpp"
#include "storm/Unicode.hpp"
#include "texture/Texture.hpp"
#include "ui/CSimpleEditBoxText.hpp"
#include "ui/CSimpleFontString.hpp"
#include "ui/CSimpleTexture.hpp"
#include "ui/CSimpleTop.hpp"
#include "ui/FrameTime.hpp"
#include "ui/UITypes.hpp"

extern "C" {
#include "lua.h"
}

using namespace EditBoxText;

extern CSimpleFrame* g_editBoxFocus;
const char* KeyToString(int32_t key);

namespace {

constexpr uint32_t FRAME_DISABLED = 0x8;
// Keys that reach the frame are passed on to the parent unless the frame is protected.
constexpr uint32_t FRAME_PROPAGATE_KEYBOARD = 0x1000;

enum : uint8_t {
    SCRIPT_ONKEYDOWN = 0x1b,
    SCRIPT_ONKEYUP = 0x1c,
    SCRIPT_ONENABLE = 0x21,
    SCRIPT_ONDISABLE = 0x22,
    SCRIPT_ONENTERPRESSED = 0x23,
    SCRIPT_ONESCAPEPRESSED = 0x24,
    SCRIPT_ONTABPRESSED = 0x29,
    SCRIPT_ONEDITFOCUSLOST = 0x2a,
    SCRIPT_ONEDITFOCUSGAINED = 0x2b,
    SCRIPT_ONINPUTLANGUAGECHANGED = 0x40,
    SCRIPT_ONCHARCOMPOSITION = 0x41,
    SCRIPT_ONARROWPRESSED = 0x49,
};

// The keys the edit box handles.
enum : int32_t {
    KEY_ESCAPE = 0x10f,
    KEY_ENTER = 0x110,
    KEY_BACKSPACE = 0x111,
    KEY_TAB = 0x112,
    KEY_LEFT = 0x113,
    KEY_UP = 0x114,
    KEY_RIGHT = 0x115,
    KEY_DOWN = 0x116,
    KEY_INSERT = 0x117,
    KEY_DELETE = 0x118,
    KEY_HOME = 0x119,
    KEY_END = 0x11a,
    KEY_PRINTSCREEN = 0x121,
};

// The input languages, by OsImeGetLanguage's order turned into the edit box's (0x142d5e4d0).
const char* const s_inputLanguages[] = { "ROMAN", "KOREAN", "CHINESE", "JAPANESE" };

const char s_tabText[] = "    ";
const char s_barText[] = "||";
const char s_newline[] = "\n";

// Whether a frame may take the edit focus from the one that has it: a protected focus only
// goes to another protected frame (FUN_1406149f0).
bool CanTakeKeyboardFocus(FrameScript_Object* frame) {
    auto focus = static_cast<FrameScript_Object*>(g_editBoxFocus);
    if (focus == nullptr) {
        return true;
    }
    uint16_t flags = focus->m_flags;
    if (!(flags & 8) && (flags & 0x20)) {
        return true;
    }
    if (frame != nullptr) {
        uint16_t own = frame->m_flags;
        if ((own & 8) || !(own & 0x20)) {
            return true;
        }
    }
    return false;
}

// Turns what the clipboard or the IME gave into text the edit box takes (the filter the
// paste and the IME result share).
// ref: FUN_141650f90
void InsertFiltered(CSimpleEditBox* box, const char* text) {
    for (const char* p = text; *p != '\0';) {
        int32_t bytes = 0;
        uint32_t code = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(p), &bytes);
        char buffer[8];
        const char* insert = nullptr;
        if (code < 0x20 || code == 0x7f) {
            if (code == 9) {
                insert = s_tabText;
            } else if ((box->m_editFlags & CSimpleEditBox::EDIT_MULTI_LINE) && code == 10) {
                uint8_t* end = SUniSPutUTF8(code, reinterpret_cast<uint8_t*>(buffer));
                *end = 0;
                insert = buffer;
            }
        } else if (code == '|') {
            insert = s_barText;
        } else {
            uint8_t* end = SUniSPutUTF8(code, reinterpret_cast<uint8_t*>(buffer));
            *end = 0;
            insert = buffer;
        }
        if (insert != nullptr) {
            box->Insert(insert, nullptr, true, false, false, true);
        }
        p += bytes;
    }
}

}

// ---- focus -----------------------------------------------------------------------------------

// ref: FUN_1424b5fa0
void CSimpleEditBox::SetFocus(bool userInput) {
    if (!(this->m_layoutState & LAYOUT_VISIBLE) || (this->m_frameFlags & FRAME_DISABLED) || this == g_editBoxFocus) {
        return;
    }
    if (!CanTakeKeyboardFocus(this)) {
        return;
    }
    if (g_editBoxFocus != nullptr) {
        g_editBoxFocus->OnEditFocusChanged(false, true, userInput);
    }
    g_editBoxFocus = this;
    this->OnEditFocusChanged(true, true, userInput);
}

// ref: FUN_1424afeb0
void CSimpleEditBox::ClearFocus() {
    if (g_editBoxFocus == this) {
        g_editBoxFocus = nullptr;
        this->OnEditFocusChanged(false, true, false);
    }
}

// ref: FUN_1424b1c10
bool CSimpleEditBox::HasFocus() {
    return this == g_editBoxFocus;
}

// ref: FUN_1424b5cc0
void CSimpleEditBox::SetEnabled(bool enabled) {
    if (!enabled && g_editBoxFocus == this) {
        g_editBoxFocus = nullptr;
        this->OnEditFocusChanged(false, true, false);
        this->SetFrameFlag(FRAME_DISABLED, true);
    } else {
        this->SetFrameFlag(FRAME_DISABLED, !enabled);
        if (enabled) {
            if (!(this->FrameScript_Object::m_flags & FSOBJ_LOADING)) {
                this->RunScript(SCRIPT_ONENABLE, nullptr);
            }
            return;
        }
    }
    if (!(this->FrameScript_Object::m_flags & FSOBJ_LOADING)) {
        this->RunScript(SCRIPT_ONDISABLE, nullptr);
    }
}

// ref: FUN_1424b41e0
void CSimpleEditBox::OnEditFocusChanged(bool focused, bool runScripts, bool userInput) {
    CSimpleTop* top = CSimpleTop::s_instance;
    // A password focus also calls a hook the release build compiles to nothing.
    if (!focused) {
        if (!(this->m_editFlags & (EDIT_NUMERIC | EDIT_NUMERIC_FULL_RANGE)) && !(this->m_editFlags & EDIT_PASSWORD)) {
            this->HideCandidates();
            if (this->m_imeComposing) {
                this->m_dirtyFlags |= DIRTY_HIGHLIGHT | DIRTY_CURSOR;
                this->m_imeComposing = false;
                if (top != nullptr) {
                    this->m_cursorContext = top->m_inputPhase;
                }
                if (this->m_imeHighlight != nullptr) {
                    this->m_imeHighlight->Hide();
                }
                int32_t end = this->m_highlightEnd;
                this->m_highlightStart = end;
                this->m_imeEnd = end;
                this->m_imeStart = end;
                this->m_cursorPos = end;
            }
            top->EnableIme(false);
        }
        if (runScripts) {
            // FUN_1406039a0
            this->RunScript(SCRIPT_ONEDITFOCUSLOST, nullptr, userInput);
        }
        return;
    }
    if (!(this->m_editFlags & (EDIT_NUMERIC | EDIT_PASSWORD | EDIT_NUMERIC_FULL_RANGE))) {
        top->EnableIme(true);
    }
    if (runScripts) {
        this->RunScript(SCRIPT_ONEDITFOCUSGAINED, nullptr, userInput);
    }
    this->m_dirtyFlags |= DIRTY_CURSOR;
    if (top != nullptr) {
        this->m_cursorContext = top->m_inputPhase;
    }
}

// ---- input handlers ----------------------------------------------------------------------------

// ref: FUN_1424b8270
bool CSimpleEditBox::HasCharHandler() {
    if (!(this->m_layoutState & LAYOUT_VISIBLE) || (this->m_frameFlags & FRAME_DISABLED)) {
        return false;
    }
    if (this == g_editBoxFocus) {
        return true;
    }
    return (this->m_editFlags & EDIT_AUTO_FOCUS) && CanTakeKeyboardFocus(this);
}

// ref: FUN_1424b82c0
bool CSimpleEditBox::Unknown120() {
    if (!(this->m_layoutState & LAYOUT_VISIBLE) || (this->m_frameFlags & FRAME_DISABLED)) {
        return false;
    }
    if (this != g_editBoxFocus && (!(this->m_editFlags & EDIT_AUTO_FOCUS) || !CanTakeKeyboardFocus(this))) {
        return false;
    }
    return !(this->m_editFlags & EDIT_PASSWORD);
}

// ref: FUN_1424b8320
bool CSimpleEditBox::HasKeyHandler(const void* event) {
    if (!(this->m_layoutState & LAYOUT_VISIBLE) || (this->m_frameFlags & FRAME_DISABLED)) {
        return false;
    }
    if (this != g_editBoxFocus && (!(this->m_editFlags & EDIT_AUTO_FOCUS) || !CanTakeKeyboardFocus(this))) {
        return false;
    }
    int32_t key = *static_cast<const int32_t*>(event);
    if (key == KEY_PRINTSCREEN) {
        return false;
    }
    if ((this->m_editFlags & EDIT_ALT_ARROW_KEY_MODE) && static_cast<uint32_t>(key - KEY_LEFT) < 4 && !ClientIsAltKeyDown()) {
        return false;
    }
    return true;
}

// ref: FUN_1424b44e0
bool CSimpleEditBox::OnChar(int32_t key) {
    if (g_editBoxFocus == nullptr && (this->m_editFlags & EDIT_AUTO_FOCUS)) {
        this->SetFocus(true);
    }
    if (this != g_editBoxFocus) {
        return false;
    }
    this->m_imeComposing = false;
    this->InsertChar(static_cast<uint32_t>(key), true);
    this->m_imeStart = 0;
    this->m_imeEnd = 0;
    return true;
}

// ref: FUN_1424b45a0
bool CSimpleEditBox::Unknown150(void* event) {
    if (!(this->m_layoutState & LAYOUT_VISIBLE)) {
        return false;
    }
    if (g_editBoxFocus == nullptr && (this->m_editFlags & EDIT_AUTO_FOCUS)) {
        this->SetFocus(true);
    }
    if (this != g_editBoxFocus) {
        return false;
    }
    if (this->m_editFlags & EDIT_PASSWORD) {
        return true;
    }
    auto ime = static_cast<const int32_t*>(event);
    uint8_t flags = static_cast<uint8_t>(ime[2]);
    bool page = false;
    switch (ime[0]) {
        case 0: {
            int32_t language = this->m_inputLanguage;
            switch (OsImeGetLanguage()) {
                case 0:
                    language = 0;
                    break;
                case 1:
                    language = 3;
                    break;
                case 2:
                    language = 1;
                    break;
                case 3:
                    language = 2;
                    break;
            }
            if (language != this->m_inputLanguage) {
                this->m_inputLanguage = language;
                this->RunScript(SCRIPT_ONINPUTLANGUAGECHANGED, nullptr, s_inputLanguages[language]);
            }
            break;
        }
        case 1: {
            this->m_imeComposing = true;
            this->m_dragTime = 0;
            if (this->m_highlightStart != this->m_highlightEnd) {
                this->DeleteRange(this->m_highlightStart, this->m_highlightEnd, false);
            }
            int32_t cursor = this->m_cursorPos;
            this->m_highlightEnd = cursor;
            this->m_highlightStart = cursor;
            this->m_imeEnd = cursor;
            this->m_imeStart = cursor;
            break;
        }
        case 2: {
            char buffer[0x200];
            if (flags & 4) {
                OsImeGetResultString(buffer, sizeof(buffer));
                InsertFiltered(this, buffer);
            }
            if (flags & 2) {
                OsImeGetCompositionString(buffer, sizeof(buffer));
                this->Insert(buffer, nullptr, false, true, false, true);
                this->RunScript(SCRIPT_ONCHARCOMPOSITION, nullptr, static_cast<const char*>(buffer));
            }
            if (flags & 1) {
                this->UpdateImeComposition();
            }
            break;
        }
        case 3:
        case 4:
            if (this->UpdateCandidates(false) == 0) {
                this->HideCandidates();
            } else {
                this->ShowCandidates();
            }
            break;
        case 5:
        case 8:
            this->HideCandidates();
            break;
        case 6:
            this->HideCandidates();
            this->Insert("", nullptr, false, true, false, true);
            this->m_imeComposing = false;
            this->m_imeStart = 0;
            this->m_imeEnd = 0;
            break;
        case 7:
            page = true;
            if (this->UpdateCandidates(page) == 0) {
                this->HideCandidates();
            } else {
                this->ShowCandidates();
            }
            break;
    }
    return true;
}

// ref: FUN_1424b49b0
bool CSimpleEditBox::OnKeyDown(void* event) {
    if (g_editBoxFocus == nullptr && (this->m_editFlags & EDIT_AUTO_FOCUS)) {
        this->SetFocus(true);
    }
    if (this != g_editBoxFocus) {
        return false;
    }
    this->Update(true);
    int32_t key = *static_cast<const int32_t*>(event);
    CSimpleTop* top = CSimpleTop::s_instance;
    if (this->HasScriptAny(SCRIPT_ONKEYDOWN)) {
        const char* name = KeyToString(key);
        // FUN_14060f6a0 / FUN_14060e0c0
        top->m_inMouseScript = 1;
        this->RunScript(SCRIPT_ONKEYDOWN, nullptr, name);
        top->m_inMouseScript = 0;
    }
    bool control = false;
    bool shift = false;
    switch (key) {
        case 'A':
        case 'a':
            if (ClientIsControlKeyDown()) {
                this->HighlightText(0, -1);
            }
            break;
        case 'B':
        case 'b':
            if (ClientIsControlKeyDown()) {
                this->MoveLeft(ClientIsShiftKeyDown());
            }
            break;
        case 'C':
        case 'X':
        case 'c':
        case 'x':
            if (ClientIsControlKeyDown() && this->CopyHighlight() && ((key - 'X') & ~0x20) == 0) {
                this->DeleteHighlight();
            }
            break;
        case 'D':
        case 'd':
            if (ClientIsControlKeyDown()) {
                this->DeleteForward(false);
            }
            break;
        case 'F':
        case 'f':
            if (ClientIsControlKeyDown()) {
                this->MoveRight(ClientIsShiftKeyDown());
            }
            break;
        case 'K':
        case 'k':
            if (ClientIsControlKeyDown()) {
                int32_t letters = this->CountLetters(this->m_cursorPos, this->m_textLength - this->m_cursorPos);
                if (letters != 0) {
                    this->DeleteChars(letters, false);
                }
            }
            break;
        case 'N':
        case 'n':
            if (ClientIsControlKeyDown()) {
                this->HistoryNext();
            }
            break;
        case 'P':
        case 'p':
            if (ClientIsControlKeyDown()) {
                this->HistoryPrevious();
            }
            break;
        case 'U':
        case 'u':
            if (ClientIsControlKeyDown()) {
                int32_t letters = this->CountLetters(0, this->m_cursorPos);
                if (letters != 0) {
                    this->DeleteChars(-letters, false);
                }
            }
            break;
        case 'V':
        case 'v':
            if (ClientIsControlKeyDown()) {
                this->Paste();
            }
            break;
        case 'W':
        case 'w':
            if (ClientIsControlKeyDown()) {
                this->BackspaceWord(false);
            }
            break;
        case KEY_ESCAPE:
            this->RunScript(SCRIPT_ONESCAPEPRESSED, nullptr);
            this->Notify(this->m_escapeListener);
            break;
        case KEY_ENTER:
            if (!(this->m_editFlags & EDIT_MULTI_LINE) || this->HasScriptAny(SCRIPT_ONENTERPRESSED)) {
                top->m_inMouseScript = 1;
                this->RunScript(SCRIPT_ONENTERPRESSED, nullptr);
                this->Notify(this->m_enterListener);
                top->m_inMouseScript = 0;
            } else {
                this->Insert(s_newline, nullptr, true, false, false, false);
            }
            break;
        case KEY_BACKSPACE:
            if (!ClientIsControlKeyDown()) {
                this->Backspace(false);
            } else {
                this->BackspaceWord(false);
            }
            break;
        case KEY_TAB:
            this->RunScript(SCRIPT_ONTABPRESSED, nullptr);
            this->Notify(this->m_tabListener);
            break;
        case KEY_LEFT:
            if (!ClientIsControlKeyDown()) {
                this->MoveLeft(ClientIsShiftKeyDown());
            } else {
                this->MoveWordLeft(ClientIsShiftKeyDown());
            }
            this->RunScript(SCRIPT_ONARROWPRESSED, nullptr, "LEFT");
            break;
        case KEY_UP:
            if (!(this->m_editFlags & EDIT_MULTI_LINE)) {
                if (!ClientIsAltKeyDown()) {
                    this->RunScript(SCRIPT_ONARROWPRESSED, nullptr, "UP");
                } else {
                    this->HistoryPrevious();
                }
            } else if (!(this->m_dirtyFlags & DIRTY_TEXT)) {
                this->MoveLines(-1, ClientIsShiftKeyDown());
            }
            break;
        case KEY_RIGHT:
            control = ClientIsControlKeyDown();
            shift = ClientIsShiftKeyDown();
            if (!control) {
                if (this->m_cursorPos < this->m_textLength) {
                    this->MoveCursor(1, shift);
                }
            } else {
                while (this->m_cursorPos < this->m_textLength) {
                    int32_t bytes = 0;
                    uint32_t code = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(this->m_text + this->m_cursorPos), &bytes);
                    if (CharIsSpace(code)) {
                        break;
                    }
                    this->MoveCursor(1, shift);
                }
                while (this->m_cursorPos < this->m_textLength) {
                    int32_t bytes = 0;
                    uint32_t code = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(this->m_text + this->m_cursorPos), &bytes);
                    if (!CharIsSpace(code)) {
                        break;
                    }
                    this->MoveCursor(1, shift);
                }
            }
            if (!shift) {
                this->DropHighlight();
            }
            this->RunScript(SCRIPT_ONARROWPRESSED, nullptr, "RIGHT");
            break;
        case KEY_DOWN:
            if (!(this->m_editFlags & EDIT_MULTI_LINE)) {
                if (!ClientIsAltKeyDown()) {
                    this->RunScript(SCRIPT_ONARROWPRESSED, nullptr, "DOWN");
                } else {
                    this->HistoryNext();
                }
            } else if (!(this->m_dirtyFlags & DIRTY_TEXT)) {
                this->MoveLines(1, ClientIsShiftKeyDown());
            }
            break;
        case KEY_INSERT:
            if (ClientIsControlKeyDown()) {
                this->CopyHighlight();
                break;
            }
            if (ClientIsShiftKeyDown()) {
                this->Paste();
            }
            break;
        case KEY_DELETE: {
            int32_t length = this->m_textLength;
            int32_t start;
            int32_t end;
            if (!ClientIsShiftKeyDown()) {
                start = this->m_highlightStart;
                end = this->m_highlightEnd;
                if (!ClientIsControlKeyDown()) {
                    if (start == end) {
                        if (this->m_cursorPos < length) {
                            this->DeleteChars(1, false);
                        }
                        break;
                    }
                } else if (start == end) {
                    while (this->m_cursorPos < this->m_textLength) {
                        int32_t bytes = 0;
                        uint32_t code = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(this->m_text + this->m_cursorPos), &bytes);
                        if (!CharIsSpace(code)) {
                            break;
                        }
                        this->DeleteChars(1, false);
                    }
                    while (this->m_cursorPos < this->m_textLength) {
                        int32_t bytes = 0;
                        uint32_t code = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(this->m_text + this->m_cursorPos), &bytes);
                        if (CharIsSpace(code)) {
                            break;
                        }
                        this->DeleteChars(1, false);
                    }
                    break;
                }
            } else {
                if (!this->CopyHighlight()) {
                    break;
                }
                length = this->m_textLength;
                start = this->m_highlightStart;
                end = this->m_highlightEnd;
            }
            start = start < 1 ? 0 : start;
            start = start < length ? start : length;
            end = length <= end ? length : end;
            end = 0 < end ? end : 0;
            this->m_highlightStart = start;
            this->m_highlightEnd = end;
            this->DeleteRange(start, end, false);
            break;
        }
        case KEY_HOME:
            if (!ClientIsControlKeyDown()) {
                this->MoveLineStart(ClientIsShiftKeyDown());
            } else {
                this->MoveToStart(ClientIsShiftKeyDown());
            }
            break;
        case KEY_END:
            if (!ClientIsControlKeyDown()) {
                this->MoveLineEnd(ClientIsShiftKeyDown());
            } else {
                this->MoveToEnd(ClientIsShiftKeyDown());
            }
            break;
    }
    if ((this->m_frameFlags & FRAME_PROPAGATE_KEYBOARD) && !this->IsProtected()) {
        return false;
    }
    return true;
}

// ref: FUN_1424b52d0
bool CSimpleEditBox::Unknown168(void* event) {
    if (!(this->m_layoutState & LAYOUT_VISIBLE)) {
        return false;
    }
    if (g_editBoxFocus == nullptr && (this->m_editFlags & EDIT_AUTO_FOCUS)) {
        this->SetFocus(true);
    }
    if (this != g_editBoxFocus) {
        return false;
    }
    return this->OnKeyDown(event);
}

// ref: FUN_1424b5340
bool CSimpleEditBox::OnKeyUp(void* event) {
    if (!(this->m_layoutState & LAYOUT_VISIBLE)) {
        return false;
    }
    if (g_editBoxFocus == nullptr && (this->m_editFlags & EDIT_AUTO_FOCUS)) {
        this->SetFocus(true);
    }
    if (this != g_editBoxFocus) {
        return false;
    }
    if (this->HasScriptAny(SCRIPT_ONKEYUP)) {
        const char* name = KeyToString(*static_cast<const int32_t*>(event));
        CSimpleTop* top = CSimpleTop::s_instance;
        top->m_inMouseScript = 1;
        this->RunScript(SCRIPT_ONKEYUP, nullptr, name);
        top->m_inMouseScript = 0;
    }
    if ((this->m_frameFlags & FRAME_PROPAGATE_KEYBOARD) && !this->IsProtected()) {
        return false;
    }
    return true;
}

// ref: FUN_1424b5400
bool CSimpleEditBox::OnMouseDown(MOUSEEVENT* event, const char* button) {
    if (CSimpleTop::s_instance->m_inputSuspended != 0) {
        return false;
    }
    bool handled = this->CSimpleFrame::OnMouseDown(event, button);
    if (handled || (this->m_frameFlags & FRAME_DISABLED)) {
        return handled;
    }
    this->Update(this->m_imeComposing);
    if (!this->m_imeComposing) {
        int32_t index = 0;
        int32_t side = 0;
        if (this->GetIndexAtPoint(event->m_x, event->m_y, &index, &side)) {
            this->DropHighlight();
            this->SetCursorPosition(this->SkipHidden(index));
            this->m_highlightEnd = this->m_cursorPos;
            this->m_highlightStart = this->m_cursorPos;
            if (this->m_dragTime == 0) {
                this->m_dragTime = FrameTime_GetTime() + 200;
            }
            handled = true;
        }
    }
    this->SetFocus(true);
    return handled;
}

// ref: FUN_1424b5510
bool CSimpleEditBox::OnMouseUp(MOUSEEVENT* event, const char* button) {
    if (this->m_dragTime != 0) {
        this->m_dragTime = 0;
    }
    if (CSimpleTop::s_instance->m_inputSuspended != 0) {
        return false;
    }
    return this->CSimpleFrame::OnMouseUp(event, button);
}

// ref: FUN_1424b5580
bool CSimpleEditBox::OnMouseMove(MOUSEEVENT* event) {
    bool handled = this->CSimpleFrame::OnMouseMove(event);
    if (this->m_dragTime != 0) {
        this->m_dirtyFlags |= DIRTY_DRAG;
    }
    return handled;
}

// ref: FUN_1424b5540
void CSimpleEditBox::QueueOnShow() {
    this->CSimpleFrame::QueueOnShow();
    if (g_editBoxFocus == nullptr && (this->m_editFlags & EDIT_AUTO_FOCUS)) {
        this->SetFocus(false);
    }
}

// ref: FUN_1424b4550
void CSimpleEditBox::QueueOnHide() {
    this->CSimpleFrame::QueueOnHide();
    if (g_editBoxFocus == this) {
        g_editBoxFocus = nullptr;
        this->OnEditFocusChanged(false, true, false);
    }
}

// ref: FUN_1424b40a0
void CSimpleEditBox::Unknown280() {
    this->Unknown2C0();
    if (this->m_highlightTexture != nullptr) {
        TextureFree(&this->m_highlightTexture);
    }
    this->m_candidateFrame = nullptr;
    if (g_editBoxFocus == this) {
        g_editBoxFocus = nullptr;
        this->OnEditFocusChanged(false, false, false);
    }
    if (this->m_font != nullptr) {
        delete this->m_font;
    }
    SMemFree(this->m_text, "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Frame\\CSimpleEditBox.cpp", 0x8c, 0);
    SMemFree(this->m_textInfo, "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Frame\\CSimpleEditBox.cpp", 0x8d, 0);
    SMemFree(this->m_passwordText, "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Frame\\CSimpleEditBox.cpp", 0x8e, 0);
    if (this->m_cursor != nullptr) {
        delete this->m_cursor;
    }
}

// ref: FUN_1424b55b0
void CSimpleEditBox::RunOnUpdate(float elapsed) {
    this->CSimpleFrame::RunOnUpdate(elapsed);
    this->Update(true);
    // FUN_14228a090: the foreground limit flag, always set.
    if (this->m_blinkSpeed != 0.0f && this->m_visibleStart <= this->m_cursorPos
        && this->m_cursorPos <= this->m_visibleStart + this->m_visibleLength && this == g_editBoxFocus) {
        this->m_blinkElapsed += elapsed;
        if (this->m_blinkElapsed <= this->m_blinkSpeed) {
            return;
        }
        bool shown = this->m_cursor->GetAlpha() != 0;
        this->m_cursor->Show();
        this->m_cursor->SetAlpha(shown ? 0 : 0xff);
        this->m_blinkElapsed = 0.0f;
        return;
    }
    this->m_cursor->Hide();
}

bool CSimpleEditBox::WantsUpdate() {
    return true;
}

// ---- the IME -----------------------------------------------------------------------------------

// ref: FUN_1424b1860
const char* CSimpleEditBox::GetInputLanguage() {
    return s_inputLanguages[this->m_inputLanguage];
}

// ref: FUN_1424b67d0
void CSimpleEditBox::ToggleInputLanguage() {
    OsImeSetNative(this->m_inputLanguage == 0);
}

// ref: FUN_1424b67e0
void CSimpleEditBox::UpdateImeComposition() {
    int32_t start = 0;
    int32_t end = 0;
    int32_t cursor = 0;
    if (!OsImeGetCompositionRange(&start, &end, &cursor)) {
        this->m_imeEnd = this->m_cursorPos;
        this->m_imeStart = this->m_cursorPos;
        return;
    }
    int32_t base = this->m_highlightStart;
    int32_t limit = this->m_highlightEnd;
    int32_t at = base + Walk(this->m_textInfo, base, start, false);
    this->m_imeStart = limit < at ? limit : at;
    at = base + Walk(this->m_textInfo, base, end, false);
    this->m_imeEnd = limit < at ? limit : at;
    at = base + Walk(this->m_textInfo, base, cursor, false);
    this->m_cursorPos = limit < at ? limit : at;
    this->CreateImeHighlight();
    this->m_dirtyFlags |= DIRTY_HIGHLIGHT | DIRTY_CURSOR;
    if (CSimpleTop::s_instance != nullptr) {
        this->m_cursorContext = CSimpleTop::s_instance->m_inputPhase;
    }
}

// ref: FUN_1424b1830
float CSimpleEditBox::GetIMEFontHeight() {
    return this->m_string->GetFontHeight(true);
}

// ref: FUN_1424b1840
float CSimpleEditBox::GetIMELayoutScale() {
    return this->m_layoutScale;
}

// ---- the font object -----------------------------------------------------------------------

// ref: FUN_1424b1480
void CSimpleEditBox::FontObjectChanged(CSimpleFontedFrameFont* font, uint16_t flags) {
    (void)font;
    if (flags & (CSimpleFontAttributes::CHANGED_FONT | CSimpleFontAttributes::CHANGED_SPACING)) {
        this->UpdateSizes();
    }
    if (flags & CSimpleFontAttributes::CHANGED_COLOR) {
        this->m_cursor->SetColorTexture(&this->m_font->m_attributes.m_color);
    }
}
