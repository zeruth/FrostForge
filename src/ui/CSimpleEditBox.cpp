#include "ui/CSimpleEditBox.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "base/CharClass.hpp"
#include "base/DataAllocator.hpp"
#include "base/Status.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "framescript/Taint.hpp"
#include "gxu/GxuFont.hpp"
#include "os/Window.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "storm/Unicode.hpp"
#include "texture/Texture.hpp"
#include "ui/BinaryXML.hpp"
#include "ui/CSimpleEditBoxText.hpp"
#include "ui/CSimpleFontString.hpp"
#include "ui/CSimpleTexture.hpp"
#include "ui/CSimpleTop.hpp"
#include "ui/FrameXML.hpp"
#include "ui/UITypes.hpp"

extern "C" {
#include "lauxlib.h"
#include "lblizzard.h"
#include "lua.h"
}

using namespace EditBoxText;

// The edit box holding the keyboard focus (0x142e97b30).
extern CSimpleFrame* g_editBoxFocus;

int32_t CSimpleEditBox::s_metaTable = -2;
int32_t CSimpleEditBox::s_restrictedMetaTable = -2;

namespace {

const char s_editBoxFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Frame\\CSimpleEditBox.cpp";
const char s_editBoxHeader[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Frame\\CSimpleEditBox.h";

// The edit boxes' pool (0x1447aedb0, made by the static initializer 0x14020dbf0).
CDataAllocator s_editBoxHeap(0x488, 5);

// The frame flag a disabled edit box carries.
constexpr uint32_t FRAME_DISABLED = 0x8;

// The scripts of an edit box.
enum : uint8_t {
    SCRIPT_ONCHAR = 0x16,
    SCRIPT_ONENABLE = 0x21,
    SCRIPT_ONDISABLE = 0x22,
    SCRIPT_ONSPACEPRESSED = 0x3e,
    SCRIPT_ONTEXTSET = 0x3f,
};

// The smallest change of a font setting that counts (DAT_14261a954).
constexpr float FONT_EPSILON = 2.3841858e-07f;

const char s_illegal[] = "Call is illegal when disabled by security settings.";

// What a tab is typed as (DAT_142845ab0), and a typed bar (DAT_142624428).
const char s_tabText[] = "    ";
const char s_barText[] = "||";

// A font object setting changed: marks it, keeps it from being taken from the font object,
// and tells the fontables.
void FontChanged(CSimpleFontedFrameFont* font, uint16_t changed) {
    font->m_fontableFlags &= ~changed;
    font->UpdateFontables();
}

// The font object's string flags, marked as set.
void SetFontStringFlags(CSimpleFontedFrameFont* font, uint32_t flags) {
    CSimpleFontAttributes& attributes = font->m_attributes;
    if (flags != attributes.m_stringFlags || !(attributes.m_flags & CSimpleFontAttributes::HAS_FLAGS)) {
        attributes.m_stringFlags = flags;
        attributes.m_flags |= CSimpleFontAttributes::HAS_FLAGS | CSimpleFontAttributes::CHANGED_FLAGS;
    }
}

// A font string's flag the font object no longer decides, set or cleared.
void SetOwnStringFlag(CSimpleFontString* string, uint32_t flag, bool on) {
    string->m_inheritedFlags &= ~flag;
    string->SetStringFlag(flag, on);
}

// A packed colour from three bytes and an optional alpha.
uint32_t PackColor(const uint32_t* rgb, const uint8_t* alpha) {
    uint32_t a = alpha[1] != 0 ? alpha[0] : 0xff;
    return (a << 24) | (*rgb & 0xffffff);
}

}

// ---- construction ----------------------------------------------------------------------------

// ref: FUN_1424af1c0
CSimpleEditBox::CSimpleEditBox(CSimpleFrame* parent) : CSimpleFrame(parent) {
    this->m_editFlags = EDIT_AUTO_FOCUS;
    this->m_dirtyFlags = DIRTY_TEXT | DIRTY_LINES;
    this->m_cursorContext = 0;
    this->m_textTaint = nullptr;
    this->m_passwordText = nullptr;
    this->m_textLength = 0;
    this->m_maxBytes = -1;
    this->m_maxLetters = 0;
    this->m_maxMarkupBytes = 0;
    this->m_visibleTextByteLimit = 0;
    this->m_textCapacity = 0x20;
    this->m_visibleStart = 0;
    this->m_visibleLength = 0;
    this->m_highlights = nullptr;
    this->m_highlightCount = 0;
    this->m_highlightCapacity = 0;
    this->m_highlightTexture = nullptr;
    this->m_highlightColor = 0;
    this->m_highlightStart = 0;
    this->m_highlightEnd = 0;
    this->m_dragTime = 0;
    this->m_cursor = nullptr;
    this->m_cursorPos = 0;
    this->m_blinkSpeed = 0.5f;
    this->m_blinkElapsed = 0.0f;
    this->m_lineCount = 0;
    this->m_lineStarts = nullptr;
    this->m_lineCapacity = 0;
    this->m_lineGranularity = 0;
    this->m_historyLines = 0;
    this->m_historyPos = 0;
    this->m_historyCount = 0;
    this->m_history = nullptr;
    this->m_historyCapacity = 0;
    this->m_enterListener = {};
    this->m_escapeListener = {};
    this->m_spaceListener = {};
    this->m_tabListener = {};
    this->m_textChangedListener = {};
    this->m_textSetListener = {};
    this->m_imeComposing = false;
    this->m_inputLanguage = 0;
    this->m_imeHighlight = nullptr;
    this->m_imeStart = 0;
    this->m_imeEnd = 0;
    this->m_textInsets[0] = 0.0f;
    this->m_textInsets[1] = 0.0f;
    this->m_textInsets[2] = 0.0f;
    this->m_textInsets[3] = 0.0f;

    this->m_text = static_cast<char*>(SMemAlloc(0x20, 0x1f, s_editBoxFile, 0, 0x60, 0));
    this->m_text[0] = '\0';
    this->m_textInfo = static_cast<uint32_t*>(SMemAlloc(static_cast<uint64_t>(this->m_textCapacity) * 4, 0x1f, s_editBoxFile, 0, 0x62, 0));
    std::memset(this->m_textInfo, 0, static_cast<uint64_t>(this->m_textCapacity) * 4);

    void* memory = SMemAlloc(sizeof(CSimpleFontedFrameFont), 0x1f, s_editBoxFile, 0, 100, 0);
    this->m_font = new (memory) CSimpleFontedFrameFont(this);
    SetFontStringFlags(this->m_font, (this->m_font->m_attributes.m_stringFlags & ~6u) | 1);

    this->m_string = new CSimpleFontString(this, DRAWLAYER_ARTWORK, true);
    this->m_string->SetFontObject(this->m_font);
    this->SetMultiLine(false);

    this->m_highlightColor = 0xff606060;
    this->m_cursor = new CSimpleTexture(this, DRAWLAYER_OVERLAY, 0, true);
    this->m_cursor->Hide();
    uint32_t white = 0xffffffff;
    this->m_cursor->SetColorTexture(&white);

    this->EnableInputEvent(0);
    this->EnableInputEvent(1);
    this->SetMouseMotionEnabled(true);
    this->SetMouseClickEnabled(true);
    uint32_t solid = 0xffffffff;
    this->m_highlightTexture = TextureCreateSolid(&solid, nullptr, "", 0);
}

// ref: FUN_1424af6a0
CSimpleEditBox::~CSimpleEditBox() {
    // FUN_1424af5e0
    for (uint32_t i = 0; i < this->m_historyCount; i++) {
        SMemFree(this->m_history[i].m_text, s_editBoxHeader, 0x215, 0);
    }
    if (this->m_history != nullptr) {
        SMemFree(this->m_history, ".?AV?$TSFixedArray@UEditHistory@CSimpleEditBox@@@@", -2, 0);
    }
    if (this->m_lineStarts != nullptr) {
        SMemFree(this->m_lineStarts, ".?AV?$TSFixedArray@I@@", -2, 0);
    }
    if (static_cast<int64_t>(this->m_highlightCapacity) >= 0 && this->m_highlights != nullptr) {
        SMemFree(this->m_highlights, ".?AUHighlightTextureInfo@CSimpleEditBox@@", -6, 0);
        this->m_highlights = nullptr;
    }
}

void* CSimpleEditBox::operator new(size_t size) {
    (void)size;
    return s_editBoxHeap.GetData(false, ".?AVCSimpleEditBox@@", -2, 0x1f);
}

void CSimpleEditBox::operator delete(void* editBox) {
    if (editBox != nullptr) {
        s_editBoxHeap.PutData(editBox);
    }
}

// DAT_14309febc
int32_t CSimpleEditBox::GetObjectType() {
    static int32_t s_type = ++FrameScript_Object::s_objectTypeCount;
    return s_type;
}

// ref: FUN_1424b0730
void CSimpleEditBox::CreateScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    s_metaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleEditBox_FillScriptMethods, false);
    s_restrictedMetaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleEditBox_FillScriptMethods, true);
}

