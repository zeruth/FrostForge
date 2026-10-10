#ifndef BASE_CHAR_CLASS_HPP
#define BASE_CHAR_CLASS_HPP

// Character classes the text code asks about (FUN_142431160 and its neighbours).

#include <cstdint>

// Hangul jamo, compatibility jamo and syllables (FUN_142431160).
bool CharIsKorean(uint32_t code);
// CJK ideographs, bopomofo and the compatibility ideographs that are unified (FUN_1424311a0).
bool CharIsChinese(uint32_t code);
// Cyrillic (FUN_142431320).
bool CharIsCyrillic(uint32_t code);
// Latin letters: ASCII and Latin-1 (FUN_1424313a0).
bool CharIsLatin(uint32_t code);
// White space, from the reference's table (FUN_142431460).
bool CharIsSpace(uint32_t code);
// ASCII letters (FUN_142431340).
bool CharIsAsciiLetter(uint32_t code);
// The Russian alphabet: U+0410..U+044F, Yo and yo (FUN_142431410).
bool CharIsRussian(uint32_t code);
// The Hangul syllables the reference's table counts as letters (FUN_142431360).
bool CharIsHangulLetter(uint32_t code);
// A letter of any alphabet a name or an alphabetic edit box may hold (FUN_141650900).
bool CharIsAlphabetic(uint32_t code);
// Whether text is well-formed UTF-8 without control characters (other than tab, newline and
// return), surrogates or noncharacters (FUN_141650f10).
bool TextIsValid(const char* text);

#endif
