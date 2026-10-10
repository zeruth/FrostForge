#ifndef STORM_UNICODE_HPP
#define STORM_UNICODE_HPP

#include <cstdint>

// UTF-8 -> UTF-16 into a fixed buffer (ConvertUTF's algorithm, without validation: a bad
// sequence decodes to whatever its bits add up to). destSize is in characters; sourceBytes
// 0x7FFFFFFF means "until the terminator". Returns 0 when the terminator was converted, 1
// when dest is full, -1 - <trailing bytes> when the source ended mid-character (-1 at a
// boundary), and -1 for no source. Its Storm name is not known.
int32_t UnicodeConvertUtf8ToUtf16(wchar_t* dest, uint32_t destSize, const char* source, uint32_t sourceBytes,
    int32_t* destChars, int32_t* sourceUsed);

// The UTF-16 units UTF-8 `source` converts to, its terminator counted (-1 when the source ends
// first, -1 - <trailing bytes> mid-character).
int32_t UnicodeCountUtf8ToUtf16(const char* source, uint32_t sourceBytes, int32_t* sourceUsed);

// UTF-16 -> UTF-8 into a fixed buffer (ConvertUTF's algorithm). sourceChars 0x7FFFFFFF
// means "until the terminator". Returns 0 when the terminator was converted, the length of
// the character that did not fit (the output is terminated) when dest is full, or -1 when
// the source ended first. *destBytes / *sourceUsed report how far each side got.
int32_t UnicodeConvertUtf16ToUtf8(char* dest, uint32_t destSize, const uint16_t* source, uint32_t sourceChars,
    int32_t* destBytes, int32_t* sourceUsed);

// Decodes the UTF-8 character at `string`: its code point, 0x80000000 for a bad sequence
// (a stray continuation byte, or a lead byte without its continuation bytes), and
// 0xFFFFFFFF at the terminator or for null. *bytes gets the bytes it covered.
uint32_t SUniSGetUTF8(const uint8_t* string, int32_t* bytes);

// Encodes `ch` as UTF-8 (up to six bytes, the old 31-bit form) at `out` and terminates it;
// returns the end of the encoding. A value from 0x80000000 up writes only the terminator.
uint8_t* SUniSPutUTF8(uint32_t ch, uint8_t* out);

// The code point at `string`, like SUniSGetUTF8 but 0x80000000 for a malformed, overlong,
// surrogate or out-of-range sequence; 0xffffffff at the end (FUN_14024dcf0).
uint32_t SUniSGetUTF8Strict(const uint8_t* string, int32_t* bytes);

#endif