// ref: FUN_1424b1360
void CSimpleEditBox::DestroyScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    luaL_unref(L, LUA_REGISTRYINDEX, s_metaTable);
    luaL_unref(L, LUA_REGISTRYINDEX, s_restrictedMetaTable);
    s_metaTable = -2;
    s_restrictedMetaTable = -2;
}

// ---- FrameScript_Object ----------------------------------------------------------------------

// ref: FUN_1424b1ab0
int32_t CSimpleEditBox::GetScriptMetaTable() {
    if (!(this->FrameScript_Object::m_flags & FSOBJ_FORBIDDEN) && !(this->FrameScript_Object::m_flags & FSOBJ_PROTECTED_METATABLE)) {
        return s_metaTable;
    }
    return s_restrictedMetaTable;
}

// ref: FUN_1424b6720
bool CSimpleEditBox::GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) {
    switch (index) {
        case 0x2c:
            data->m_wrapper = "return function(self, userInput) %s end";
            return true;
        case 0x23:
        case 0x24:
        case 0x29:
        case 0x2a:
        case 0x2b:
        case 0x3e:
        case 0x3f:
        case 0x41:
            return true;
        case 0x40:
            data->m_wrapper = "return function(self,language) %s end";
            return true;
        case 0x43:
            data->m_wrapper = "return function(self,x,y,w,h,context) %s end";
            return true;
        case 0x49:
            data->m_wrapper = "return function(self,key) %s end";
            return true;
        default:
            return this->CSimpleFrame::GetScriptData(index, data);
    }
}

// ref: FUN_1424b2560
bool CSimpleEditBox::IsObjectTypeName(const char* name) {
    return _stricmp(name, "EditBox") == 0 || _stricmp(name, "Frame") == 0 || _stricmp(name, "Region") == 0
        || _stricmp(name, "Object") == 0 || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_1424b24b0
bool CSimpleEditBox::IsObjectType(int32_t type) {
    if (type == CSimpleEditBox::GetObjectType() || type == CSimpleFrame::GetObjectType()) {
        return true;
    }
    return this->CScriptRegion::IsObjectType(type);
}

// ref: FUN_1424b1a40
const char* CSimpleEditBox::GetObjectTypeName() {
    return "EditBox";
}

// ref: FUN_1424b1930
uint32_t CSimpleEditBox::GetObjectSize() {
    return 0x488;
}

// ---- XML ----------------------------------------------------------------------------------------

// ref: FUN_1424b25e0
void CSimpleEditBox::LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) {
    this->CSimpleFrame::LoadXML(node, status, intrinsic);
    if (!(node->m_flags & 0x800)) {
        return;
    }
    auto xml = static_cast<XMLEDITBOX*>(node);
    if (xml->m_fontFlags & 0x80) {
        CSimpleFont* font = CSimpleFont::GetFont(xml->m_font.m_text, false);
        if (font == nullptr) {
            const char* name = this->GetName();
            if (name == nullptr) {
                name = this->GetObjectTypeName();
            }
            status->Add(1, "%s %s: Couldn't find font object named %s", this->GetObjectTypeName(), name, xml->m_font.m_text);
        } else {
            this->m_font->SetFontObject(font);
        }
    }
    uint32_t flags = xml->m_editFlags;
    if ((flags & 0xc00000) == 0xc00000) {
        this->m_editFlags |= EDIT_SECURITY_DISABLE_SET_TEXT;
    }
    if ((flags & 0x3000000) == 0x3000000) {
        this->m_editFlags |= EDIT_SECURITY_DISABLE_PASTE;
    }
    if (flags & 1) {
        this->m_maxBytes = xml->m_bytes < 1 ? -1 : xml->m_bytes - 1;
    }
    if (flags & 2) {
        int32_t limit = xml->m_visibleBytes < 0 ? 0 : xml->m_visibleBytes;
        if (limit != this->m_visibleTextByteLimit) {
            this->m_visibleTextByteLimit = limit;
            this->EnforceLimits(true, false);
        }
    }
    if (flags & 4) {
        this->m_maxLetters = xml->m_letters;
    }
    if (flags & 0x10) {
        this->m_maxMarkupBytes = xml->m_invisibleBytes < 0 ? 0 : xml->m_invisibleBytes;
    }
    if (flags & 8) {
        this->m_editFlags = (this->m_editFlags & ~EDIT_COUNT_INVISIBLE_LETTERS) | ((flags & 0x20) ? EDIT_COUNT_INVISIBLE_LETTERS : 0);
    }
    if (flags & 0x40) {
        this->m_blinkSpeed = xml->m_blinkSpeed;
    }
    if (flags & 0x80) {
        this->m_editFlags = (this->m_editFlags & ~EDIT_AUTO_FOCUS) | ((flags >> 8) & 1);
    }
    if ((flags >> 9) & 1) {
        this->SetMultiLine(((flags >> 10) & 1) != 0);
    }
    if ((flags >> 0xb) & 1) {
        this->SetNumeric(((flags >> 0xc) & 1) != 0);
    }
    if ((flags >> 0xd) & 1) {
        this->SetNumericFullRange(((flags >> 0xe) & 1) != 0);
    }
    if ((flags >> 0xf) & 1) {
        this->SetAlphabeticOnly(((flags >> 0x10) & 1) != 0);
    }
    if ((flags >> 0x11) & 1) {
        this->SetPassword(((flags >> 0x12) & 1) != 0);
    }
    if ((flags >> 0x13) & 1) {
        this->m_editFlags = (this->m_editFlags & ~EDIT_ALT_ARROW_KEY_MODE) | (((flags >> 0x14) & 1) ? EDIT_ALT_ARROW_KEY_MODE : 0);
    }
    if (flags & 0x200000) {
        if (0 < xml->m_historyLines) {
            this->SetHistoryLines(xml->m_historyLines);
        }
    }
    bool hadFontObject = this->m_font->m_fontObject != nullptr;
    for (XMLOBJECT* child = node->m_child; child != nullptr; child = child->m_next) {
        if (child->m_type == XMLTYPE_HIGHLIGHTCOLOR) {
            uint32_t color = 0;
            if (XMLColorGet(static_cast<XMLCOLOR*>(child), &color)) {
                this->m_highlightColor = color;
            }
        } else if (child->m_type == XMLTYPE_FONTSTRING) {
            if (this->m_font->m_fontObject != nullptr && !hadFontObject) {
                const char* name = this->GetName();
                if (name == nullptr) {
                    name = this->GetObjectTypeName();
                }
                status->Add(1, "%s: FontString element overriding font %s", name, this->m_font->m_fontObject->GetName());
            }
            hadFontObject = false;
            this->m_font->LoadXML(child, status);
            if (static_cast<XMLFONTSTRING*>(child)->m_fontStringFlags & 1) {
                const char* name = this->GetName();
                if (name == nullptr) {
                    name = this->GetObjectTypeName();
                }
                status->Add(1, "%s: Set bytes attribute on EditBox element instead of child FontString", name);
            }
        } else if (child->m_type == XMLTYPE_TEXTINSETS) {
            float left = 0.0f;
            float right = 0.0f;
            float top = 0.0f;
            float bottom = 0.0f;
            if (XMLInsetGet(static_cast<XMLINSET*>(child), &left, &right, &top, &bottom, status)) {
                this->m_textInsets[1] = left;
                this->m_textInsets[3] = right;
                this->m_textInsets[2] = top;
                this->m_textInsets[0] = bottom;
                this->UpdateSizes();
            }
        }
    }
}

