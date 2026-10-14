// WoW/Source/WowClient/src/WowClient_StartupService.cpp: the services the client brings up
// before the game - ClientServices, Agent, locale and region, telemetry, the downloader.

#include "blz/Function.hpp"
#include "blz/String.hpp"
#include "client/CommandLine.hpp"
#include "client/Locale.hpp"
#include "client/Startup.hpp"
#include "console/Var.hpp"
#include "services/Agent.hpp"
#include "services/ClientServices.hpp"
#include "services/Telemetry.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Fnv.hpp"
#include "util/Unimplemented.hpp"
#include <cstring>
#include <windows.h>
#include <commctrl.h>

CVar* g_overrideArchive;  // DAT_142ddfa78

namespace {

uint8_t s_initialHotfixPending = 1;  // DAT_142cc84bc
void** s_downloader;                 // DAT_1430d2378

// The listeners told about the initial hotfix events (a blz::vector of function pointers).
struct {
    void (**m_data)(int32_t event);
    uint64_t m_size;
    uint64_t m_capacity;
} s_initialHotfixListeners;  // DAT_142e8e0f0

}

// Adds a listener for the initial hotfix events (vector push_back).
// ref: FUN_14056df90
void HotfixAddInitialListener(void (*listener)(int32_t event)) {
    static const char s_tag[] = ".P6AXW4InitialHotfixEvent@@@Z";
    auto& list = s_initialHotfixListeners;
    uint64_t capacity = list.m_capacity & ~blz::string::INLINE;
    if (list.m_size == capacity && capacity < capacity + 1) {
        uint64_t half = (list.m_capacity >> 1) & 0x3FFFFFFFFFFFFFFFull;
        uint64_t grow = capacity + 1 <= half + capacity ? half : 1;
        list.m_capacity = ((grow + capacity) & ~blz::string::INLINE) | (list.m_capacity & blz::string::INLINE);
        auto data = static_cast<void (**)(int32_t)>(
            SMemAlloc((grow + capacity) * 8, 0xB, s_tag, SStrHashFnv1a(s_tag), -6, 0));
        for (uint64_t i = 0; i < list.m_size; i++) {
            data[i] = list.m_data[i];
        }
        if (!(list.m_capacity & blz::string::INLINE)) {
            SMemFree(list.m_data, s_tag, -6, 0);
        }
        list.m_capacity &= ~blz::string::INLINE;
        list.m_data = data;
    }
    list.m_data[list.m_size++] = listener;
}

