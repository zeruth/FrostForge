#include "client/Locale.hpp"
#include "client/LocaleTables.hpp"
#include "os/Os.hpp"
#include <cstring>

namespace {

uint32_t s_textLocale;   // DAT_142e96964
uint32_t s_audioLocale;  // DAT_142e96968
uint32_t s_region = 1;   // DAT_142cce24c (static value: US)
// DAT_142e9696c: set to 4 when the region becomes CN; what reads it is not ported yet.
uint32_t s_regionVariant;

uint32_t CodeOf(const char* name) {
    uint32_t code;
    std::memcpy(&code, name, sizeof(code));
    return code;
}

}

// The locale id whose code is `code`, or LOCALE_NONE.
// ref: FUN_1405d5cf0
uint32_t LocaleFromCode(uint32_t code) {
    for (uint32_t i = 0; i < 12; i++) {
        if (s_localeNames[i] && CodeOf(s_localeNames[i]) == code) {
            return i;
        }
    }
    return LOCALE_NONE;
}

// The locale id named by a four-character string; enUS when it names none.
// ref: FUN_1405d5c40
uint32_t LocaleFromString(const char* locale) {
    uint32_t id = LocaleFromCode(CodeOf(locale));
    return id != LOCALE_NONE ? id : 0;
}

// The locale for a system locale code: an exact match, else by language ("de", "fr", ...),
// else by country ("DE", "MX", ...), else `fallback`.
// ref: FUN_1405d57c0
uint32_t LocaleFromCodeOrLanguage(uint32_t code, uint32_t fallback) {
    uint32_t id = LocaleFromCode(code);
    if (id != LOCALE_NONE) {
        return id;
    }
    if (code == 0x45496167) {  // gaIE
        return 0;
    }
    if (code == 0x4E4D6E6D) {  // mnMN
        return 4;
    }
    if (code == 0x53457565) {  // euES
        return 6;
    }
    if (code == 0x554C626C) {  // lbLU
        return 2;
    }

    char language0 = static_cast<char>(code);
    char language1 = static_cast<char>(code >> 8);
    switch (language0) {
    case 'd':
        if (language1 == 'e') {
            return 3;
        }
        break;
    case 'e':
        if (language1 == 'n') {
            return 0;
        }
        if (language1 == 's') {
            return 7;
        }
        break;
    case 'f':
        if (language1 == 'r') {
            return 2;
        }
        break;
    case 'i':
        if (language1 == 't') {
            return 11;
        }
        break;
    case 'k':
        if (language1 == 'o') {
            return 1;
        }
        break;
    case 'p':
        if (language1 == 't') {
            return 10;
        }
        break;
    case 'r':
        if (language1 == 'u') {
            return 8;
        }
        break;
    case 'z':
        if (language1 == 'h') {
            return 4;
        }
        break;
    }

    char country0 = static_cast<char>(code >> 16);
    char country1 = static_cast<char>(code >> 24);
    switch (country0) {
    case 'A':
        return country1 == 'U' ? 0 : fallback;
    case 'B':
        return country1 == 'R' ? 10 : fallback;
    case 'C':
        if (country1 == 'A') {
            return 0;
        }
        return country1 == 'N' ? 4 : fallback;
    case 'D':
        return country1 == 'E' ? 3 : fallback;
    case 'E':
        return country1 == 'S' ? 6 : fallback;
    case 'F':
        return country1 == 'R' ? 2 : fallback;
    case 'G':
        return country1 == 'B' ? 0 : fallback;
    case 'I':
        return country1 == 'T' ? 11 : fallback;
    case 'K':
        return country1 == 'R' ? 1 : fallback;
    case 'M':
        return country1 == 'X' ? 7 : fallback;
    case 'N':
        return country1 == 'Z' ? 0 : fallback;
    case 'P':
        return country1 == 'T' ? 10 : fallback;
    case 'R':
        return country1 == 'U' ? 8 : fallback;
    case 'T':
        return country1 == 'W' ? 5 : fallback;
    case 'U':
        return country1 == 'S' ? 0 : fallback;
    }
    return fallback;
}

// ref: FUN_1405d5a70
uint32_t LocaleFromSystem(uint32_t fallback) {
    return LocaleFromCodeOrLanguage(OsGetUserLocaleCode(), fallback);
}