// ---- small pieces -------------------------------------------------------------------------------

void CSimpleEditBox::Notify(LISTENER& listener) {
    if (listener.m_listener == nullptr) {
        return;
    }
    // The event's vtable is 0x14297d648.
    CSimpleEditBoxListener::EVENT event = { nullptr, 0, listener.m_id, this };
    listener.m_listener->OnEditBoxEvent(&event);
}

void CSimpleEditBox::DropHighlight() {
    if (this->m_highlightStart != this->m_highlightEnd) {
        this->m_dirtyFlags |= DIRTY_HIGHLIGHT;
        this->m_highlightStart = 0;
        this->m_highlightEnd = 0;
    }
}

void CSimpleEditBox::CursorMoved() {
    this->m_dirtyFlags |= DIRTY_CURSOR;
    if (CSimpleTop::s_instance != nullptr) {
        // FUN_1405ee7b0
        this->m_cursorContext = CSimpleTop::s_instance->m_inputPhase;
    }
}

char* CSimpleEditBox::DisplayBuffer() {
    return (this->m_editFlags & EDIT_PASSWORD) ? this->m_passwordText : this->m_text;
}

void CSimpleEditBoxListener::Unknown00() {
}

void CSimpleEditBoxListener::Unknown08() {
}

void CSimpleEditBoxListener::Unknown10() {
}

void CSimpleEditBoxListener::Unknown18() {
}

void CSimpleEditBoxListener::OnEditBoxEvent(EVENT* event) {
    (void)event;
}

// ---- the text ---------------------------------------------------------------------------------

// ref: FUN_1424b1b50
const char* CSimpleEditBox::GetText() {
    FrameScript_ReadTaint(FrameScript_GetContext(), this->m_textTaint);
    if ((this->m_editFlags & EDIT_SECURE_TEXT) && lua_taint != nullptr) {
        FrameScript_ActionError(s_illegal);
        return nullptr;
    }
    return this->m_text;
}

// ref: FUN_1424b1c20
bool CSimpleEditBox::HasText() {
    return this->m_text != nullptr && *this->m_text != '\0';
}

// ref: FUN_1424b64e0
void CSimpleEditBox::SetText(const char* text, const char* taint, bool force, bool always) {
    if ((this->m_editFlags & EDIT_SECURITY_DISABLE_SET_TEXT) && !force) {
        FrameScript_ActionError(s_illegal);
        return;
    }
    this->DropHighlight();
    if (std::strcmp(text, this->m_text) == 0 && !always) {
        return;
    }
    this->DeleteRange(0, this->m_textLength, true);
    this->m_visibleStart = 0;
    this->Insert(text, taint, false, false, true, force);
    // FUN_140603620
    this->RunScript(SCRIPT_ONTEXTSET, taint);
    this->Notify(this->m_textSetListener);
}

// ref: FUN_1424b66d0
void CSimpleEditBox::SetTextScript(const char* text) {
    this->SetText(text, lua_taint, false, false);
}

// ref: FUN_1424b2460
void CSimpleEditBox::InsertScript(const char* text) {
    if (this->m_editFlags & EDIT_SECURITY_DISABLE_SET_TEXT) {
        FrameScript_ActionError(s_illegal);
        return;
    }
    this->Insert(text, lua_taint, true, false, false, false);
}

// ref: FUN_1424b1d10
void CSimpleEditBox::InsertChar(uint32_t code, bool force) {
    if ((this->m_editFlags & EDIT_SECURITY_DISABLE_SET_TEXT) && !force) {
        return;
    }
    char buffer[8];
    const char* text;
    if (code < 0x20 || code == 0x7f) {
        if (code == 9) {
            this->Insert(s_tabText, nullptr, true, false, false, force);
            return;
        }
        if (!(this->m_editFlags & EDIT_MULTI_LINE) || code != 10) {
            return;
        }
    }
    if (code == '|') {
        text = s_barText;
    } else {
        uint8_t* end = SUniSPutUTF8(code, reinterpret_cast<uint8_t*>(buffer));
        *end = 0;
        text = buffer;
    }
    this->Insert(text, nullptr, true, false, false, force);
}

// ref: FUN_1424b1db0
void CSimpleEditBox::Insert(const char* text, const char* taint, bool runOnChar, bool raw, bool scripted, bool force) {
    if ((this->m_editFlags & EDIT_SECURITY_DISABLE_SET_TEXT) && !force) {
        return;
    }
    // Nothing goes inside a hyperlink or a texture.
    int32_t cursor = this->m_cursorPos;
    if ((this->m_textInfo[cursor] & 0xff000000) && 0 < cursor) {
        int32_t previous = cursor;
        do {
            previous--;
        } while (0 <= previous && this->m_textInfo[previous] == 0);
        if (this->m_textInfo[previous < 0 ? 0 : previous] & 0xff000000) {
            return;
        }
    }
    if (this->m_highlightStart != this->m_highlightEnd) {
        int32_t length = this->m_textLength;
        int32_t start = this->m_highlightStart;
        int32_t clampedStart = start < 1 ? 0 : start;
        int32_t startIndex = length;
        if (clampedStart < length) {
            startIndex = start < 1 ? 0 : start;
        }
        this->m_highlightStart = startIndex;
        int32_t end = this->m_highlightEnd;
        int32_t endIndex = end < length ? end : length;
        int32_t finalEnd = 0;
        if (0 < endIndex) {
            finalEnd = end < length ? end : length;
        }
        this->m_highlightEnd = finalEnd;
        this->DeleteRange(startIndex, finalEnd, scripted);
    }
    if (text != nullptr && TextIsValid(text)) {
        // Hyperlink markers cannot be typed in.
        for (const char* p = text; *p != '\0'; p++) {
            if (*p == '|') {
                p++;
                if (*p == '\0') {
                    break;
                }
                if (((*p + 0xb5) & 0xdf) == 0) {
                    text = "";
                    break;
                }
            }
        }
    } else {
        text = "";
    }
    int32_t added = static_cast<int32_t>(std::strlen(text));
    if ((this->m_editFlags & EDIT_ALPHABETIC_ONLY) && !raw) {
        for (const char* p = text; *p != '\0';) {
            int32_t bytes = 0;
            uint32_t code = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(p), &bytes);
            if (!CharIsAlphabetic(code)) {
                return;
            }
            p += bytes;
        }
    }
    if ((this->m_editFlags & (EDIT_NUMERIC | EDIT_NUMERIC_FULL_RANGE)) && !raw) {
        for (const char* p = text; *p != '\0';) {
            int32_t bytes = 0;
            uint32_t code = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(p), &bytes);
            if (9 < code - 0x30) {
                if (!(this->m_editFlags & EDIT_NUMERIC_FULL_RANGE)) {
                    return;
                }
                if (1 < code - 0x2d) {
                    return;
                }
            }
            p += bytes;
        }
        if ((this->m_editFlags & EDIT_NUMERIC_FULL_RANGE) && added != 0) {
            uint64_t total = static_cast<uint64_t>(this->m_textLength) + static_cast<uint64_t>(added);
            int64_t at = this->m_cursorPos;
            if (at < 0 || this->m_textLength < at) {
                return;
            }
            uint64_t suffix = static_cast<uint64_t>(this->m_textLength - at);
            if (total - suffix - at != static_cast<uint64_t>(added)) {
                return;
            }
            char* combined = static_cast<char*>(_alloca((total + 0x10) & ~0xfull));
            combined[0] = '\0';
            std::memcpy(combined, this->m_text, static_cast<size_t>(at));
            std::memcpy(combined + at, text, static_cast<size_t>(added));
            std::memcpy(combined + at + added, this->m_text + at, static_cast<size_t>(suffix));
            combined[total] = '\0';
            // FUN_142363630
            char* parsed = nullptr;
            std::strtod(combined, &parsed);
            bool failed = errno == ERANGE;
            errno = 0;
            if (static_cast<uint64_t>(parsed - combined) != total || failed) {
                if (1 < total || *combined != '-') {
                    return;
                }
            }
        }
    }
    int32_t needed = this->m_textLength + 1 + added;
    if (this->m_textCapacity < needed) {
        int32_t capacity = (needed + 0x1f) & ~0x1f;
        this->m_textCapacity = capacity;
        this->m_text = static_cast<char*>(SMemReAlloc(this->m_text, static_cast<uint64_t>(capacity), 0x1f, s_editBoxFile, 0, 0x5c7, 0));
        this->m_textInfo = static_cast<uint32_t*>(
            SMemReAlloc(this->m_textInfo, static_cast<uint64_t>(this->m_textCapacity) * 4, 0x1f, s_editBoxFile, 0, 0x5c8, 0));
    }
    int32_t at = this->m_cursorPos;
    char* insertAt = this->m_text + at;
    if (at < this->m_textLength) {
        std::memmove(insertAt + added, insertAt, static_cast<size_t>(this->m_textLength - at));
    }
    std::memmove(insertAt, text, static_cast<size_t>(added));
    this->m_textLength += added;
    this->m_text[this->m_textLength] = '\0';
    int32_t start = this->m_cursorPos;
    this->m_cursorPos = start + added;
    std::memset(this->m_textInfo + start, 0, static_cast<size_t>(this->m_textCapacity - start) * 4);
    uint32_t inside = 0;
    int32_t index = start;
    for (const char* p = this->m_text + start; p != nullptr && *p != '\0';) {
        int32_t advance = 0;
        uint32_t code = 0;
        int32_t kind = GxuDetermineControlCode(p, &advance, nullptr, 0, &code);
        if (kind == 5) {
            inside |= INFO_IN_LINK;
        } else if (kind == 9) {
            inside |= INFO_IN_TEXTURE;
        }
        this->m_textInfo[index] = (static_cast<uint32_t>(kind) << 16) | (static_cast<uint32_t>(advance) & 0xffff) | inside;
        index += advance;
        p += advance;
        if (kind == 6) {
            inside &= ~INFO_IN_LINK;
        } else if (kind == 10) {
            inside &= ~INFO_IN_TEXTURE;
        }
    }
    this->m_dirtyFlags |= DIRTY_TEXT | DIRTY_CURSOR | DIRTY_LINES;
    if (CSimpleTop::s_instance != nullptr) {
        this->m_cursorContext = CSimpleTop::s_instance->m_inputPhase;
    }
    if (scripted) {
        this->m_dirtyFlags &= ~DIRTY_USER_INPUT;
    } else {
        this->m_dirtyFlags |= DIRTY_USER_INPUT;
    }
    if (!this->EnforceLimits(false, scripted)) {
        this->EnforceLimits(true, scripted);
    }
    if (raw) {
        this->HighlightText(start, this->m_cursorPos);
    }
    if (this->m_textTaint == nullptr && 0 < this->m_textLength) {
        this->m_textTaint = taint;
    }
    if (!runOnChar) {
        return;
    }
    // FUN_140603870
    this->RunScript(SCRIPT_ONCHAR, nullptr, text);
    const char* p = text;
    int32_t bytes = 0;
    uint32_t code = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(p), &bytes);
    while (code != 0xffffffff) {
        if (code == ' ') {
            this->RunScript(SCRIPT_ONSPACEPRESSED, nullptr);
            this->Notify(this->m_spaceListener);
            return;
        }
        p += bytes;
        code = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(p), &bytes);
    }
}

