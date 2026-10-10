// Engine/Source/Os/W32/OsClipboard.cpp: the clipboard's Unicode text, as UTF-8.

#include "os/Window.hpp"
#include "storm/Memory.hpp"
#include "storm/Unicode.hpp"
#include <windows.h>

namespace {

const char s_clipboardFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Os\\W32\\OsClipboard.cpp";

}

// ref: FUN_14254f820 (FUN_14254fb50)
char* OsClipboardGetString() {
    if (!OpenClipboard(nullptr)) {
        return nullptr;
    }
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data != nullptr) {
        auto wide = static_cast<const uint16_t*>(GlobalLock(data));
        if (wide != nullptr) {
            uint64_t length = 0;
            while (wide[length] != 0) {
                length++;
            }
            auto text = static_cast<char*>(SMemAlloc(length * 3 + 1, 0x13, s_clipboardFile, 0, 0x67, 0));
            int32_t written = 0;
            UnicodeConvertUtf16ToUtf8(text, static_cast<uint32_t>(length) * 3 + 1, wide, static_cast<uint32_t>(length), &written,
                nullptr);
            text[written] = '\0';
            GlobalUnlock(data);
            CloseClipboard();
            return text;
        }
    }
    CloseClipboard();
    return nullptr;
}

// ref: FUN_14254f830 (FUN_14254fc70)
bool OsClipboardPutString(const char* text) {
    uint64_t length = 0;
    while (text[length] != '\0') {
        length++;
    }
    uint64_t chars = length + 1;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT | GMEM_DDESHARE, chars * 2);
    if (memory == nullptr) {
        return false;
    }
    auto wide = static_cast<wchar_t*>(GlobalLock(memory));
    if (wide == nullptr) {
        CloseClipboard();
        return false;
    }
    UnicodeConvertUtf8ToUtf16(wide, static_cast<uint32_t>(chars), text, 0x7fffffff, nullptr, nullptr);
    GlobalUnlock(memory);
    if (OpenClipboard(nullptr)) {
        if (EmptyClipboard()) {
            SetClipboardData(CF_UNICODETEXT, memory);
            CloseClipboard();
            return true;
        }
        CloseClipboard();
    }
    GlobalFree(memory);
    return false;
}

// ref: FUN_14254fb30
void OsClipboardFree(char* text) {
    SMemFree(text, s_clipboardFile, 0x79, 0);
}