namespace {

// Initial hotfixes: event 3 clears the pending flag, event 4 sets it.
// ref: 0x140227750
void InitialHotfixListener(int32_t event) {
    if (event == 3) {
        s_initialHotfixPending = 0;
    } else if (event == 4) {
        s_initialHotfixPending = 1;
    }
}

// ClientServices' status poll: ready when no initial hotfix is pending and the world and
// loading states allow it; otherwise reports progress. Unreachable while ClientServices is
// unported.
// ref: FUN_140227770
bool ClientServicesStatusCallback() {
    FROST_UNIMPLEMENTED();
    return false;
}

// The telemetry context providers (filled into protobuf messages). Never called while
// telemetry is offline.
// ref: FUN_1402281a0
void TelemetryProvideIds(void* message) {
    (void)message;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140228220
void TelemetryProvideClient(void* message) {
    (void)message;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140227f50
void TelemetryProvideCharacter(void* message) {
    (void)message;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140228060
void TelemetryProvideWorld(void* message) {
    (void)message;
    FROST_UNIMPLEMENTED();
}

// The invoker for a provider held as a plain function pointer.
// ref: 0x1402282e0
void InvokeProvider(void* message, void* storage) {
    (*static_cast<void (**)(void*)>(storage))(message);
}

blz::function_storage MakeProvider(void (*provider)(void*)) {
    blz::function_storage function;
    function.m_invoke = reinterpret_cast<void*>(&InvokeProvider);
    function.m_manager = &blz::function_trivial_manager;
    std::memcpy(function.m_storage, &provider, sizeof(provider));
    return function;
}

// What Agent calls back with; forwards to 0x1402271a0 (not ported).
// ref: 0x140226ea0
void AgentEventCallback(void* param) {
    (void)param;
    FROST_UNIMPLEMENTED();
}

// Reads a value from HKCU\<key>\<value> as UTF-8, decrypting it (DPAPI) when asked; returns
// 0 or an error code. Only reached under -launcherlogin; not ported yet.
// ref: FUN_141dfb910
uint32_t OsRegistryReadString(blz::string* key, blz::string* value, blz::string* result, bool encrypted) {
    (void)key;
    (void)value;
    (void)encrypted;
    result->m_data[0] = '\0';
    result->m_size = 0;
    FROST_UNIMPLEMENTED();
    return 0x2EE1;
}

}  // namespace

// Reads a launch option the Battle.net launcher left for `product` (only with -launcherlogin).
// ref: FUN_14148b460
bool LauncherOptionRead(const char* product, const char* name, char* buffer, uint64_t size, bool encrypted) {
    if (!CommandLineHasOption(COMMAND_LINE_LAUNCHERLOGIN)) {
        return false;
    }
    blz::string result;
    blz::string value;
    value.assign(name);
    blz::string key;
    key.assign("Software\\Blizzard Entertainment\\Battle.net\\Launch Options\\");
    uint64_t length = std::strlen(product);
    key.reserve(key.m_size + length);
    std::memcpy(key.m_data + key.m_size, product, length);
    key.m_size += length;
    key.m_data[key.m_size] = '\0';

    uint32_t error = OsRegistryReadString(&key, &value, &result, encrypted);
    if (error != 0) {
        return false;
    }
    if (size != 0) {
        const char* in = result.m_data;
        for (;;) {
            if (--size == 0) {
                *buffer = '\0';
                break;
            }
            char c = *in++;
            *buffer++ = c;
            if (c == '\0') {
                break;
            }
        }
    }
    return true;
}


// ref: FUN_14148ba20
bool LauncherSettingRead(const char* name, char* buffer, uint64_t size, bool encrypted) {
    return LauncherOptionRead("WoW", name, buffer, size, encrypted);
}

namespace {

// The reference executable, mapped at its own base with its headers and resources
// (src/gen/RefImage.cpp): the region picker's dialog and strings are its resources, and it
// names itself as their module.
HINSTANCE const s_resources = reinterpret_cast<HINSTANCE>(0x140000000ull);  // IMAGE_DOS_HEADER_140000000

// The picker's rows (0x18 bytes each, DAT_142cc8650): the language's code, its locale, the
// group (region) it is listed under, and the region it picks.
struct REGIONCHOICE {
    const char* m_code;
    int32_t m_locale;
    int32_t m_group;
    int32_t m_region;
    int32_t m_pad;
};
const REGIONCHOICE s_choices[13] = {
    { "enUS", 0, 0, 1, 0 },
    { "esMX", 7, 0, 1, 0 },
    { "ptBR", 10, 0, 1, 0 },
    { "deDE", 3, 1, 3, 0 },
    { "enGB", 0, 1, 3, 0 },
    { "esES", 6, 1, 3, 0 },
    { "frFR", 2, 1, 3, 0 },
    { "itIT", 11, 1, 3, 0 },
    { "ptPT", 10, 1, 3, 0 },
    { "ruRU", 8, 1, 3, 0 },
    { "koKR", 1, 2, 2, 0 },
    { "zhTW", 5, 3, 4, 0 },
    { "zhCN", 4, 4, 5, 0 },
};  // DAT_142cc8650

// Each locale's Windows language group, for whether its strings can be shown (DAT_142cc8018).
const uint32_t s_languageGroups[12] = { 1, 8, 1, 1, 10, 9, 1, 1, 5, 0, 1, 1 };  // DAT_142cc8018

// The string ids: each group's header (DAT_142618d20) and confirmation (DAT_142618d70), then the
// locale's own version of a string at that id plus the locale.
const int32_t s_groupHeaders[6] = { 0xec, 0xf8, 0x104, 0x110, 0x11c, 0x128 };  // DAT_142618d20
const int32_t s_groupConfirms[5] = { 0xb0, 0xbc, 0xc8, 0xd4, 0xe0 };          // DAT_142618d70

constexpr int32_t ID_LIST = 0x3ec;

// String `id + locale` in the locale's own language when Windows can show it, else string `id`.
void LoadLocalizedString(int32_t id, int32_t localized, int32_t locale, wchar_t* text) {
    if ((LoadStringW(s_resources, static_cast<UINT>(localized), text, 0x1000) == 0
            || !IsValidLanguageGroup(s_languageGroups[locale], LGRPID_INSTALLED))
        && LoadStringW(s_resources, static_cast<UINT>(id), text, 0x1000) == 0) {
        text[0] = L'\0';
    }
}

// The list: one full-row column, a group per region (headed in the current choice's language),
// each language in its own; the current choice selected.
// ref: FUN_14021a250
INT_PTR RegionDialogInit(HWND dialog, WPARAM focus, int32_t* selected) {
    (void)focus;
    HWND list = GetDlgItem(dialog, ID_LIST);
    RECT rect;
    GetClientRect(list, &rect);
    wchar_t empty = L'\0';
    LVCOLUMNA column = {};
    column.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT;
    column.cx = rect.right - rect.left - 4;
    column.pszText = reinterpret_cast<char*>(&empty);
    SendMessageA(list, LVM_INSERTCOLUMNA, 0, reinterpret_cast<LPARAM>(&column));
    SendMessageA(list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT);
    SendMessageA(list, LVM_ENABLEGROUPVIEW, 1, 0);
    wchar_t text[0x1000];
    int32_t group = -1;
    for (int32_t i = 0; i < 13; i++) {
        const REGIONCHOICE& choice = s_choices[i];
        if (choice.m_group != group) {
            int32_t locale = s_choices[*selected == -1 ? 0 : *selected].m_locale;
            int32_t id = s_groupHeaders[choice.m_group];
            LoadLocalizedString(id, id + locale, locale, text);
            LVGROUP header = {};
            header.cbSize = 0x98;
            header.mask = LVGF_HEADER | LVGF_GROUPID;
            header.pszHeader = text;
            header.iGroupId = choice.m_group;
            SendMessageA(list, LVM_INSERTGROUP, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(&header));
            group = choice.m_group;
        }
        LoadLocalizedString(0x134 + i, 0x90 + i, choice.m_locale, text);
        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_GROUPID;
        item.pszText = text;
        item.iGroupId = choice.m_group;
        item.iItem = static_cast<int>(SendMessageA(list, LVM_GETITEMCOUNT, 0, 0));
        SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
    }
    SetFocus(list);
    if (*selected != -1) {
        LVITEMW item = {};
        item.stateMask = 0xf;
        item.state = LVIS_FOCUSED | LVIS_SELECTED;
        SendMessageA(list, LVM_SETITEMSTATE, static_cast<WPARAM>(*selected), reinterpret_cast<LPARAM>(&item));
    }
    return 1;
}

// A selection enables OK and relabels both buttons in the chosen language; the selected row is
// drawn in the highlight colours.
// ref: FUN_14021a570
INT_PTR RegionDialogNotify(HWND dialog, int32_t control, NMHDR* header) {
    if (control != ID_LIST) {
        return 0;
    }
    if (static_cast<int32_t>(header->code) == LVN_ITEMCHANGED) {
        auto changed = reinterpret_cast<NMLISTVIEW*>(header);
        uint32_t selected = changed->uNewState >> 1 & 1;
        if (selected != (changed->uOldState >> 1 & 1)) {
            EnableWindow(GetDlgItem(dialog, IDOK), static_cast<BOOL>(selected));
        }
        if (selected != 0) {
            int32_t locale = s_choices[changed->iItem].m_locale;
            wchar_t text[0x1000];
            LoadLocalizedString(0x84, locale + 0x84, locale, text);
            SetDlgItemTextW(dialog, IDCANCEL, text);
            LoadLocalizedString(0xa4, locale + 0xa4, locale, text);
            SetDlgItemTextW(dialog, IDOK, text);
        }
        return 1;
    }
    if (static_cast<int32_t>(header->code) != NM_CUSTOMDRAW) {
        return 0;
    }
    auto draw = reinterpret_cast<NMLVCUSTOMDRAW*>(header);
    if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) {
        SetWindowLongPtrW(dialog, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
        return 1;
    }
    if (draw->nmcd.dwDrawStage != CDDS_ITEMPREPAINT) {
        return 0;
    }
    HWND list = GetDlgItem(dialog, ID_LIST);
    if (SendMessageA(list, LVM_GETITEMSTATE, draw->nmcd.dwItemSpec, LVIS_SELECTED) != 0) {
        draw->clrTextBk = GetSysColor(COLOR_HIGHLIGHT);
        draw->clrText = GetSysColor(COLOR_HIGHLIGHTTEXT);
    }
    SetWindowLongPtrW(dialog, DWLP_MSGRESULT, CDRF_DODEFAULT);
    return 1;
}

// OK asks for confirmation in the chosen language and ends with the row; Cancel and closing end
// with -1.
// ref: FUN_14021a7d0
INT_PTR CALLBACK RegionDialogProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    INT_PTR result = -1;
    if (message != WM_CLOSE) {
        if (message == WM_NOTIFY) {
            return RegionDialogNotify(dialog, static_cast<int32_t>(wparam), reinterpret_cast<NMHDR*>(lparam));
        }
        if (message == WM_INITDIALOG) {
            return RegionDialogInit(dialog, wparam, reinterpret_cast<int32_t*>(lparam));
        }
        if (message != WM_COMMAND) {
            return 0;
        }
        if (LOWORD(wparam) == IDOK) {
            int32_t row = static_cast<int32_t>(
                SendMessageA(GetDlgItem(dialog, ID_LIST), LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
            if (row == -1) {
                return 0;
            }
            int32_t locale = s_choices[row].m_locale;
            int32_t id = s_groupConfirms[s_choices[row].m_group];
            wchar_t text[0x1000];
            LoadLocalizedString(id, id + locale, locale, text);
            if (MessageBoxW(dialog, text, L"World of Warcraft", MB_OKCANCEL | MB_ICONEXCLAMATION) != IDOK) {
                return 0;
            }
            result = row;
        } else if (LOWORD(wparam) != IDCANCEL) {
            return 0;
        }
    }
    EndDialog(dialog, result);
    return 1;
}

// The region/language picker (dialog resource 0x90) shown when the portal or the locale had
// to be guessed: starts on the row matching the guess (or the first in the guessed region),
// and returns the chosen region and locale; false on cancel.
// ref: FUN_140219cc0
bool RegionDialogRun(uint32_t* region, uint32_t* locale) {
    int32_t regionMatch = -1;
    int32_t selected = -1;
    for (int32_t i = 0; i < 13; i++) {
        if (s_choices[i].m_region == static_cast<int32_t>(*region)) {
            if (s_choices[i].m_locale == static_cast<int32_t>(*locale)) {
                selected = i;
                break;
            }
            if (regionMatch == -1) {
                regionMatch = i;
            }
        }
        selected = regionMatch;
    }
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&controls);
    INT_PTR row = DialogBoxParamW(s_resources, MAKEINTRESOURCEW(0x90), nullptr, &RegionDialogProc,
        reinterpret_cast<LPARAM>(&selected));
    if (static_cast<int32_t>(row) == -1) {
        return false;
    }
    *region = static_cast<uint32_t>(s_choices[row].m_region);
    *locale = static_cast<uint32_t>(s_choices[row].m_locale);
    return true;
}

// Reference-counted start/stop of a library's global state (call-once init, a shutdown list).
// Only on the cancel path; not identified yet.
// ref: FUN_142204650
void LibraryAddRef() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_142204520
void LibraryRelease() {
    FROST_UNIMPLEMENTED();
}

CVar* Lookup(const char* name) {
    return CVarLookup(t2util::ConstFnvHashStringI(name));
}

}

// Brings up the client services. False when the region/language picker is cancelled, after
// tearing down what was started.
// ref: FUN_140227910
bool StartupServiceInitialize() {
    ClientServicesInitialize();
    ClientServicesSetStatusCallback(ClientServicesGet(), &ClientServicesStatusCallback);
    HotfixAddInitialListener(&InitialHotfixListener);
    g_clientInitFlags |= 4;
    SetBuildType(2);
    AgentReadInstallConfig();
    g_clientInitFlags |= 0x10;
    // 0x140255c60, an empty trace function, is called here with the install config's uid

    const char* uid = CommandLineGetUid();
    if (!uid || *uid == '\0') {
        uid = g_agentUid->m_stringValue.m_str;
    } else {
        CVarSet(g_agentUid, uid, true, false, false, true);
    }
    // the same trace again, with the uid
    AgentInitialize(uid);
    AgentStart();
    g_clientInitFlags |= 0x20;

    bool guessedLocale = false;
    char setting[0x10];
    setting[0] = '\0';
    if (LauncherSettingRead("LOCALE", setting, 0x10, false)) {
        uint32_t locale = LocaleFromString(setting);
        LocaleSetText(locale);
        if (LauncherSettingRead("LOCALE_AUDIO", setting, 0x10, false)) {
            locale = LocaleFromString(setting);
        }
        LocaleSetAudio(locale);
    } else {
        CVar* textLocale = Lookup("textLocale");
        CVar* audioLocale = Lookup("audioLocale");
        bool haveText = false;
        uint32_t locale = LOCALE_NONE;
        if (textLocale->m_stringValue.m_str && *textLocale->m_stringValue.m_str != '\0') {
            locale = LocaleFromString(textLocale->m_stringValue.m_str);
            LocaleSetText(locale);
            haveText = true;
        }
        if (!audioLocale->m_stringValue.m_str || *audioLocale->m_stringValue.m_str == '\0') {
            if (locale != LOCALE_NONE) {
                LocaleSetAudio(locale);
            }
        } else {
            LocaleSetAudio(LocaleFromString(audioLocale->m_stringValue.m_str));
        }
        if (!haveText) {
            locale = LocaleFromSystem(0);
            LocaleSetText(locale);
            LocaleSetAudio(locale);
            guessedLocale = true;
        }
    }

    CVar* portal = Lookup("portal");
    setting[0] = '\0';
    uint32_t region = REGION_NONE;
    if (LauncherSettingRead("REGION", setting, 0x10, false)) {
        region = RegionFromName(setting);
    }
    uint32_t systemRegion;
    bool guessedPortal = ClientServicesSetupPortal(region, &systemRegion);
    if (guessedPortal || guessedLocale) {
        // 0x1402112e0, an empty function, is called here
        uint32_t locale = LocaleGetText();
        uint32_t chosen = guessedPortal ? systemRegion : region;
        if (!RegionDialogRun(&chosen, &locale)) {
            CVarSet(portal, "", true, false, false, true);
            AgentShutdown();
            g_clientTeardownFlags |= 0x20;
            AgentFreeInstallConfig();
            g_clientTeardownFlags |= 0x10;
            ClientServicesShutdown();
            g_clientTeardownFlags |= 4;
            LibraryAddRef();
            LibraryRelease();
            return false;
        }
        CVarSet(portal, PortalForRegion(chosen), true, false, false, true);
        LocaleSetText(locale);
        LocaleSetAudio(locale);
    }

    if (LocaleGetAudio() != LocaleGetText() && LocaleGetAudio() != 0) {
        LocaleSetAudio(LocaleGetText());
    }

    CVar* textLocale = Lookup("textLocale");
    CVar* audioLocale = Lookup("audioLocale");
    AgentSetTextLocaleCode(LocaleToCode(LocaleGetText()));
    AgentSetAudioLocaleCode(LocaleToCode(LocaleGetAudio()));
    LocaleSetFontGroup(LocaleGetFontGroup(LocaleGetText()));

    g_overrideArchive = CVarRegister("overrideArchive", "Whether or not the client loads alternate data", 0,
        RegionGet() == 5 ? "1" : "0", static_cast<CVARCALLBACK>(nullptr), 4, false, nullptr, false);
    CVarSet(textLocale, LocaleGetName(LocaleGetText()), true, false, false, true);
    CVarSet(audioLocale, LocaleGetName(LocaleGetAudio()), true, false, false, true);

    {
        PORTALHOST host;
        PortalGetHost(&host, portal->m_stringValue.m_str);
        RegionSet(RegionFromPortalHost(&host));
    }

    bool china = RegionGet() == 5;
    blz::function_storage ids = MakeProvider(&TelemetryProvideIds);
    blz::function_storage client = MakeProvider(&TelemetryProvideClient);
    blz::function_storage character = MakeProvider(&TelemetryProvideCharacter);
    blz::function_storage world = MakeProvider(&TelemetryProvideWorld);
    TelemetryInitialize(&ids, &client, &character, &world, china);

    auto holder = static_cast<void**>(SMemAlloc(8, 3, __FILE__, SStrHashFnv1a(__FILE__), 0x1A0, 0));
    s_downloader = WowDownloadCreate(holder);
    WowDownloadStart(s_downloader);
    g_clientInitFlags |= 8;

    if (RegionGet() == 5) {
        if (CVar* profanityFilter = Lookup("profanityFilter")) {
            CVarSet(profanityFilter, "1", true, false, false, true);
        }
    }

    AgentSetProgramCode(0x576F57);  // 'WoW'
    AgentSetCallback(&AgentEventCallback);
    if (g_launchAgent && g_launchAgent->m_intValue != 0) {
        AgentSetTextLocale(LocaleGetText());
        AgentSetAudioLocale(LocaleGetAudio());
        AgentSetRegion(RegionGet());
        AgentSetGameLocale(LocaleGetText());
    }
    // 0x1402112e0, an empty function, is called here
    g_clientInitFlags |= 0x40;
    return true;
}