// ref: FUN_1424b0fa0
bool CSimpleEditBox::DeleteRange(int32_t start, int32_t end, bool scripted) {
    uint32_t* info = this->m_textInfo;
    // A start inside a hyperlink or a texture goes back to the codes that open it.
    if (info[start] & 0xff000000) {
        int32_t at = start;
        int32_t last = start;
        bool found = false;
        while (0 <= at) {
            if (IsOpen(Kind(info[at])) && Kind(info[at]) < 0xe) {
                found = true;
                break;
            }
            last = at;
            do {
                at--;
            } while (0 <= at && info[at] == 0);
        }
        if (!found) {
            start = last;
        } else {
            int32_t first = at;
            while (0 <= at && Kind(info[at]) < 0xe && IsOpen(Kind(info[at]))) {
                first = at;
                do {
                    at--;
                } while (0 <= at && info[at] == 0);
            }
            start = first;
        }
    }
    // An end inside one goes past the codes that close it.
    if (info[end] & 0xff000000) {
        int32_t length = this->m_textLength;
        while (end < length) {
            uint8_t kind = Kind(info[end]);
            if (kind == 2 || (static_cast<uint8_t>(kind - 6) < 9 && ((kind - 6) & 1) == 0)) {
                while (end < length && IsClose(Kind(info[end]))) {
                    end += static_cast<int32_t>(Length(info[end]));
                }
                break;
            }
            end += static_cast<int32_t>(Length(info[end]));
        }
    }
    int32_t count = end - start;
    this->m_cursorPos = start;
    int32_t remaining = this->m_textLength - count;
    if (remaining < 0) {
        return false;
    }
    this->m_textLength = remaining;
    char* from = this->m_text + start;
    std::memmove(from, from + count, static_cast<size_t>(remaining - start));
    std::memmove(info + start, info + start + count, static_cast<size_t>(this->m_textLength - this->m_cursorPos) * 4);
    this->m_text[this->m_textLength] = '\0';
    this->m_textInfo[this->m_textLength] = 0;
    this->DropHighlight();
    this->m_dirtyFlags |= DIRTY_TEXT | DIRTY_LINES;
    this->CursorMoved();
    if (!scripted) {
        this->m_dirtyFlags |= DIRTY_USER_INPUT;
    }
    if (this->m_textLength == 0) {
        this->m_textTaint = nullptr;
    }
    return 0 < count;
}

// ref: FUN_1424afee0
void CSimpleEditBox::ClearText() {
    this->DropHighlight();
    this->DeleteRange(0, this->m_textLength, true);
    std::memset(this->m_text, 0, static_cast<size_t>(this->m_textCapacity));
    std::memset(this->m_textInfo, 0, static_cast<size_t>(this->m_textCapacity) * 4);
    this->m_visibleStart = 0;
}

// ref: FUN_1424b81b0
void CSimpleEditBox::ReapplyText() {
    if (this->m_text == nullptr || this->m_textLength == 0) {
        return;
    }
    uint64_t length = static_cast<uint64_t>(this->m_textLength);
    char* copy = static_cast<char*>(_alloca((length + 0x10) & ~0xfull));
    uint64_t actual = std::strlen(this->m_text);
    if (length < actual) {
        actual = length;
    }
    std::memcpy(copy, this->m_text, static_cast<size_t>(actual));
    copy[actual] = '\0';
    this->SetText(copy, lua_taint, true, true);
}

// ref: FUN_1424b19b0
float CSimpleEditBox::GetNumber() {
    FrameScript_ReadTaint(FrameScript_GetContext(), this->m_textTaint);
    return SStrToFloat(this->m_text);
}

// ref: FUN_1424b61d0
void CSimpleEditBox::SetNumber(float number) {
    char buffer[0x20];
    float whole = 0.0f;
    if (std::modf(number, &whole) == 0.0f) {
        std::snprintf(buffer, sizeof(buffer), "%d", static_cast<int32_t>(number));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.6f", static_cast<double>(number));
    }
    this->SetText(buffer, lua_taint, false, false);
}

// ref: FUN_1424b1640
bool CSimpleEditBox::GetDisplayText(char* buffer, int32_t capacity) {
    FrameScript_ReadTaint(FrameScript_GetContext(), this->m_textTaint);
    if ((this->m_editFlags & EDIT_SECURE_TEXT) && lua_taint != nullptr) {
        return false;
    }
    int32_t written = 0;
    for (int32_t i = 0; i < this->m_textLength; i++) {
        if (this->m_textInfo[i] == 0) {
            continue;
        }
        int32_t advance = 1;
        uint32_t code = 0;
        int32_t kind = GxuDetermineControlCode(this->m_text + i, &advance, nullptr, 0, &code);
        if (IsLetter(static_cast<uint8_t>(kind)) && written < capacity - advance && 0 < advance) {
            std::memcpy(buffer + written, this->m_text + i, static_cast<size_t>(advance));
            written += advance;
        }
    }
    buffer[written] = '\0';
    return true;
}

