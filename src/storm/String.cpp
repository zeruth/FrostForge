#include "storm/String.hpp"
#include "storm/Memory.hpp"
#include "util/Unimplemented.hpp"
#include <cfloat>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <cstring>

// ref: FUN_140254100
void SStrTokenize(const char** string, char* buffer, uint64_t bufferchars, const char* whitespace, int32_t* quoted) {
    bool quotes = std::strchr(whitespace, '"') != nullptr;
    const char* p = *string;
    int32_t wasQuoted = 0;
    bool inQuotes = false;

    // leading whitespace; an opening quote ends the skip and starts a quoted token
    while (*p != '\0' && std::strchr(whitespace, *p)) {
        if (quotes && *p == '"') {
            bool opening = !inQuotes;
            inQuotes = opening;
            wasQuoted = 1;
            if (opening) {
                p++;
                break;
            }
        }
        p++;
    }

    uint32_t length = 0;
    const char* end = p;
    for (;;) {
        char c = *p;
        if (c == '\0') {
            end = p;
            break;
        }
        if (quotes && c == '"') {
            if (length != 0 && !inQuotes) {
                end = p;
                break;
            }
            wasQuoted = 1;
            if (inQuotes) {
                end = p + 1;
                break;
            }
            inQuotes = true;
        } else {
            if (!inQuotes && std::strchr(whitespace, c)) {
                end = p + 1;
                break;
            }
            if (length + 1 < bufferchars) {
                buffer[length++] = c;
            }
        }
        p++;
    }

    if (length < bufferchars) {
        buffer[length] = '\0';
    }
    *string = end;
    if (quoted) {
        *quoted = wasQuoted;
    }
}

// ref: FUN_140254260
uint32_t SStrHashBytes(const uint8_t* k, uint32_t length, uint32_t c) {
    uint32_t a = 0x9E3779B9;
    uint32_t b = 0x9E3779B9;
    uint32_t len = length;

#define SSTR_MIX(a, b, c) \
    a -= b; a -= c; a ^= (c >> 13); \
    b -= c; b -= a; b ^= (a << 8); \
    c -= a; c -= b; c ^= (b >> 13); \
    a -= b; a -= c; a ^= (c >> 12); \
    b -= c; b -= a; b ^= (a << 16); \
    c -= a; c -= b; c ^= (b >> 5); \
    a -= b; a -= c; a ^= (c >> 3); \
    b -= c; b -= a; b ^= (a << 10); \
    c -= a; c -= b; c ^= (b >> 15);

    if (len > 11) {
        for (uint64_t blocks = length / 12u; blocks != 0; blocks--) {
            a += k[0] + (uint32_t(k[1]) << 8) + (uint32_t(k[2]) << 16) + (uint32_t(k[3]) << 24);
            b += k[4] + (uint32_t(k[5]) << 8) + (uint32_t(k[6]) << 16) + (uint32_t(k[7]) << 24);
            c += k[8] + (uint32_t(k[9]) << 8) + (uint32_t(k[10]) << 16) + (uint32_t(k[11]) << 24);
            SSTR_MIX(a, b, c);
            k += 12;
            len -= 12;
        }
    }

    c += length;
    switch (len) {
    case 11: c += uint32_t(k[10]) << 24; [[fallthrough]];
    case 10: c += uint32_t(k[9]) << 16; [[fallthrough]];
    case 9: c += uint32_t(k[8]) << 8; [[fallthrough]];
    case 8: b += uint32_t(k[7]) << 24; [[fallthrough]];
    case 7: b += uint32_t(k[6]) << 16; [[fallthrough]];
    case 6: b += uint32_t(k[5]) << 8; [[fallthrough]];
    case 5: b += k[4]; [[fallthrough]];
    case 4: a += uint32_t(k[3]) << 24; [[fallthrough]];
    case 3: a += uint32_t(k[2]) << 16; [[fallthrough]];
    case 2: a += uint32_t(k[1]) << 8; [[fallthrough]];
    case 1: a += k[0]; [[fallthrough]];
    default: break;
    }
    SSTR_MIX(a, b, c);
#undef SSTR_MIX
    return c;
}