// The locale's code as a big-endian four-character constant ('enUS' reads 0x656e5553).
// ref: FUN_1405d5b70
uint32_t LocaleToCode(uint32_t locale) {
    const char* name = s_localeNames[locale];
    if (!name) {
        return 0;
    }
    return ((((uint32_t(int8_t(name[0])) << 8) | uint32_t(int8_t(name[1]))) << 8 | uint32_t(int8_t(name[2]))) << 8)
        | uint32_t(int8_t(name[3]));
}

// ref: FUN_1405d5bc0
const char* LocaleGetName(uint32_t locale) {
    return s_localeNames[locale];
}

// ref: FUN_1405d5e30
void LocaleSetText(uint32_t locale) {
    s_textLocale = locale;
}

// ref: FUN_1405d5e00
void LocaleSetAudio(uint32_t locale) {
    s_audioLocale = locale;
}

// ref: FUN_1405d5790
uint32_t LocaleGetText() {
    return s_textLocale;
}

// ref: FUN_1405d5770
uint32_t LocaleGetAudio() {
    return s_audioLocale;
}

// The region id named `name` (case-insensitive, the first match), or 0x48.
// ref: FUN_1405d5d90
uint32_t RegionFromName(const char* name) {
    for (uint32_t i = 0; i < 0x100; i++) {
        if (s_regionNames[i] && _stricmp(name, s_regionNames[i]) == 0) {
            return i;
        }
    }
    return 0x48;
}

// The region the system's country suggests: KR 2, TW 4, CN and MN 5, most of Europe and
// Russia 3, the rest 1.
// ref: FUN_1405d5a90
uint32_t RegionFromSystem() {
    uint32_t code = OsGetUserLocaleCode();
    uint32_t country = (((code >> 8) & 0xFFFFFF20) | (code >> 24)) & 0xDFDF;
    if (country < 0x4955) {
        if (country == 0x4954) {
            return 3;
        }
        if (country > 0x4445) {
            if (country == 0x4553 || country == 0x4652 || country == 0x4742 || country == 0x4945) {
                return 3;
            }
            return 1;
        }
        if (country == 0x4445) {
            return 3;
        }
        if (country == 0x4155 || country == 0x4252 || country == 0x4341) {
            return 1;
        }
        return country == 0x434E ? 5 : 1;
    }
    if (country > 0x4E5A) {
        if (country == 0x5054 || country == 0x5255) {
            return 3;
        }
        return country == 0x5457 ? 4 : 1;
    }
    if (country == 0x4E5A) {
        return 1;
    }
    if (country == 0x4B52) {
        return 2;
    }
    if (country == 0x4C55) {
        return 3;
    }
    return country == 0x4D4E ? 5 : 1;
}

// ref: FUN_1405d5e10
void RegionSet(uint32_t region) {
    s_region = region;
    if (region == 5) {
        s_regionVariant = 4;
    }
}

// ref: FUN_1405d5780
uint32_t RegionGet() {
    return s_region;
}

namespace {

uint32_t s_fontGroup;  // DAT_142e9a060

}

// The font group a locale needs: 1 for koKR, frFR, zhCN, zhTW; 2 for ruRU; 0 otherwise.
// ref: FUN_141650820
uint32_t LocaleGetFontGroup(uint32_t locale) {
    if (locale == 1 || locale == 2 || locale == 4 || locale == 5) {
        return 1;
    }
    return locale == 8 ? 2 : 0;
}

// ref: FUN_140641c50
void LocaleSetFontGroup(uint32_t group) {
    s_fontGroup = group;
}

// The plural rule GlobalStrings lookups use (DAT_142e9a060).
uint32_t LocaleGetPluralRule() {
    return s_fontGroup;
}

namespace {

// DAT_1426c44f0: each locale's bit in the build manifest's locale flags.
const uint32_t s_casLocaleBits[12] = { 1, 2, 4, 5, 6, 8, 7, 12, 13, 0, 14, 15 };

}

// ref: FUN_1405d5bb0
uint32_t LocaleToCasIndex(uint32_t locale) {
    return s_casLocaleBits[locale];
}

// ref: FUN_1405d5bd0
uint32_t LocaleFromCasIndex(uint32_t index) {
    for (uint32_t locale = 0; locale < 12; locale++) {
        if (s_casLocaleBits[locale] == index) {
            return locale;
        }
    }
    return 0;
}

// ref: FUN_1405d5d80
const char* RegionGetName(uint32_t region) {
    return s_regionNames[region];
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142e9a060 = (void*)&s_fontGroup;
// ---- end of generated globals ----