// ref: FUN_1424b1940
bool CSimpleEditBox::GetNumLetters(int32_t* count) {
    if ((this->m_editFlags & EDIT_SECURE_TEXT) && lua_taint != nullptr) {
        return false;
    }
    if (this->m_editFlags & EDIT_COUNT_INVISIBLE_LETTERS) {
        *count = SStrLenUTF8(this->m_text);
        return true;
    }
    *count = this->CountLetters(0, this->m_textLength);
    return true;
}

// ref: FUN_1424b18a0
int32_t CSimpleEditBox::CountLetters(int32_t start, int32_t count) {
    int32_t letters = 0;
    const uint32_t* entry = this->m_textInfo + start;
    if (0 < count) {
        do {
            count--;
            if (*entry != 0 && IsLetter(Kind(*entry))) {
                letters++;
            }
            entry++;
        } while (0 < count);
        return letters;
    }
    count = -count;
    while (0 < count) {
        count--;
        if (entry <= this->m_textInfo) {
            break;
        }
        entry--;
        if (*entry != 0 && IsLetter(Kind(*entry))) {
            letters++;
        }
    }
    return letters;
}

// ref: FUN_1424af900
bool CSimpleEditBox::IsOverLimit() {
    if (0 <= this->m_maxBytes && this->m_maxBytes < this->m_textLength) {
        return true;
    }
    if (this->m_maxLetters != 0) {
        int32_t letters = (this->m_editFlags & EDIT_COUNT_INVISIBLE_LETTERS) ? SStrLenUTF8(this->m_text)
                                                                             : this->CountLetters(0, this->m_textLength);
        if (this->m_maxLetters < letters) {
            return true;
        }
    }
    int32_t visible = 0;
    int32_t markup = 0;
    for (int32_t i = 0; i < this->m_textLength;) {
        uint32_t info = this->m_textInfo[i];
        i += static_cast<int32_t>(Length(info));
        if (IsLetter(Kind(info))) {
            visible += static_cast<int32_t>(Length(info));
        } else {
            markup += static_cast<int32_t>(Length(info));
        }
        if (Length(info) == 0) {
            break;
        }
    }
    if (this->m_maxMarkupBytes != 0 && this->m_maxMarkupBytes < markup) {
        return true;
    }
    if (this->m_visibleTextByteLimit == 0) {
        return false;
    }
    return this->m_visibleTextByteLimit < visible;
}

// ref: FUN_1424b11c0
bool CSimpleEditBox::EnforceLimits(bool atEnd, bool scripted) {
    bool failed = false;
    if (!this->IsOverLimit()) {
        return true;
    }
    int32_t saved = this->m_cursorPos;
    if (atEnd) {
        this->SetCursorPosition(this->m_textLength);
    }
    while (this->IsOverLimit()) {
        int32_t cursor = this->m_cursorPos;
        int32_t bytes = Walk(this->m_textInfo, cursor, -1, true);
        if (!this->DeleteRange(cursor - bytes, cursor, scripted)) {
            failed = true;
            break;
        }
    }
    if (atEnd) {
        this->SetCursorPosition(saved);
    }
    return !failed;
}

// ---- the cursor and the highlight ---------------------------------------------------------

// ref: FUN_1424b13b0
int32_t CSimpleEditBox::SkipHidden(int32_t index) {
    uint32_t length = static_cast<uint32_t>(this->m_textLength);
    if (length < static_cast<uint32_t>(index)) {
        index = index < 0 ? 0 : static_cast<int32_t>(length);
    }
    const uint32_t* entry = this->m_textInfo + index;
    while ((*entry == 0 || ((*entry & 0xff000000) == INFO_IN_TEXTURE && Kind(*entry) != 9)) && index < static_cast<int32_t>(length)) {
        index++;
        entry++;
    }
    return index;
}

// ref: FUN_1424b5b90
void CSimpleEditBox::SetCursorPosition(int32_t position) {
    uint32_t length = static_cast<uint32_t>(this->m_textLength);
    if (length < static_cast<uint32_t>(position)) {
        position = position < 0 ? 0 : static_cast<int32_t>(length);
        if (length < static_cast<uint32_t>(position)) {
            position = position < 0 ? 0 : static_cast<int32_t>(length);
        }
    }
    position = this->SkipHidden(position);
    if (position != this->m_cursorPos) {
        this->m_dirtyFlags |= DIRTY_CURSOR;
        this->m_cursorPos = position;
        if (CSimpleTop::s_instance != nullptr) {
            this->m_cursorContext = CSimpleTop::s_instance->m_inputPhase;
        }
    }
}

// ref: FUN_1424b5c30
void CSimpleEditBox::SetCursorPositionScript(int32_t position) {
    position = this->SkipHidden(position);
    if (position != this->m_cursorPos) {
        this->m_dirtyFlags |= DIRTY_CURSOR;
        this->m_cursorPos = position;
        if (CSimpleTop::s_instance != nullptr) {
            this->m_cursorContext = CSimpleTop::s_instance->m_inputPhase;
        }
    }
}

// ref: FUN_1424b1c40
void CSimpleEditBox::HighlightText(int32_t start, int32_t end) {
    start = this->SkipHidden(start);
    if (end < 0) {
        end = -1;
    } else {
        end = this->SkipHidden(end);
    }
    this->m_highlightStart = start;
    if (end < start) {
        end = this->m_textLength;
    }
    this->m_dirtyFlags |= DIRTY_HIGHLIGHT;
    this->m_highlightEnd = end;
}

// ref: FUN_1424afe00
void CSimpleEditBox::ClearHighlight() {
    this->m_dirtyFlags |= DIRTY_HIGHLIGHT;
    this->m_highlightStart = 0;
    this->m_highlightEnd = 0;
}

// ref: FUN_1424b1410
void CSimpleEditBox::ExtendHighlight(int32_t delta) {
    if (delta == 0) {
        return;
    }
    int32_t length = this->m_textLength;
    int32_t position = this->m_cursorPos + delta;
    bool end = delta < 0 ? this->m_highlightStart <= position : this->m_highlightEnd < position;
    if (end) {
        int32_t value = position < length ? position : length;
        this->m_dirtyFlags |= DIRTY_HIGHLIGHT;
        this->m_highlightEnd = value < 1 ? 0 : value;
        return;
    }
    int32_t value = position < 1 ? 0 : position;
    this->m_dirtyFlags |= DIRTY_HIGHLIGHT;
    this->m_highlightStart = value < length ? value : length;
}

// ref: FUN_1424b2e70
void CSimpleEditBox::MoveCursor(int32_t count, bool extend) {
    int32_t cursor = this->m_cursorPos;
    int32_t bytes = Walk(this->m_textInfo, cursor, count, true);
    int32_t delta = count < 0 ? -bytes : bytes;
    if (!extend) {
        this->DropHighlight();
    } else {
        if (this->m_highlightStart == this->m_highlightEnd) {
            this->m_highlightEnd = cursor;
            this->m_highlightStart = cursor;
        }
        this->ExtendHighlight(delta);
    }
    this->m_cursorPos += delta;
    this->CursorMoved();
}

// ref: FUN_1424b30a0
void CSimpleEditBox::MoveLeft(bool extend) {
    if (0 < this->m_cursorPos) {
        this->MoveCursor(-1, extend);
    }
    if (!extend) {
        this->DropHighlight();
    }
}

// ref: FUN_1424b3600
void CSimpleEditBox::MoveRight(bool extend) {
    if (this->m_cursorPos < this->m_textLength) {
        this->MoveCursor(1, extend);
    }
    if (!extend) {
        this->DropHighlight();
    }
}