namespace {

// DAT_14261f4d0 / DAT_14261f4b0: UTF-8 sequence length by lead byte, and the offset to
// subtract after accumulating a sequence (ConvertUTF's tables).
const uint8_t s_utf8Length[256] = {
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6,
};
const uint32_t s_utf8Offset[7] = { 0x0, 0x0, 0x3080, 0xE2080, 0x3C82080, 0xFA082080, 0x82082080 };

}

// ref: FUN_140252fc0
uint32_t SStrHashHT(const char* string) {
    uint8_t normalized[0x400];
    uint8_t* out = normalized;
    uint32_t length = 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(string);

    while (*p != 0 && length <= 0x3FB) {
        uint32_t kind = s_utf8Length[*p];
        uint32_t ch = 0;
        uint32_t upper = 0;
        bool complete = true;

        // accumulate the sequence: (kind - 1) continuation bytes after the lead byte, each
        // stage shifting the total left by 6 -- stopping early at a terminator
        if (kind >= 2) {
            ch = *p;
            for (uint32_t i = 1; i < kind; i++) {
                p++;
                if (*p == 0) {
                    complete = false;
                    break;
                }
                ch = (ch << 6) + *p;
            }
            p++;
        } else if (kind == 1) {
            ch = *p;
            p++;
        }

        if (complete) {
            ch -= s_utf8Offset[kind];
            if (ch > 0xFFFF) {
                ch = 0xFFFD;
            }
            if (kind == 1) {
                if (ch - 0x61 <= 0x19) {
                    ch -= 0x20;
                }
            } else if (kind == 2) {
                if (ch - 0xE0 < 0x1F) {
                    ch -= 0x20;
                } else if (ch == 0xFF) {
                    ch = 0x178;
                } else if (ch == 0x153) {
                    ch = 0x152;
                } else if (ch == 0x451) {
                    ch = 0x401;
                } else if (ch - 0x430 <= 0x1F) {
                    ch -= 0x20;
                }
            }
            upper = ch == '/' ? '\\' : ch;
        }

        // the reference stores the value's bytes, low first, at least one byte
        uint64_t value = upper;
        do {
            *out++ = static_cast<uint8_t>(value);
            length++;
            value >>= 8;
        } while (value != 0);
        if (!complete) {
            break;
        }
    }

    *out = 0;
    return SStrHashBytes(normalized, length, 0);
}

uint32_t SStrHashFnv1a(const char* string) {
    uint32_t hash = 0x811C9DC5;
    for (; *string; string++) {
        hash = (hash ^ static_cast<uint8_t>(*string)) * 0x1000193;
    }
    return hash;
}

// The "C" locale, created on first use and freed at exit.
// ref: FUN_1402528a0
_locale_t SStrGetCLocale() {
    static _locale_t s_locale = [] {
        _locale_t locale = _create_locale(LC_ALL, "C");
        std::atexit([] { _free_locale(s_locale); });
        return locale;
    }();
    return s_locale;
}

// Parses a float in the "C" locale. A finite nonzero result that overflows a float clamps to
// +-FLT_MAX; zero, denormals, infinities, NaN and anything below FLT_MIN give 0.
// ref: FUN_140254020
float SStrToFloat(const char* string) {
    double value = _strtod_l(string, nullptr, SStrGetCLocale());
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    uint32_t exponent = static_cast<uint32_t>(bits >> 52) & 0x7FF;
    if (exponent != 0 && exponent != 0x7FF) {
        double magnitude = std::fabs(value);
        if (magnitude > static_cast<double>(FLT_MAX)) {
            return value >= 0.0 ? FLT_MAX : -FLT_MAX;
        }
        if (magnitude >= static_cast<double>(FLT_MIN)) {
            return static_cast<float>(value);
        }
    }
    return 0.0f;
}

// Copies a string into a new SMemAlloc block (tag and line name the caller).
// ref: FUN_140252e20
char* SStrDupA(const char* string, const char* tag, int32_t line) {
    uint64_t length = std::strlen(string);
    auto copy = static_cast<char*>(SMemAlloc(length + 1, 7, tag, 0, line, 0));
    std::memcpy(copy, string, length + 1);
    return copy;
}

