#ifndef STORM_STRING_HPP
#define STORM_STRING_HPP

#include <cstdarg>
#include <cstdint>
#include <cstdlib>

// Splits the next token off *string: skips leading whitespace, honours double quotes when
// '"' is among the whitespace characters, copies the token into buffer (bufferchars may be
// 0 to just skip it) and advances *string past it and one separator. *quoted is set when
// the token was quoted.
void SStrTokenize(const char** string, char* buffer, uint64_t bufferchars, const char* whitespace, int32_t* quoted);

// printf into a fixed buffer through Storm's own formatter.
void SStrPrintf(char* dest, uint64_t maxchars, const char* format, ...);

void SStrVPrintf(char* dest, uint64_t maxchars, const char* format, va_list args);

// Like SStrVPrintf, returning the characters written (0xFFFFFFFF for bad arguments).
uint64_t SStrVPrintfCount(char* dest, uint64_t maxchars, const char* format, va_list args);

// snprintf semantics: the full length the output would have, or -1.
int32_t SStrVSnprintf(char* dest, uint64_t size, const char* format, va_list args);

// SStrVPrintfCount that returns 0xFFFFFFFF on a bad format.
uint64_t SStrVPrintfChecked(char* dest, uint64_t maxchars, const char* format, va_list args);

// SStrPrintf into a char[260].
void SStrPrintf260(char* dest, const char* format, ...);

// SStrPrintf into a char[64] / char[128].
void SStrPrintf64(char* dest, const char* format, ...);
void SStrPrintf128(char* dest, const char* format, ...);

// Case-insensitive hash for Storm hash tables: decodes UTF-8, upper-cases (ASCII, Latin-1,
// and a few Latin Extended / Cyrillic letters), maps '/' to '\\', then hashes the result.
uint32_t SStrHashHT(const char* string);

// Bob Jenkins' lookup2 hash over a byte buffer.
uint32_t SStrHashBytes(const uint8_t* data, uint32_t length, uint32_t initval);

// FNV-1a of a string, the hash 3.4.3 passes beside __FILE__ to SMemAlloc. The reference
// computes it inline at every allocation site.
uint32_t SStrHashFnv1a(const char* string);

// Collating compare of two UTF-8 strings over at most `maxchars` characters: case and the
// accents on Latin letters are ignored (AE, OE and SS expand to two letters), with the raw
// code points breaking ties.
int32_t SStrCmpUTF8I(const char* a, const char* b, uint64_t maxchars);

// The number of UTF-8 characters in `string` (continuation bytes are not counted).
int32_t SStrLenUTF8(const char* string);

// Case-insensitive compare of two UTF-8 strings over at most `maxchars` characters, by a
// fold of ASCII, Latin-1 and Cyrillic letters (Cyrillic ordered with YO after YE); the
// difference of the first unequal folded characters, or 0.
int32_t SStrCmpUTF8Fold(const char* a, const char* b, int64_t maxchars);

// The first case-insensitive occurrence of `pattern` in `string` (_strnicmp at each position).
const char* SStrStrI(const char* string, const char* pattern);

// A copy of `string` in a new SMemAlloc block.
char* SStrDupA(const char* string, const char* tag, int32_t line);

// The process-wide "C" locale (created on first use).
_locale_t SStrGetCLocale();

// strtod in the "C" locale, narrowed to float: overflow clamps to +-FLT_MAX, anything that
// is not a normal float (zero, denormal, inf, NaN) gives 0.
float SStrToFloat(const char* string);

#endif