// ref: FUN_1424b3260
void CSimpleEditBox::MoveWordLeft(bool extend) {
    while (0 < this->m_cursorPos) {
        int32_t bytes = 0;
        uint32_t code = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(this->m_text + this->m_cursorPos - 1), &bytes);
        if (!CharIsSpace(code)) {
            break;
        }
        this->MoveCursor(-1, extend);
    }
    while (0 < this->m_cursorPos) {
        int32_t bytes = 0;
        uint32_t code = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(this->m_text + this->m_cursorPos - 1), &bytes);
        if (CharIsSpace(code)) {
            break;
        }
        this->MoveCursor(-1, extend);
    }
    if (!extend) {
        this->DropHighlight();
    }
}

// ref: FUN_1424b3660
void CSimpleEditBox::MoveLines(int32_t delta, bool extend) {
    this->UpdateLines();
    uint32_t cursor = static_cast<uint32_t>(this->m_cursorPos);
    this->UpdateLines();
    int32_t last = static_cast<int32_t>(this->m_lineCount) - 2;
    int32_t line = 0;
    while (line < last && this->m_lineStarts[line + 1] <= cursor) {
        line++;
    }
    int32_t target = line + delta;
    if (target <= last) {
        last = target < 0 ? 0 : target;
    }
    if (last == line) {
        if (!extend) {
            this->DropHighlight();
        }
        return;
    }
    int32_t position = this->m_cursorPos;
    uint32_t lineStart = this->m_lineStarts[line];
    int32_t letters = this->CountLetters(static_cast<int32_t>(lineStart), position - static_cast<int32_t>(lineStart));
    uint32_t targetStart = this->m_lineStarts[last];
    uint32_t targetEnd = this->m_lineStarts[last + 1];
    int32_t bytes = Walk(this->m_textInfo, static_cast<int32_t>(targetStart), letters, false);
    uint32_t moved = targetStart + static_cast<uint32_t>(bytes);
    if (targetStart < targetEnd && targetEnd <= moved) {
        moved = targetEnd - static_cast<uint32_t>(Walk(this->m_textInfo, static_cast<int32_t>(targetEnd), -1, false));
    }
    int32_t change = static_cast<int32_t>(moved) - position;
    if (!extend) {
        this->DropHighlight();
    } else {
        if (this->m_highlightStart == this->m_highlightEnd) {
            this->m_highlightEnd = position;
            this->m_highlightStart = position;
        }
        this->ExtendHighlight(change);
    }
    this->m_cursorPos += change;
    this->CursorMoved();
}

// ref: FUN_1424b39f0
void CSimpleEditBox::MoveLineStart(bool extend) {
    while (0 < this->m_cursorPos && this->m_text[this->m_cursorPos - 1] != '\n') {
        this->MoveCursor(-1, extend);
    }
    if (!extend) {
        this->DropHighlight();
    }
}

// ref: FUN_1424b3be0
void CSimpleEditBox::MoveLineEnd(bool extend) {
    while (this->m_cursorPos < this->m_textLength && this->m_text[this->m_cursorPos] != '\n') {
        this->MoveCursor(1, extend);
    }
    if (!extend) {
        this->DropHighlight();
    }
}

// ref: FUN_1424b3d60
void CSimpleEditBox::MoveToEnd(bool extend) {
    while (this->m_cursorPos < this->m_textLength) {
        this->MoveCursor(1, extend);
    }
    if (!extend) {
        this->DropHighlight();
    }
}

// ref: FUN_1424b3ed0
void CSimpleEditBox::MoveToStart(bool extend) {
    while (0 < this->m_cursorPos) {
        this->MoveCursor(-1, extend);
    }
    if (!extend) {
        this->DropHighlight();
    }
}

// ref: FUN_1424b0780
void CSimpleEditBox::DeleteChars(int32_t count, bool scripted) {
    int32_t cursor = this->m_cursorPos;
    int32_t bytes = Walk(this->m_textInfo, cursor, count, true);
    if (count < 0) {
        this->DeleteRange(cursor - bytes, cursor, scripted);
        return;
    }
    this->DeleteRange(cursor, cursor + bytes, scripted);
}

// ref: FUN_1424b0950
void CSimpleEditBox::Backspace(bool scripted) {
    if (this->m_highlightStart != this->m_highlightEnd) {
        int32_t length = this->m_textLength;
        int32_t start = this->m_highlightStart;
        int32_t clamped = start < 1 ? 0 : start;
        int32_t startIndex = length;
        if (clamped < length) {
            startIndex = start < 1 ? 0 : start;
        }
        this->m_highlightStart = startIndex;
        int32_t end = this->m_highlightEnd < length ? this->m_highlightEnd : length;
        if (end < 1) {
            this->m_highlightEnd = 0;
            this->DeleteRange(startIndex, 0, scripted);
            return;
        }
        this->m_highlightEnd = end;
        this->DeleteRange(startIndex, end, scripted);
        return;
    }
    int32_t cursor = this->m_cursorPos;
    if (cursor < 1) {
        return;
    }
    int32_t bytes = Walk(this->m_textInfo, cursor, -1, true);
    this->DeleteRange(cursor - bytes, cursor, scripted);
}

// ref: FUN_1424b0b20
void CSimpleEditBox::BackspaceWord(bool scripted) {
    if (this->m_highlightStart != this->m_highlightEnd) {
        this->Backspace(scripted);
        return;
    }
    for (int32_t pass = 0; pass < 2; pass++) {
        while (0 < this->m_cursorPos) {
            int32_t previous = this->m_cursorPos;
            do {
                previous--;
            } while (0 <= previous && this->m_textInfo[previous] == 0);
            int32_t bytes = 0;
            uint32_t code = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(this->m_text + previous), &bytes);
            if (CharIsSpace(code) != (pass == 0)) {
                break;
            }
            int32_t cursor = this->m_cursorPos;
            int32_t step = Walk(this->m_textInfo, cursor, -1, true);
            this->DeleteRange(cursor - step, cursor, scripted);
        }
    }
}

// ref: FUN_1424b0ec0
void CSimpleEditBox::DeleteForward(bool scripted) {
    int32_t length = this->m_textLength;
    if (this->m_highlightStart != this->m_highlightEnd) {
        int32_t start = this->m_highlightStart < 1 ? 0 : this->m_highlightStart;
        start = start < length ? start : length;
        this->m_highlightStart = start;
        int32_t end = length <= this->m_highlightEnd ? length : this->m_highlightEnd;
        end = 0 < end ? end : 0;
        this->m_highlightEnd = end;
        this->DeleteRange(start, end, scripted);
        return;
    }
    if (this->m_cursorPos < length) {
        this->DeleteChars(1, scripted);
    }
}

// ref: FUN_1424b0f30
void CSimpleEditBox::DeleteHighlight() {
    int32_t length = this->m_textLength;
    int32_t start = this->m_highlightStart < 1 ? 0 : this->m_highlightStart;
    start = start < length ? start : length;
    this->m_highlightStart = start;
    int32_t end = length <= this->m_highlightEnd ? length : this->m_highlightEnd;
    end = 0 < end ? end : 0;
    this->m_highlightEnd = end;
    this->DeleteRange(start, end, false);
}

// ref: FUN_1424b04d0
bool CSimpleEditBox::CopyHighlight() {
    if (this->m_highlightStart == this->m_highlightEnd || (this->m_editFlags & EDIT_PASSWORD)) {
        return false;
    }
    int32_t count = this->m_highlightEnd - this->m_highlightStart;
    char* heap = nullptr;
    char* buffer;
    if (count < 0x30d41) {
        buffer = static_cast<char*>(_alloca((static_cast<uint64_t>(count) + 0x10) & ~0xfull));
    } else {
        heap = static_cast<char*>(SMemAlloc(static_cast<uint64_t>(count) + 1, 0x1f, s_editBoxFile, 0, 0, 0));
        buffer = heap;
    }
    GxuFontStripCodes(this->m_text + this->m_highlightStart, count, 0x1500, buffer, static_cast<uint32_t>(count + 1));
    bool copied = *buffer != '\0';
    if (copied) {
        OsClipboardPutString(buffer);
    }
    if (heap != nullptr) {
        SMemFree(heap, s_editBoxFile, 0, 0);
    }
    return copied;
}

