#ifndef CLIENT_LOCALE_HPP
#define CLIENT_LOCALE_HPP

#include <cstdint>

// Locale ids index s_localeNames (0 enUS ... 11 itIT); LOCALE_NONE (12) is "not a locale".
// A locale code is its four characters read as a little-endian integer ('enUS' = 0x53556e65).
#define LOCALE_NONE 12
#define LOCALE_CODE_ENUS 0x53556E65

// Region ids index s_regionNames (1 US, 2 KR, 3 EU, 4 TW, 5 CN, ...); REGION_NONE (0x100)
// is "none given", 0x48 is what an unknown region name maps to.
#define REGION_NONE 0x100

uint32_t LocaleFromCode(uint32_t code);
uint32_t LocaleFromString(const char* locale);
uint32_t LocaleFromCodeOrLanguage(uint32_t code, uint32_t fallback);
uint32_t LocaleFromSystem(uint32_t fallback);
uint32_t LocaleToCode(uint32_t locale);
const char* LocaleGetName(uint32_t locale);

void LocaleSetText(uint32_t locale);
void LocaleSetAudio(uint32_t locale);
uint32_t LocaleGetText();
uint32_t LocaleGetAudio();
// A locale's bit index in the build manifest's locale flags, and back (0 when none matches).
uint32_t LocaleToCasIndex(uint32_t locale);
uint32_t LocaleFromCasIndex(uint32_t index);

uint32_t RegionFromName(const char* name);
uint32_t RegionFromSystem();
void RegionSet(uint32_t region);
uint32_t RegionGet();
const char* RegionGetName(uint32_t region);

uint32_t LocaleGetFontGroup(uint32_t locale);
void LocaleSetFontGroup(uint32_t group);

#endif