namespace {

// DAT_14261f4d0 / DAT_14261f4b0: a UTF-8 lead byte's sequence length (0 for the terminator;
// continuation bytes count as single characters), and what the sequence's marker bits add up
// to per length.
uint32_t CollateSequenceLength(uint8_t lead) {
    if (lead == 0) {
        return 0;
    }
    if (lead < 0xC0) {
        return 1;
    }
    if (lead < 0xE0) {
        return 2;
    }
    if (lead < 0xF0) {
        return 3;
    }
    if (lead < 0xF8) {
        return 4;
    }
    if (lead < 0xFC) {
        return 5;
    }
    return 6;
}

const uint32_t s_collateOffsets[7] = { 0x0, 0x0, 0x3080, 0xE2080, 0x3C82080, 0xFA082080, 0x82082080 };

// One character for SStrCmpUTF8I: its code point (clamped to 0xFFFD above the BMP; the
// partial value for a sequence cut short by the terminator), and its collation key - upper
// case, accented Latin capitals reduced to their base letter, and AE / OE / SS split into a
// key and a second letter that must be matched next.
void CollateNext(const uint8_t** cursor, uint32_t* raw, uint32_t* key, uint32_t* second) {
    const uint8_t* p = *cursor;
    uint32_t length = CollateSequenceLength(*p);
    *second = 0;

    uint32_t value = 0;
    if (length != 0) {
        for (uint32_t i = 0;; i++) {
            value += *p++;
            if (i + 1 == length) {
                break;
            }
            if (*p == 0) {
                // cut short: the partial value, no key
                *cursor = p;
                *raw = value;
                *key = 0;
                return;
            }
            value <<= 6;
        }
    }
    value -= s_collateOffsets[length];
    if (value > 0xFFFF) {
        value = 0xFFFD;
    }
    *cursor = p;
    *raw = value;

    uint32_t ch = value;
    if (length == 1) {
        if (ch - 0x61 <= 0x19) {
            ch -= 0x20;
        }
    } else if (length == 2) {
        if (ch - 0xE0 < 0x1F) {
            ch -= 0x20;
        } else if (ch == 0xFF) {
            ch = 0x178;
        } else if (ch == 0x153) {
            ch = 0x152;
        } else if (ch == 0x451) {
            ch = 0x401;
        } else if (ch - 0x430 <= 0x1F) {
            ch -= 0x20;
        }
    }

    if (ch - 0xC0 < 0x20) {
        if (ch < 0xC6) {
            ch = 'A';
        } else if (ch == 0xC6) {
            ch = 'A';
            *second = 'E';
        } else if (ch == 0xC7) {
            ch = 'C';
        } else if (ch - 0xC8 < 4) {
            ch = 'E';
        } else if (ch - 0xCC < 4) {
            ch = 'I';
        } else if (ch == 0xD1) {
            ch = 'N';
        } else if (ch - 0xD2 < 5) {
            ch = 'O';
        } else if (ch - 0xD9 < 4) {
            ch = 'U';
        } else if (ch == 0xDF) {
            ch = 'S';
            *second = 'S';
        }
    } else if (ch == 0x152) {
        ch = 'O';
        *second = 'E';
    } else if (ch == 0x178) {
        ch = 'Y';
    } else if (ch == 0x401) {
        ch = 0x415;
    }
    *key = ch;
}

}

// Compares up to `maxchars` characters of two UTF-8 strings by collation key (see
// CollateNext); equal keys fall back to the first difference in raw code points.
// ref: FUN_1402534d0
int32_t SStrCmpUTF8I(const char* a, const char* b, uint64_t maxchars) {
    auto pa = reinterpret_cast<const uint8_t*>(a);
    auto pb = reinterpret_cast<const uint8_t*>(b);
    int32_t tie = 0;
    for (;;) {
        if (*pa == 0 && *pb == 0) {
            return tie;
        }
        if (maxchars-- == 0) {
            return tie;
        }

        uint32_t rawA, keyA, secondA;
        uint32_t rawB, keyB, secondB;
        CollateNext(&pa, &rawA, &keyA, &secondA);
        CollateNext(&pb, &rawB, &keyB, &secondB);
        if (tie == 0) {
            tie = static_cast<int32_t>(rawA - rawB);
        }
        if (keyA != keyB) {
            return static_cast<int32_t>(keyA - keyB);
        }

        // a pending second letter on one side is matched against the other side's next key
        while (secondA != secondB) {
            uint32_t raw;
            if (secondA == 0) {
                uint32_t pending = secondB;
                CollateNext(&pa, &raw, &keyA, &secondA);
                secondB = 0;
                if (keyA != pending) {
                    return static_cast<int32_t>(keyA - pending);
                }
            } else {
                uint32_t pending = secondA;
                CollateNext(&pb, &raw, &keyB, &secondB);
                secondA = 0;
                if (pending != keyB) {
                    return static_cast<int32_t>(pending - keyB);
                }
            }
        }
    }
}