// ref: FUN_1424b5730
void CSimpleEditBox::Paste() {
    if (this->m_editFlags & EDIT_SECURITY_DISABLE_PASTE) {
        return;
    }
    char* text = OsClipboardGetString();
    if (text == nullptr) {
        return;
    }
    for (const char* p = text; *p != '\0';) {
        int32_t bytes = 0;
        uint32_t code = SUniSGetUTF8(reinterpret_cast<const uint8_t*>(p), &bytes);
        char buffer[8];
        const char* insert = nullptr;
        if (code < 0x20 || code == 0x7f) {
            if (code == 9) {
                insert = s_tabText;
            } else if ((this->m_editFlags & EDIT_MULTI_LINE) && code == 10) {
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
            this->Insert(insert, nullptr, true, false, false, true);
        }
        p += bytes;
    }
    OsClipboardFree(text);
}

// ---- history ---------------------------------------------------------------------------------

// ref: FUN_1424af7d0
void CSimpleEditBox::AddHistoryLine(const char* text) {
    const char* taint = lua_taint;
    if (this->m_historyLines == 0) {
        return;
    }
    EditHistory& entry = this->m_history[this->m_historyPos];
    uint64_t length = std::strlen(text);
    entry.m_text = static_cast<char*>(SMemReAlloc(entry.m_text, length + 1, 0x1f, s_editBoxFile, 0, 0x8b3, 0));
    entry.m_taint = taint;
    std::memcpy(entry.m_text, text, static_cast<size_t>(length + 1));
    this->m_historyPos = (this->m_historyPos + 1) % this->m_historyLines;
}

// ref: FUN_1424afe20
void CSimpleEditBox::ClearHistory() {
    this->m_historyPos = 0;
    for (int32_t i = 0; i < this->m_historyLines; i++) {
        SMemFree(this->m_history[i].m_text, s_editBoxFile, 0x922, 0);
        this->m_history[i].m_text = nullptr;
        this->m_history[i].m_taint = nullptr;
    }
}

// ref: FUN_1424b5e60
void CSimpleEditBox::SetHistoryLines(int32_t lines) {
    if (lines < 1) {
        return;
    }
    // FUN_1424b58a0: TSFixedArray<EditHistory>::SetCount
    uint32_t count = static_cast<uint32_t>(lines);
    if (count != this->m_historyCount) {
        EditHistory* old = this->m_history;
        for (uint32_t i = count; i < this->m_historyCount; i++) {
            SMemFree(old[i].m_text, s_editBoxHeader, 0x215, 0);
        }
        this->m_historyCapacity = count;
        this->m_history = static_cast<EditHistory*>(SMemAlloc(static_cast<uint64_t>(count) * sizeof(EditHistory), 8, s_editBoxHeader, 0, -7, 0));
        if (old != nullptr) {
            uint32_t keep = count < this->m_historyCount ? count : this->m_historyCount;
            for (uint32_t i = 0; i < keep; i++) {
                this->m_history[i].m_text = old[i].m_text != nullptr ? SStrDupA(old[i].m_text, s_editBoxHeader, 0) : nullptr;
                this->m_history[i].m_taint = old[i].m_taint;
                SMemFree(old[i].m_text, s_editBoxHeader, 0x215, 0);
            }
            SMemFree(old, ".?AV?$TSFixedArray@UEditHistory@CSimpleEditBox@@@@", -2, 0);
        }
        for (uint32_t i = this->m_historyCount; i < count; i++) {
            this->m_history[i] = {};
        }
        this->m_historyCount = count;
    }
    if (lines <= this->m_historyPos) {
        this->m_historyPos = lines - 1;
    }
    this->m_historyLines = lines;
}

// ref: FUN_1424af9f0
void CSimpleEditBox::HistoryPrevious() {
    if (this->m_editFlags & EDIT_SECURITY_DISABLE_SET_TEXT) {
        return;
    }
    int32_t lines = this->m_historyLines;
    if (lines < 1) {
        return;
    }
    int32_t position = this->m_historyPos;
    for (int32_t tried = 1; tried <= lines; tried++) {
        position--;
        int32_t index = 0 <= position ? position : position + lines;
        if (this->m_history[index].m_text != nullptr) {
            this->m_historyPos = index;
            this->SetText(this->m_history[index].m_text, this->m_history[index].m_taint, false, false);
            return;
        }
    }
}

// ref: FUN_1424b14d0
void CSimpleEditBox::HistoryNext() {
    if (this->m_editFlags & EDIT_SECURITY_DISABLE_SET_TEXT) {
        return;
    }
    int32_t lines = this->m_historyLines;
    for (int32_t tried = 1; tried <= lines; tried++) {
        int32_t index = (this->m_historyPos + tried) % lines;
        if (this->m_history[index].m_text != nullptr) {
            this->m_historyPos = index;
            this->SetText(this->m_history[index].m_text, this->m_history[index].m_taint, false, false);
            return;
        }
    }
}

// ---- modes -----------------------------------------------------------------------------------

// ref: FUN_1424b6020
void CSimpleEditBox::SetMultiLine(bool multiLine) {
    this->m_editFlags = multiLine ? (this->m_editFlags | EDIT_MULTI_LINE) : (this->m_editFlags & ~EDIT_MULTI_LINE);
    // FUN_1424b5a40
    this->m_lineCount = 2;
    if (this->m_lineCapacity < 2) {
        uint32_t* old = this->m_lineStarts;
        this->m_lineCapacity = 2;
        this->m_lineStarts = static_cast<uint32_t*>(SMemAlloc(8, 8, ".?AV?$TSGrowableArray@I@@", 0, -7, 0));
        this->m_lineStarts[0] = 0;
        this->m_lineStarts[1] = 0;
        if (old != nullptr) {
            SMemFree(old, ".?AV?$TSGrowableArray@I@@", -2, 0);
        }
    }
    CSimpleFontString* string = this->m_string;
    string->m_inheritedFlags &= ~CSimpleFontString::FONTSTRING_JUSTIFY_V;
    if (!(this->m_editFlags & EDIT_MULTI_LINE)) {
        uint32_t flags = (string->m_stringFlags & ~0x28u) | CSimpleFontString::FONTSTRING_JUSTIFY_MIDDLE;
        if (string->m_stringFlags != flags) {
            string->m_stringFlags = flags;
            if (string->m_textBlock != nullptr) {
                string->UpdateString();
            }
        }
        SetOwnStringFlag(string, 0x4000, true);
        SetOwnStringFlag(string, CSimpleFontString::FONTSTRING_NO_WORD_WRAP, true);
    } else {
        uint32_t flags = (string->m_stringFlags & ~0x30u) | CSimpleFontString::FONTSTRING_JUSTIFY_TOP;
        if (string->m_stringFlags != flags) {
            string->m_stringFlags = flags;
            if (string->m_textBlock != nullptr) {
                string->UpdateString();
            }
        }
        SetOwnStringFlag(string, 0x4000, false);
        SetOwnStringFlag(string, CSimpleFontString::FONTSTRING_NO_WORD_WRAP, false);
    }
    SetOwnStringFlag(string, CSimpleFontString::FONTSTRING_NON_SPACE_WRAP, true);
    this->UpdateSizes();
}

// ref: FUN_1424b62a0
void CSimpleEditBox::SetNumeric(bool numeric) {
    uint32_t flags = this->m_editFlags;
    if (numeric == ((flags & (EDIT_NUMERIC | EDIT_NUMERIC_FULL_RANGE)) != 0) && !(numeric && (flags & EDIT_NUMERIC_FULL_RANGE))) {
        return;
    }
    this->m_editFlags = (flags & ~EDIT_INPUT_MODES) | (numeric ? EDIT_NUMERIC : 0);
    if (numeric) {
        this->ReapplyText();
    }
}

// ref: FUN_1424b62f0
void CSimpleEditBox::SetNumericFullRange(bool numeric) {
    if (numeric == (((this->m_editFlags >> 10) & 1) != 0)) {
        return;
    }
    this->m_editFlags = (this->m_editFlags & ~EDIT_INPUT_MODES) | (numeric ? (EDIT_NUMERIC | EDIT_NUMERIC_FULL_RANGE) : 0);
    if (numeric) {
        this->ReapplyText();
    }
}

// ref: FUN_1424b6330
void CSimpleEditBox::SetPassword(bool password) {
    if (password == (((this->m_editFlags >> 3) & 1) != 0)) {
        return;
    }
    this->m_editFlags = (this->m_editFlags & ~EDIT_INPUT_MODES) | (password ? EDIT_PASSWORD : 0);
    if (password) {
        this->ReapplyText();
    }
}

// ref: FUN_1424b5820
void CSimpleEditBox::SetAlphabeticOnly(bool enabled) {
    if (enabled == (((this->m_editFlags >> 9) & 1) != 0)) {
        return;
    }
    this->m_editFlags = (this->m_editFlags & ~EDIT_INPUT_MODES) | (enabled ? EDIT_ALPHABETIC_ONLY : 0);
    if (enabled) {
        this->ReapplyText();
    }
}

// ref: FUN_1424b6370
void CSimpleEditBox::SetSecureText(bool secure) {
    if (secure) {
        this->m_editFlags |= EDIT_SECURE_TEXT;
        return;
    }
    if (lua_taint == nullptr) {
        this->m_editFlags &= ~EDIT_SECURE_TEXT;
    }
}

// ref: FUN_1424b5810
void CSimpleEditBox::ResetInputMode() {
    this->m_editFlags &= ~EDIT_INPUT_MODES;
}

// ref: FUN_1424b66f0
void CSimpleEditBox::SetVisibleTextByteLimit(int32_t limit) {
    limit = limit < 0 ? 0 : limit;
    if (limit != this->m_visibleTextByteLimit) {
        this->m_visibleTextByteLimit = limit;
        this->EnforceLimits(true, true);
    }
}

// ---- the font --------------------------------------------------------------------------------

// ref: FUN_1424b1810
void CSimpleEditBox::GetFont(const char** path, float* height, uint8_t* flags) {
    CSimpleFontedFrameFont* font = this->m_font;
    if (font == nullptr) {
        return;
    }
    *path = font->m_attributes.m_fontPath;
    *height = font->m_attributes.m_height;
    *flags = font->m_attributes.m_fontFlags;
}

// ref: FUN_1424b5d60
bool CSimpleEditBox::SetFont(const char* path, float height, const uint8_t* flags) {
    if (height <= 0.0f) {
        FrameScript_ActionError("Invalid fontHeight: %f, height must be > 0", static_cast<double>(height));
        return false;
    }
    CSimpleFontedFrameFont* font = this->m_font;
    if (!font->m_attributes.SetFont(path, height, flags)) {
        return false;
    }
    FontChanged(font, CSimpleFontAttributes::CHANGED_FONT);
    return true;
}

// ref: FUN_1424b5dd0
void CSimpleEditBox::SetFontObject(CSimpleFont* font) {
    if (font == nullptr) {
        FrameScript_ActionError("Invalid font");
        return;
    }
    for (CSimpleFont* parent = font->m_fontObject; parent != nullptr; parent = parent->m_fontObject) {
        if (parent == font) {
            FrameScript_ActionError("Can't create a font object loop");
            return;
        }
    }
    this->m_font->SetFontObject(font);
}

// ref: FUN_1424b5eb0
void CSimpleEditBox::SetIndentedWordWrap(bool indented) {
    uint32_t flags = this->m_font->m_attributes.m_stringFlags;
    flags = indented ? (flags | CSimpleFontString::FONTSTRING_INDENTED_WORD_WRAP) : (flags & ~CSimpleFontString::FONTSTRING_INDENTED_WORD_WRAP);
    SetFontStringFlags(this->m_font, flags);
    FontChanged(this->m_font, CSimpleFontAttributes::CHANGED_FLAGS);
}

// ref: FUN_1424b5f00
void CSimpleEditBox::SetJustifyH(uint32_t justify) {
    uint32_t flags = this->m_font->m_attributes.m_stringFlags;
    SetFontStringFlags(this->m_font, (flags & ~7u) | (justify & 7));
    FontChanged(this->m_font, CSimpleFontAttributes::CHANGED_FLAGS);
}

// ref: FUN_1424b5f50
void CSimpleEditBox::SetJustifyV(uint32_t justify) {
    uint32_t flags = this->m_font->m_attributes.m_stringFlags;
    SetFontStringFlags(this->m_font, (flags & ~0x38u) | (justify & 0x38));
    FontChanged(this->m_font, CSimpleFontAttributes::CHANGED_FLAGS);
}

// ref: FUN_1424b6620
void CSimpleEditBox::SetTextColor(const uint32_t* rgb, const uint8_t* alpha) {
    CSimpleFontAttributes& attributes = this->m_font->m_attributes;
    uint32_t color = PackColor(rgb, alpha);
    if (color != attributes.m_color || !(attributes.m_flags & CSimpleFontAttributes::HAS_COLOR)) {
        attributes.m_color = color;
        attributes.m_flags |= CSimpleFontAttributes::HAS_COLOR | CSimpleFontAttributes::CHANGED_COLOR;
    }
    FontChanged(this->m_font, CSimpleFontAttributes::CHANGED_COLOR);
}

// ref: FUN_1424b63a0
void CSimpleEditBox::SetShadowColor(const uint32_t* rgb, const uint8_t* alpha) {
    CSimpleFontAttributes& attributes = this->m_font->m_attributes;
    uint32_t color = PackColor(rgb, alpha);
    if (color != attributes.m_shadowColor || !(attributes.m_flags & CSimpleFontAttributes::HAS_SHADOW)) {
        attributes.m_shadowColor = color;
        attributes.m_flags |= CSimpleFontAttributes::HAS_SHADOW | CSimpleFontAttributes::CHANGED_SHADOW;
    }
    FontChanged(this->m_font, CSimpleFontAttributes::CHANGED_SHADOW);
}

// ref: FUN_1424b6430
void CSimpleEditBox::SetShadowOffset(const float* offset) {
    CSimpleFontAttributes& attributes = this->m_font->m_attributes;
    if (attributes.m_shadowOffset[0] != offset[0] || attributes.m_shadowOffset[1] != offset[1]
        || !(attributes.m_flags & CSimpleFontAttributes::HAS_SHADOW)) {
        attributes.m_shadowOffset[0] = offset[0];
        attributes.m_shadowOffset[1] = offset[1];
        attributes.m_flags |= CSimpleFontAttributes::HAS_SHADOW | CSimpleFontAttributes::CHANGED_SHADOW;
    }
    FontChanged(this->m_font, CSimpleFontAttributes::CHANGED_SHADOW);
}

// ref: FUN_1424b6490
void CSimpleEditBox::SetSpacing(float spacing) {
    CSimpleFontAttributes& attributes = this->m_font->m_attributes;
    if (FONT_EPSILON <= std::fabs(spacing - attributes.m_spacing) || !(attributes.m_flags & CSimpleFontAttributes::HAS_SPACING)) {
        attributes.m_spacing = spacing;
        attributes.m_flags |= CSimpleFontAttributes::HAS_SPACING | CSimpleFontAttributes::CHANGED_SPACING;
    }
    FontChanged(this->m_font, CSimpleFontAttributes::CHANGED_SPACING);
}

// ref: FUN_1424b5e20
void CSimpleEditBox::SetHighlightColor(const uint32_t* rgb, const uint8_t* alpha) {
    this->m_highlightColor = PackColor(rgb, alpha);
}

// ref: FUN_1424b66a0
void CSimpleEditBox::SetTextInsets(float left, float right, float top, float bottom) {
    this->m_textInsets[0] = bottom;
    this->m_textInsets[1] = left;
    this->m_textInsets[3] = right;
    this->m_textInsets[2] = top;
    this->UpdateSizes();
}

// ref: FUN_1424b1b20
void CSimpleEditBox::GetTextInsets(float* left, float* right, float* top, float* bottom) {
    *left = this->m_textInsets[1];
    *right = this->m_textInsets[3];
    *top = this->m_textInsets[2];
    *bottom = this->m_textInsets[0];
    this->UpdateSizes();
}