// ref: FUN_140253ef0
const char* SStrStrI(const char* string, const char* pattern) {
    size_t length = std::strlen(pattern);
    for (; *string != '\0'; string++) {
        if (_strnicmp(string, pattern, length) == 0) {
            return string;
        }
    }
    return nullptr;
}

// ref: FUN_140253320
int32_t SStrLenUTF8(const char* string) {
    int32_t count = 0;
    for (auto p = reinterpret_cast<const uint8_t*>(string); *p != 0; p++) {
        if ((*p & 0xC0) != 0x80) {
            count++;
        }
    }
    return count;
}

// The characters in at most `bytes` bytes of a UTF-8 string.
// ref: FUN_140253350
int32_t SStrLenUTF8N(const char* string, int64_t bytes) {
    int32_t count = 0;
    for (auto p = reinterpret_cast<const uint8_t*>(string); *p != 0 && bytes != 0; p++, bytes--) {
        if ((*p & 0xC0) != 0x80) {
            count++;
        }
    }
    return count;
}

namespace {

// The sequence length of each lead byte (0 for the terminator) and what each length's
// accumulated value is offset by (0x14261f4d0, 0x14261f4b0).
const uint8_t s_foldLength[256] = {
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6,
};
const uint32_t s_foldOffset[7] = { 0, 0, 0x3080, 0xE2080, 0x3C82080, 0xFA082080, 0x82082080 };

// Decodes and folds the character at *cursor and advances past it. A sequence cut short by the
// terminator folds to 0, with the cursor left on the terminator; the terminator itself folds to
// 0 without advancing.
uint32_t FoldNext(const uint8_t** cursor) {
    const uint8_t* p = *cursor;
    uint8_t length = s_foldLength[*p];
    uint32_t value = 0;
    for (uint8_t i = 0; i < length; i++) {
        if (i != 0) {
            if (*p == 0) {
                *cursor = p;
                return 0;
            }
            value <<= 6;
        }
        value += *p++;
    }
    *cursor = p;
    value -= s_foldOffset[length];
    if (value > 0xFFFF) {
        value = 0xFFFD;
    }
    if (length == 1) {
        if (value - 0x61 < 0x1A) {
            value -= 0x20;
        }
    } else if (length == 2) {
        if (value - 0xE0 < 0x1F) {
            value -= 0x20;
        } else if (value == 0xFF) {
            value = 0x178;
        } else if (value == 0x153) {
            value = 0x152;
        } else if (value == 0x401 || value == 0x451) {
            value = 0x415;
        } else if (value - 0x410 < 6) {
            value -= 1;
        } else if (value - 0x430 < 0x20) {
            value = (value - (value < 0x436 ? 1 : 0)) - 0x20;
        }
    }
    return value;
}

}  // namespace

// ref: FUN_140252aa0
int32_t SStrCmpUTF8Fold(const char* a, const char* b, int64_t maxchars) {
    auto pa = reinterpret_cast<const uint8_t*>(a);
    auto pb = reinterpret_cast<const uint8_t*>(b);
    for (;;) {
        if ((*pa == 0 && *pb == 0) || maxchars == 0) {
            return 0;
        }
        uint32_t ka = FoldNext(&pa);
        uint32_t kb = FoldNext(&pb);
        maxchars--;
        if (ka != kb) {
            return static_cast<int32_t>(ka - kb);
        }
    }
}
