// Storm's printf engine, as 3.4.3 has it: a format parser (0x140260060) that supports
// positional arguments (%n$), a per-conversion formatter (0x14025e2c0), a writer object the
// output goes through, and Ryu (d2d) for floating-point digits. Every function here is
// transcribed from the reference; the Ryu tables are read out of the binary
// (tools/gen_ryu_tables.py).

#include "storm/String.hpp"
#include "storm/string/RyuTables.hpp"
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <intrin.h>

namespace {

// ---- the writer -------------------------------------------------------------------------

// vtable 0x142620760: write a run, repeat a character, write a run of 16-bit characters
class SStrWriter {
    public:
    virtual void Write(const char* text, int64_t count) = 0;
    virtual void Fill(char c, int64_t count) = 0;
    virtual void WriteWide(const uint16_t* text, int64_t count) = 0;
};

class SStrBufferWriter : public SStrWriter {
    public:
    char* m_start;
    char* m_cur;
    uint64_t m_size;

    // ref: FUN_140260e40
    void Write(const char* text, int64_t count) override {
        while (count != 0) {
            count--;
            if (this->m_size <= static_cast<uint64_t>(this->m_cur - this->m_start)) {
                return;
            }
            *this->m_cur++ = *text++;
        }
    }

    // ref: FUN_140260e00
    void Fill(char c, int64_t count) override {
        while (count != 0) {
            count--;
            if (this->m_size <= static_cast<uint64_t>(this->m_cur - this->m_start)) {
                return;
            }
            *this->m_cur++ = c;
        }
    }

    // ref: FUN_140260e80
    void WriteWide(const uint16_t* text, int64_t count) override {
        while (count != 0) {
            count--;
            if (this->m_size <= static_cast<uint64_t>((this->m_cur - this->m_start) >> 1)) {
                return;
            }
            *reinterpret_cast<uint16_t*>(this->m_cur) = *text++;
            this->m_cur += 2;
        }
    }
};

// ---- a parsed conversion ----------------------------------------------------------------

enum : int32_t {
    LEN_NONE = 0, LEN_H = 1, LEN_L = 2, LEN_LL = 3, LEN_BIGL = 4, LEN_I = 6, LEN_I32 = 7, LEN_I64 = 8,
    LEN_Q = 9, LEN_HH = 10, LEN_Z = 11, LEN_J = 12, LEN_T = 13,
};

enum : uint8_t {
    FLAG_LEFT = 0x1, FLAG_PLUS = 0x2, FLAG_ZERO = 0x4, FLAG_SPACE = 0x8, FLAG_ALT = 0x10,
    FLAG_WIDTH_ARG = 0x40, FLAG_PRECISION_ARG = 0x80,
};

struct SPEC {
    SPEC* nextByIndex;    // 0x00 (positional mode: sorted by argument index)
    SPEC* nextInOrder;    // 0x08 (positional mode: format order)
    const char* start;    // 0x10
    int64_t length;       // 0x18
    int32_t width;        // 0x20 (-1 none, -2 from an argument)
    int32_t precision;    // 0x24
    char conversion;      // 0x28
    int32_t argclass;     // 0x2C
    int32_t lengthmod;    // 0x30
    uint8_t flags;        // 0x34
    int8_t index;         // 0x35 (-1: not positional)
};
static_assert(sizeof(SPEC) == 0x38, "a format spec is 0x38 bytes");

// DAT_1426206f0 / DAT_142620728: argument class by length modifier
const int32_t s_intClass[14] = { 4, 3, 5, 6, 0, 0, 15, 7, 8, 6, 1, 15, 9, 14 };
const int32_t s_floatClass[14] = { 10, 0, 10, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

void ResetSpec(SPEC* spec) {
    spec->nextByIndex = nullptr;
    spec->nextInOrder = nullptr;
    spec->length = 0;
    spec->width = -1;
    spec->precision = -1;
    spec->conversion = '\0';
    spec->argclass = 0;
    spec->lengthmod = 0;
    spec->flags = 0;
    spec->index = -1;
}

bool SpecHasSize(const SPEC* spec) {
    return spec->width != -1 || spec->precision != -1 || spec->lengthmod != 0 || spec->conversion != '\0';
}

// ---- floating-point helpers -------------------------------------------------------------

// 0 normal/zero, -1 normal (non-zero exponent), -2 subnormal, 1/2 infinity (sign), 3 signalling
// NaN, 4 quiet NaN with the "indefinite" pattern.
// ref: FUN_140267870
int32_t ClassifyDouble(uint64_t bits) {
    uint32_t high = static_cast<uint32_t>(bits >> 32);
    uint32_t low = static_cast<uint32_t>(bits);
    if ((bits >> 32 & 0x7FF00000) == 0) {
        if ((bits >> 32 & 0xFFFFF) == 0) {
            return low != 0 ? -2 : 0;
        }
        return -2;
    }
    if ((high & 0x7FF00000) != 0x7FF00000) {
        return -1;
    }
    if ((bits & 0xFFFFF00000000ull) == 0 && low == 0) {
        return static_cast<int32_t>((high & 0xFFFFF) + 1);
    }
    if ((high >> 19 & 1) == 0) {
        return 3;
    }
    if (static_cast<int64_t>(bits) < 0 && low == 0) {
        return (high & 0xFFFFF) == 0x80000 ? 4 : 2;
    }
    return 2;
}

// ref: FUN_140268040
int32_t DecimalLength17(uint64_t v) {
    if (v > 9999999999999999ull) return 17;
    if (v > 999999999999999ull) return 16;
    if (v > 99999999999999ull) return 15;
    if (v > 9999999999999ull) return 14;
    if (v > 999999999999ull) return 13;
    if (v > 99999999999ull) return 12;
    if (v > 9999999999ull) return 11;
    if (v > 999999999ull) return 10;
    if (v > 99999999ull) return 9;
    if (v > 9999999ull) return 8;
    if (v > 999999ull) return 7;
    if (v > 99999ull) return 6;
    if (v > 9999ull) return 5;
    if (v > 999ull) return 4;
    if (v > 99ull) return 3;
    return v < 10 ? 1 : 2;
}

// ref: FUN_140268160
bool MultipleOfPowerOf5(uint64_t value, uint32_t p) {
    uint32_t count = 0;
    for (;;) {
        uint64_t q = value / 5;
        uint32_t r = static_cast<uint32_t>(value) - 5 * static_cast<uint32_t>(q);
        if (r != 0) {
            break;
        }
        value = q;
        count++;
    }
    return count >= p;
}

uint64_t MulShift64(uint64_t m, const uint64_t* mul, int32_t shift) {
    uint64_t high1;
    uint64_t low1 = _umul128(m, mul[1], &high1);
    uint64_t high0;
    _umul128(m, mul[0], &high0);
    uint64_t sum = high0 + low1;
    if (sum < high0) {
        high1++;
    }
    return __shiftright128(sum, high1, static_cast<uint8_t>(shift - 64));
}

struct FLOATDECIMAL {
    uint64_t mantissa;
    int32_t exponent;
};

// Ryu's d2d: the shortest decimal that round-trips.
// ref: FUN_140267a90
FLOATDECIMAL RyuD2D(uint64_t ieeeMantissa, uint32_t ieeeExponent) {
    int32_t e2 = ieeeExponent == 0 ? -0x434 : static_cast<int32_t>(ieeeExponent) - 0x435;
    uint64_t m2 = ieeeExponent == 0 ? ieeeMantissa : (ieeeMantissa | 0x10000000000000ull);
    bool even = (m2 & 1) == 0;
    uint64_t mv = m2 * 4;
    uint32_t mmShift = (ieeeMantissa != 0 || ieeeExponent <= 1) ? 1 : 0;

    uint64_t vr, vp, vm;
    int32_t e10;
    bool vmIsTrailingZeros = false;
    bool vrIsTrailingZeros = false;

    if (e2 >= 0) {
        uint32_t q = ((static_cast<uint32_t>(e2) * 0x13441) >> 18) - (e2 > 3 ? 1 : 0);
        e10 = static_cast<int32_t>(q);
        int32_t shift = static_cast<int32_t>((q * 0x12934F) >> 19) - e2 + static_cast<int32_t>(q) + 0x3A + 64;
        vr = MulShift64(mv, s_ryuPow5InvSplit[q], shift);
        vp = MulShift64(mv + 2, s_ryuPow5InvSplit[q], shift);
        vm = MulShift64(mv - 1 - mmShift, s_ryuPow5InvSplit[q], shift);
        if (q <= 21) {
            if (static_cast<uint32_t>(mv) - 5 * static_cast<uint32_t>(mv / 5) == 0) {
                vrIsTrailingZeros = MultipleOfPowerOf5(mv, q);
            } else if (even) {
                vmIsTrailingZeros = MultipleOfPowerOf5(mv - 1 - mmShift, q);
            } else {
                vp -= MultipleOfPowerOf5(mv + 2, q) ? 1 : 0;
            }
        }
    } else {
        uint32_t q = ((static_cast<uint32_t>(-e2) * 0xB2EFB) >> 20) - (-e2 > 1 ? 1 : 0);
        e10 = static_cast<int32_t>(q) + e2;
        int32_t i = -e2 - static_cast<int32_t>(q);
        int32_t shift = static_cast<int32_t>(q) - static_cast<int32_t>((static_cast<uint32_t>(i) * 0x12934F) >> 19) + 0x38 + 64;
        vr = MulShift64(mv, s_ryuPow5Split[i], shift);
        vp = MulShift64(mv + 2, s_ryuPow5Split[i], shift);
        vm = MulShift64(mv - 1 - mmShift, s_ryuPow5Split[i], shift);
        if (q <= 1) {
            vrIsTrailingZeros = true;
            if (even) {
                vmIsTrailingZeros = mmShift == 1;
            } else {
                vp--;
            }
        } else if (q < 63) {
            vrIsTrailingZeros = (mv & ((1ull << q) - 1)) == 0;
        }
    }

    int32_t removed = 0;
    uint8_t lastRemovedDigit = 0;
    uint64_t output;

    if (vmIsTrailingZeros || vrIsTrailingZeros) {
        for (;;) {
            uint64_t vpDiv10 = vp / 10;
            uint64_t vmDiv10 = vm / 10;
            if (vpDiv10 <= vmDiv10) {
                break;
            }
            uint32_t vmMod10 = static_cast<uint32_t>(vm) - 10 * static_cast<uint32_t>(vmDiv10);
            uint64_t vrDiv10 = vr / 10;
            uint32_t vrMod10 = static_cast<uint32_t>(vr) - 10 * static_cast<uint32_t>(vrDiv10);
            vmIsTrailingZeros &= vmMod10 == 0;
            vrIsTrailingZeros &= lastRemovedDigit == 0;
            lastRemovedDigit = static_cast<uint8_t>(vrMod10);
            vr = vrDiv10;
            vp = vpDiv10;
            vm = vmDiv10;
            removed++;
        }
        if (vmIsTrailingZeros) {
            for (;;) {
                uint64_t vmDiv10 = vm / 10;
                uint32_t vmMod10 = static_cast<uint32_t>(vm) - 10 * static_cast<uint32_t>(vmDiv10);
                if (vmMod10 != 0) {
                    break;
                }
                uint64_t vpDiv10 = vp / 10;
                uint64_t vrDiv10 = vr / 10;
                uint32_t vrMod10 = static_cast<uint32_t>(vr) - 10 * static_cast<uint32_t>(vrDiv10);
                vrIsTrailingZeros &= lastRemovedDigit == 0;
                lastRemovedDigit = static_cast<uint8_t>(vrMod10);
                vr = vrDiv10;
                vp = vpDiv10;
                vm = vmDiv10;
                removed++;
            }
        }
        if (vrIsTrailingZeros && lastRemovedDigit == 5 && vr % 2 == 0) {
            lastRemovedDigit = 4;
        }
        output = vr + (((vr == vm && (!even || !vmIsTrailingZeros)) || lastRemovedDigit >= 5) ? 1 : 0);
    } else {
        bool roundUp = false;
        uint64_t vpDiv100 = vp / 100;
        uint64_t vmDiv100 = vm / 100;
        if (vpDiv100 > vmDiv100) {
            uint64_t vrDiv100 = vr / 100;
            uint32_t vrMod100 = static_cast<uint32_t>(vr) - 100 * static_cast<uint32_t>(vrDiv100);
            roundUp = vrMod100 >= 50;
            vr = vrDiv100;
            vp = vpDiv100;
            vm = vmDiv100;
            removed += 2;
        }
        for (;;) {
            uint64_t vpDiv10 = vp / 10;
            uint64_t vmDiv10 = vm / 10;
            if (vpDiv10 <= vmDiv10) {
                break;
            }
            uint64_t vrDiv10 = vr / 10;
            uint32_t vrMod10 = static_cast<uint32_t>(vr) - 10 * static_cast<uint32_t>(vrDiv10);
            roundUp = vrMod10 >= 5;
            vr = vrDiv10;
            vp = vpDiv10;
            vm = vmDiv10;
            removed++;
        }
        output = vr + ((vr == vm || roundUp) ? 1 : 0);
    }

    return { output, e10 + removed };
}

// The value's digits as an integer of `digits` significant digits (rounded half up from the
// shortest representation), returning the decimal exponent of the first digit.
// ref: FUN_1402681c0
int32_t RoundToDigits(uint64_t bits, uint64_t* mantissa, int32_t digits) {
    uint32_t ieeeExponent = static_cast<uint32_t>(bits >> 52) & 0x7FF;
    if (ieeeExponent == 0 && (bits & 0xFFFFFFFFFFFFFull) == 0) {
        *mantissa = 0;
        return 0;
    }

    uint64_t m = (bits & 0xFFFFFFFFFFFFFull) | 0x10000000000000ull;
    int32_t exponent;
    uint8_t shift = static_cast<uint8_t>(0x33 - static_cast<uint8_t>(ieeeExponent)) & 0x3F;
    if (ieeeExponent - 0x3FF < 0x35 && (m & ((1ull << shift) - 1)) == 0) {
        // an integer: shift out the fraction and strip trailing zeros (Ryu's small-int path)
        m >>= shift;
        exponent = 0;
        while (static_cast<uint32_t>(m) == static_cast<uint32_t>(m / 10) * 10) {
            m /= 10;
            exponent++;
        }
    } else {
        FLOATDECIMAL d = RyuD2D(bits & 0xFFFFFFFFFFFFFull, ieeeExponent);
        exponent = d.exponent;
        m = d.mantissa;
    }

    *mantissa = m;
    int32_t length = DecimalLength17(m);
    int32_t first = length + exponent - 1;

    if (length < digits) {
        *mantissa = m * s_pow10Run[digits - length];
    } else if (digits < length) {
        if ((length - digits) - 1 < 0x12) {
            uint64_t scaled = m / s_pow10Run[length - digits - 1];
            uint64_t kept = scaled / 10;
            *mantissa = kept;
            uint8_t next = static_cast<uint8_t>(scaled) - static_cast<uint8_t>(kept) * 10;
            if (next > 4 && (first < 0x134 || kept < s_pow10Run[17 + digits])) {
                kept++;
                *mantissa = kept;
                if (digits != 0 && kept == s_pow10Run[digits]) {
                    *mantissa = kept / 10;
                    first = length + exponent;
                }
            }
        } else {
            *mantissa = 0;
        }
    }
    return first;
}

// The argument's value narrowed to its class.
// ref: FUN_140260f00
void UnsignedForClass(int32_t argclass, uint64_t value, uint64_t* out) {
    *out = 0;
    switch (argclass) {
    case 1:
        *out = value & 0xFF;
        return;
    case 2:
    case 3:
        *out = value & 0xFFFF;
        return;
    case 4:
    case 5:
    case 7:
        *out = value & 0xFFFFFFFF;
        return;
    case 6:
    case 8:
    case 9:
    case 13:
    case 14:
    case 15:
        *out = value;
        return;
    default:
        return;
    }
}

uint64_t DoubleBits(double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

double BitsDouble(uint64_t bits) {
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// ---- formatting one conversion ----------------------------------------------------------

// ref: FUN_14025e2c0
void FormatSpec(SPEC* spec, uint64_t value, SStrWriter* writer) {
    int32_t width = spec->width;
    uint8_t buffer[34];
    uint8_t* end = buffer + 33;  // digits are written backwards from here
    uint8_t* digits = end;
    *end = 0;

    char pad = ' ';
    uint32_t length = 0;           // characters in `text`
    uint32_t total = 0;            // everything except width padding
    int32_t exponent = 0;          // local_a4
    uint8_t conv = static_cast<uint8_t>(spec->conversion);
    uint8_t lower = conv & 0x20;   // 0x20 for lower-case conversions
    uint8_t kind = conv | 0x20;    // local_res20[0]
    int32_t fieldWidth = width != -1 ? width : 0;
    int32_t pointMode = 0;         // local_a0: 0 none, 1 point inside, 2 "0." prefix, 3 point after zeros
    int32_t pointAt = 0;           // local_9c
    int32_t trailingZeros = 0;     // local_ac
    int32_t leadingZeros = 0;      // local_a8
    bool signedValue = false;      // bVar1
    bool negative = false;         // local_90
    bool hexPrefix = false;        // local_b7
    int32_t precision = 1;         // iVar23
    const char* text = nullptr;
    char exponentText[5];

    switch (conv) {
    case 'A':
    case 'a': {
        signedValue = true;
        precision = spec->precision == -1 ? 13 : spec->precision;
        int32_t cls = ClassifyDouble(value);
        negative = static_cast<int64_t>(value) < 0;
        if (cls == 2 || cls == 1) {
            text = cls == 2 ? (lower ? "nan" : "NAN") : (lower ? "inf" : "INF");
            length = 3;
        } else if (cls == 3) {
            text = lower ? "nan(snan)" : "NAN(SNAN)";
            length = 9;
        } else if (cls == 4) {
            text = lower ? "nan(ind)" : "NAN(IND)";
            length = 8;
        } else {
            uint64_t bits = value;
            if (static_cast<int64_t>(bits) < 0) {
                bits ^= 0x8000000000000000ull;
            }
            int32_t e = 0;
            double fraction = std::frexp(BitsDouble(bits), &e);
            exponent = e;
            if (BitsDouble(bits) != 0.0) {
                exponent--;
            }
            uint64_t mantissa = DoubleBits(fraction) << 12;
            if (precision != 0) {
                mantissa += 0x8000000000000000ull >> ((precision * 4) & 0x3F);
            }
            uint8_t* out = reinterpret_cast<uint8_t*>(exponentText);  // reuse below; build in buffer
            (void)out;
            uint8_t* p = buffer;
            *p++ = '0';
            *p++ = lower | 'X';
            bool point = false;
            if (BitsDouble(bits) == 0.0) {
                *p++ = '0';
                point = true;
            } else if (precision != 0 || mantissa < 0x8000000000000000ull) {
                *p++ = '1';
                point = true;
            } else {
                *p++ = '2';
            }
            if (point && precision != 0) {
                *p++ = '.';
                do {
                    precision--;
                    uint8_t nibble = static_cast<uint8_t>(mantissa >> 60);
                    *p++ = nibble < 10 ? (nibble | '0') : static_cast<uint8_t>((nibble + 0x37) | lower);
                    mantissa <<= 4;
                } while (precision != 0);
            }
            *p++ = lower | 'P';
            int32_t e10 = exponent;
            if (e10 < 0) {
                *p++ = '-';
                e10 = -e10;
            } else {
                *p++ = '+';
            }
            if (e10 >= 1000) {
                *p++ = static_cast<uint8_t>((e10 / 1000) | '0');
                e10 %= 1000;
                *p++ = static_cast<uint8_t>((e10 / 100) | '0');
                e10 %= 100;
                *p++ = static_cast<uint8_t>((e10 / 10) | '0');
                e10 %= 10;
            } else if (e10 > 99) {
                *p++ = static_cast<uint8_t>((e10 / 100) | '0');
                e10 %= 100;
                *p++ = static_cast<uint8_t>((e10 / 10) | '0');
                e10 %= 10;
            } else if (e10 > 9) {
                *p++ = static_cast<uint8_t>((e10 / 10) | '0');
                e10 %= 10;
            }
            *p++ = static_cast<uint8_t>(e10 | '0');
            *p = 0;
            length = static_cast<uint32_t>(p - buffer);
            text = reinterpret_cast<const char*>(buffer);
            exponent = exponent;  // reported as the "exponent" for the common tail
        }
        break;
    }

    default:
        value = conv;
        [[fallthrough]];
    case 'c':
        writer->Fill(static_cast<char>(value), 1);
        return;

    case 'C': {
        int32_t c = static_cast<char>(value);
        if (static_cast<uint16_t>(value) > 0xFF) {
            c = '*';
        }
        writer->Fill(static_cast<char>(c), 1);
        return;
    }

    case 'E':
    case 'F':
    case 'G':
    case 'e':
    case 'f':
    case 'g': {
        signedValue = true;
        precision = spec->precision == -1 ? 6 : spec->precision;
        int32_t cls = ClassifyDouble(value);
        negative = static_cast<int64_t>(value) < 0;
        if (cls >= 1 && cls <= 4) {
            if (cls == 2 || cls == 1) {
                text = cls == 2 ? (lower ? "nan" : "NAN") : (lower ? "inf" : "INF");
                length = 3;
            } else if (cls == 3) {
                text = lower ? "nan(snan)" : "NAN(SNAN)";
                length = 9;
            } else {
                text = lower ? "nan(ind)" : "NAN(IND)";
                length = 8;
            }
            hexPrefix = false;
            kind = 'f';
            exponent = 0;
            total = length;
            goto sign;
        }

        uint64_t bits = value;
        if (static_cast<int64_t>(bits) < 0) {
            bits ^= 0x8000000000000000ull;
        }
        int32_t estimate = 0;
        if (BitsDouble(bits) != 0.0) {
            estimate = static_cast<int32_t>(std::floor(std::log10(BitsDouble(bits))));
        }

        uint8_t flags = spec->flags;
        bool keepZeros = true;  // bVar25: false for %g without '#'
        int32_t fraction = precision;
        if (kind == 'g') {
            if (precision == 0) {
                precision = 1;
            }
            if (estimate < -4 || precision <= estimate) {
                kind = 'e';
                fraction = precision;
            } else {
                fraction = precision - estimate;
            }
            fraction--;
            keepZeros = (flags & FLAG_ALT) != 0;
        }

        int32_t want = fraction + 1;
        if (kind != 'e') {
            want += estimate;
        }
        if (want > 17) {
            want = 17;
        }

        uint64_t mantissa;
        exponent = RoundToDigits(bits, &mantissa, want);
        if (exponent != estimate && (spec->conversion | 0x20) == 'g') {
            if (kind == 'e') {
                if ((exponent == -4 && estimate == -5) || (fraction <= exponent && estimate < fraction)) {
                    kind = 'f';
                }
            } else if (exponent < -4 || precision <= exponent) {
                fraction = precision - 1;
                kind = 'e';
            }
        }

        do {
            digits--;
            *digits = static_cast<uint8_t>((mantissa % 10) | '0');
            mantissa /= 10;
        } while (mantissa != 0);
        length = static_cast<uint32_t>(end - digits);
        int32_t count = static_cast<int32_t>(length);
        if (want <= count && want < count) {
            exponent += count - want;
        }

        if (kind == 'e') {
            pointMode = (fraction != 0 || (flags & FLAG_ALT)) ? 1 : 0;
            pointAt = 0;
            if (count - 1 < fraction) {
                trailingZeros = (fraction - count) + 1;
                total = pointMode + trailingZeros;
            } else {
                total = pointMode;
            }
        } else {
            int32_t after = (count - 1) - exponent;
            if (!keepZeros) {
                if (after < 0) {
                    trailingZeros = -after;
                }
            } else if (after < fraction) {
                trailingZeros = fraction - (kind == 'g' ? count : after);
            }
            bool point = (flags & FLAG_ALT) == 0 && exponent + 1 < count;
            if (kind == 'g') {
                point = false;
                uint8_t* last = end - 1;
                for (; after != 0; after--) {
                    if (*last != '0') {
                        point = true;
                        break;
                    }
                    if (!keepZeros) {
                        *last = 0;
                        last--;
                        count--;
                    }
                }
                length = static_cast<uint32_t>(count);
            }

            bool withPoint = (flags & FLAG_ALT) != 0 || (!keepZeros ? point : spec->precision != 0);
            total = trailingZeros;
            if (withPoint) {
                total = trailingZeros + 1;
                if (exponent < 0) {
                    leadingZeros = -exponent;
                    pointMode = 2;
                    total += leadingZeros;
                } else if (count - 1 < exponent) {
                    pointMode = 3;
                    pointAt = (exponent - count) + 1;
                } else {
                    pointMode = 1;
                    pointAt = exponent;
                }
            }
        }

        // %g without '#': drop trailing zeros after the point
        if (!keepZeros && (pointMode == 1 || pointMode == 2)) {
            uint8_t* first = end - static_cast<int32_t>(length) + (pointMode == 1 ? pointAt : 0);
            uint8_t* last = end - 1;
            for (; length != 0 && *last == '0' && first <= last; last--) {
                *last = 0;
                length--;
            }
            if (length == 1 && pointMode == 1 && (flags & FLAG_ALT) == 0) {
                pointMode = 0;
            }
        }

        pad = (spec->flags & (FLAG_LEFT | FLAG_ZERO)) == FLAG_ZERO ? '0' : ' ';
        text = reinterpret_cast<const char*>(digits);
        fieldWidth = width != -1 ? width : 0;
        break;
    }

    case 'P':
    case 'X':
    case 'p':
    case 'x': {
        uint64_t v;
        UnsignedForClass(spec->argclass, value, &v);
        if ((conv == 'p' || conv == 'P') && width == -1) {
            spec->flags |= FLAG_ZERO;
            fieldWidth = 16;
        }
        int32_t minimum = spec->precision == -1 ? 1 : spec->precision;
        int32_t remaining = minimum;
        if (minimum > 0x1F) {
            remaining = 0x1F;
            fieldWidth += 0x1F - minimum;
        }
        leadingZeros = minimum >= 0x20 ? minimum - 0x1F : 0;
        while (v != 0 || remaining != 0) {
            uint8_t nibble = static_cast<uint8_t>(v) & 0xF;
            digits--;
            *digits = nibble < 10 ? (nibble | '0') : static_cast<uint8_t>((nibble + 0x37) | lower);
            if (remaining != 0) {
                remaining--;
            }
            v >>= 4;
        }
        length = static_cast<uint32_t>(end - digits);
        hexPrefix = (spec->flags & FLAG_ALT) != 0;
        pad = (spec->flags & (FLAG_LEFT | FLAG_ZERO)) == FLAG_ZERO ? '0' : ' ';
        text = reinterpret_cast<const char*>(digits);
        break;
    }

    case 'd':
    case 'i': {
        int32_t requested = spec->precision;
        negative = static_cast<int64_t>(value) < 0;
        signedValue = true;
        uint64_t magnitude = negative ? static_cast<uint64_t>(-static_cast<int64_t>(value)) : value;
        int32_t minimum = 1;
        int32_t remaining = 1;
        if (requested != -1) {
            minimum = requested;
            remaining = requested;
            if (requested > 0x1F) {
                remaining = 0x1F;
                fieldWidth = (fieldWidth - requested) + 0x1F;
            }
        }
        leadingZeros = minimum >= 0x20 ? minimum - 0x1F : 0;
        while (magnitude != 0 || remaining != 0) {
            digits--;
            *digits = static_cast<uint8_t>((magnitude % 10) | '0');
            magnitude /= 10;
            if (remaining != 0) {
                remaining--;
            }
        }
        length = static_cast<uint32_t>(end - digits);
        text = reinterpret_cast<const char*>(digits);
        pad = (spec->flags & (FLAG_LEFT | FLAG_ZERO)) == FLAG_ZERO ? '0' : ' ';
        break;
    }

    case 'o':
    case 'u': {
        uint64_t v;
        UnsignedForClass(spec->argclass, value, &v);
        int32_t minimum = spec->precision == -1 ? 1 : spec->precision;
        int32_t remaining = minimum;
        if (minimum > 0x1F) {
            remaining = 0x1F;
            fieldWidth = (fieldWidth - minimum) + 0x1F;
        }
        leadingZeros = minimum >= 0x20 ? minimum - 0x1F : 0;
        uint64_t base = conv == 'u' ? 10 : 8;
        while (v != 0 || remaining != 0) {
            digits--;
            *digits = static_cast<uint8_t>((v % base) | '0');
            if (remaining != 0) {
                remaining--;
            }
            v /= base;
        }
        length = static_cast<uint32_t>(end - digits);
        text = reinterpret_cast<const char*>(digits);
        pad = (spec->flags & (FLAG_LEFT | FLAG_ZERO)) == FLAG_ZERO ? '0' : ' ';
        break;
    }

    case 's': {
        int32_t limit = spec->precision;
        const char* string = reinterpret_cast<const char*>(value);
        if (!string) {
            text = "(null)";
            length = 6;
            if (limit != -1 && limit < 6) {
                length = static_cast<uint32_t>(limit);
            }
        } else {
            text = string;
            if (*string != '\0') {
                const char* p = string;
                if (limit == -1) {
                    do {
                        p++;
                        length++;
                    } while (*p != '\0');
                } else {
                    do {
                        p++;
                        if (limit <= static_cast<int32_t>(length)) {
                            break;
                        }
                        length++;
                    } while (*p != '\0');
                }
            }
        }
        break;
    }
    }

    total += length;
    if (kind == 'e') {
        total += (exponent > 99 ? 1 : 0) + 4;
    }

sign:
    char signChar = '\0';
    if (signedValue) {
        if (negative) {
            signChar = '-';
        } else if (spec->flags & FLAG_PLUS) {
            signChar = '+';
        } else if (spec->flags & FLAG_SPACE) {
            signChar = ' ';
        }
        if (signChar != '\0') {
            total++;
            if (pad == '0') {
                writer->Fill(signChar, 1);
            }
        }
    }

    if (hexPrefix) {
        writer->Fill('0', 1);
        writer->Fill(static_cast<char>(lower | 'X'), 1);
    }
    if ((spec->flags & FLAG_LEFT) == 0 && static_cast<int32_t>(fieldWidth - total) > 0) {
        writer->Fill(pad, static_cast<int32_t>(fieldWidth - total));
        fieldWidth = static_cast<int32_t>(total);
    }
    if (signChar != '\0' && pad == ' ') {
        writer->Fill(signChar, 1);
    }
    if (leadingZeros > 0) {
        writer->Fill('0', 1);
        if (pointMode == 2) {
            writer->Fill('.', 1);
        }
        writer->Fill('0', leadingZeros - 1);
    }
    if (pointMode == 1) {
        writer->Write(text, pointAt + 1);
        writer->Fill('.', 1);
        writer->Write(text + pointAt + 1, static_cast<int32_t>((length - pointAt) - 1));
    } else {
        writer->Write(text, static_cast<int32_t>(length));
    }
    if (trailingZeros > 0) {
        int32_t zeros = pointMode == 3 ? pointAt : trailingZeros;
        writer->Fill('0', zeros);
        if (pointMode == 3) {
            writer->Fill('.', 1);
            writer->Fill('0', trailingZeros - zeros);
        }
    }
    if (kind == 'e') {
        exponentText[0] = static_cast<char>(lower | 'E');
        int32_t e = exponent;
        if (e < 0) {
            exponentText[1] = '-';
            e = -e;
        } else {
            exponentText[1] = '+';
        }
        bool three = e > 99;
        int32_t rest = e;
        if (three) {
            rest = e / 10;
            exponentText[4] = static_cast<char>((e - rest * 10) | '0');
        }
        exponentText[2] = static_cast<char>(((rest / 10) % 10) | '0');
        exponentText[3] = static_cast<char>((rest % 10) | '0');
        writer->Write(exponentText, three ? 5 : 4);
    }
    if ((spec->flags & FLAG_LEFT) != 0 && static_cast<int32_t>(fieldWidth - total) > 0) {
        writer->Fill(' ', static_cast<int32_t>(fieldWidth - total));
    }
}

// ---- parsing ----------------------------------------------------------------------------

uint32_t ParseNumber(const char*& p) {
    // decimal digits from *p onward; overflow beyond 0xCCCCCCB makes the result -1
    uint32_t value = static_cast<uint32_t>(*p - '0');
    bool overflow = false;
    while (p[1] >= '0' && p[1] <= '9') {
        if (value > 0xCCCCCCB) {
            overflow = true;
        }
        value = static_cast<uint32_t>(p[1]) + (value * 5 - 0x18) * 2;
        p++;
    }
    return overflow ? 0xFFFFFFFF : value;
}

// Fetches the next argument of a class from a va_list.
uint64_t NextArgument(int32_t argclass, va_list& args) {
    switch (argclass) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 7:
        return static_cast<uint64_t>(static_cast<int64_t>(va_arg(args, int32_t)));
    case 6:
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
        return va_arg(args, uint64_t);
    default:
        return 0;
    }
}

}

// Parses `format` and writes through `writer`. With `positional` the arguments may be
// referenced as %n$; they are all fetched first, then the output is produced in format order.
// Returns 0, or ERROR_INVALID_PARAMETER (0x57) for a malformed format.
// ref: FUN_140260060
uint32_t SStrFormat(const char* format, va_list args, bool positional, SStrWriter* writer) {
    if (!format || *format == '\0') {
        return 0;
    }

    SPEC specs[12];
    uint32_t specCount = 0;
    SPEC* spec = nullptr;
    bool inSpec = false;
    bool complete = false;
    bool malformed = false;
    SPEC* byIndexHead = nullptr;
    SPEC* byIndexTail = nullptr;
    SPEC* inOrderHead = nullptr;
    SPEC* inOrderTail = nullptr;
    uint32_t argumentCount = 0;
    const char* p = format;
    char c = *p;

    for (;;) {
        if (inSpec) {
            switch (c) {
            case ' ':
                if (SpecHasSize(spec)) malformed = true;
                spec->flags |= FLAG_SPACE;
                break;
            case '#':
                if (SpecHasSize(spec)) malformed = true;
                spec->flags |= FLAG_ALT;
                break;
            case '%':
                spec->length = (p + 1) - spec->start;
                if (spec->length == 2) {
                    spec->start++;
                    spec->conversion = c;
                    complete = true;
                    break;
                }
                return 0x57;
            case '+':
                if (SpecHasSize(spec)) malformed = true;
                spec->flags |= FLAG_PLUS;
                break;
            case '-':
                if (SpecHasSize(spec)) goto bad;
                spec->flags |= FLAG_LEFT;
                break;
            case '.':
                if (spec->width == -1) {
                    spec->width = 0;
                }
                if ((p[1] < '0' || p[1] > '9') && p[1] != '*') {
                    spec->precision = 0;
                }
                break;
            case '0':
                if (spec->width == -1) {
                    if (spec->precision != -1 || spec->lengthmod != 0 || spec->conversion != '\0') malformed = true;
                    spec->flags |= FLAG_ZERO;
                    break;
                }
                [[fallthrough]];
            case '*':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9': {
                bool fromArgument = false;
                uint32_t number;
                char next;
                if (c == '*') {
                    next = p[1];
                    p++;
                    if (next < '0' || next > '9') {
                        number = 0xFFFFFFFE;
                    } else {
                        number = ParseNumber(p);
                        fromArgument = true;
                        next = p[1];
                        if (number == 0xFFFFFFFF || static_cast<int32_t>(number) > 0xC || next != '$' || !positional) {
                            return 0x57;
                        }
                        number--;
                        next = p[2];
                        p += 2;
                    }
                } else {
                    number = ParseNumber(p);
                    p++;
                    next = *p;
                    if (number == 0xFFFFFFFF) {
                        return 0x57;
                    }
                }

                if (next == '$') {
                    if (spec->index != -1 || spec->flags != 0 || SpecHasSize(spec)) goto bad;
                    if (positional && static_cast<int32_t>(number) < 0xD) {
                        spec->index = static_cast<int8_t>(number - 1);
                        break;
                    }
                    return 0x57;
                }
                if (next == '.') {
                    if (spec->width == -1 && spec->precision == -1 && spec->lengthmod == 0 && spec->conversion == '\0') {
                        spec->width = static_cast<int32_t>(number);
                        if (fromArgument) {
                            spec->flags |= FLAG_WIDTH_ARG;
                        }
                        p--;
                        break;
                    }
                } else {
                    p--;
                    if (spec->precision == -1) {
                        if (spec->width == -1) {
                            if (spec->lengthmod == 0 && spec->conversion == '\0') {
                                spec->width = static_cast<int32_t>(number);
                                if (fromArgument) {
                                    spec->flags |= FLAG_WIDTH_ARG;
                                }
                                break;
                            }
                        } else if (spec->lengthmod == 0 && spec->conversion == '\0') {
                            spec->precision = static_cast<int32_t>(number);
                            if (fromArgument) {
                                spec->flags |= FLAG_PRECISION_ARG;
                            }
                            break;
                        }
                    }
                }
                goto bad;
            }
            case 'A':
            case 'E':
            case 'F':
            case 'G':
            case 'a':
            case 'e':
            case 'f':
            case 'g':
                if (spec->lengthmod == LEN_BIGL) {
                    return 0x57;
                }
                spec->conversion = c;
                spec->argclass = s_floatClass[spec->lengthmod];
                if (spec->argclass == 0) {
                    return 0x57;
                }
                complete = true;
                break;
            case 'C':
                if ((spec->lengthmod & ~2) != 0) {
                    return 0x57;
                }
                spec->argclass = 2;
                spec->conversion = c;
                complete = true;
                break;
            case 'I': {
                char next = p[1];
                int32_t mod;
                if (next < '0' || next > '9') {
                    mod = LEN_I;
                } else {
                    p++;
                    uint32_t number = ParseNumber(p);
                    if (number == 0x20) {
                        mod = LEN_I32;
                    } else if (number == 0x40) {
                        mod = LEN_I64;
                    } else {
                        return 0x57;
                    }
                }
                spec->lengthmod = mod;
                break;
            }
            case 'L':
                if (spec->lengthmod != 0 || spec->conversion != '\0') malformed = true;
                spec->lengthmod = LEN_BIGL;
                break;
            case 'S':
                if ((spec->lengthmod & ~2) != 0) {
                    return 0x57;
                }
                spec->argclass = 12;
                spec->conversion = c;
                complete = true;
                break;
            case 'X':
            case 'd':
            case 'i':
            case 'o':
            case 'u':
            case 'x':
                spec->conversion = c;
                spec->argclass = s_intClass[spec->lengthmod];
                if (spec->argclass == 0) {
                    return 0x57;
                }
                complete = true;
                break;
            case 'c':
                if (static_cast<uint32_t>(spec->lengthmod) >= 2) {
                    return 0x57;
                }
                spec->argclass = 1;
                spec->conversion = c;
                complete = true;
                break;
            case 'h':
                if (spec->lengthmod != 0 || spec->conversion != '\0') goto bad;
                if (p[1] == 'h') {
                    p++;
                    spec->lengthmod = LEN_HH;
                } else {
                    spec->lengthmod = LEN_H;
                }
                break;
            case 'j':
                if (spec->lengthmod != 0 || spec->conversion != '\0') malformed = true;
                spec->lengthmod = LEN_J;
                break;
            case 'l':
                if (spec->lengthmod != 0 || spec->conversion != '\0') goto bad;
                if (p[1] == 'l') {
                    p++;
                    spec->lengthmod = LEN_LL;
                } else {
                    spec->lengthmod = LEN_L;
                }
                break;
            case 'p':
                if (spec->lengthmod != 0) {
                    return 0x57;
                }
                spec->argclass = 13;
                spec->conversion = c;
                complete = true;
                break;
            case 'q':
                if (spec->lengthmod != 0 || spec->conversion != '\0') malformed = true;
                spec->lengthmod = LEN_Q;
                break;
            case 's':
                if (static_cast<uint32_t>(spec->lengthmod) > 1) {
                    return 0x57;
                }
                spec->argclass = 11;
                spec->conversion = c;
                complete = true;
                break;
            case 't':
                if (spec->lengthmod != 0 || spec->conversion != '\0') malformed = true;
                spec->lengthmod = LEN_T;
                break;
            case 'w':
                if (spec->lengthmod != 0 || spec->conversion != '\0') malformed = true;
                spec->lengthmod = LEN_L;
                break;
            case 'z':
                if (spec->lengthmod != 0 || spec->conversion != '\0') malformed = true;
                spec->lengthmod = LEN_Z;
                break;
            default:
                return 0x57;
            }

            if (malformed) {
            bad:
                ResetSpec(spec);
                spec->start = nullptr;
                spec->conversion = c;
                return 0x57;
            }

            if (complete) {
                complete = false;
                // %c/%C/%s/%S follow the 'l'/'w' and 'h' modifiers to their wide/narrow twin
                char conversion = spec->conversion;
                if (conversion == 'c') {
                    if (spec->lengthmod == LEN_L || spec->lengthmod == 5) {
                        spec->conversion = 'C';
                        spec->argclass = 2;
                    }
                } else if (conversion == 'C') {
                    if (spec->lengthmod == LEN_H) {
                        spec->conversion = 'c';
                        spec->argclass = 1;
                    }
                } else if (conversion == 's') {
                    if (spec->lengthmod == LEN_L || spec->lengthmod == 5) {
                        spec->conversion = 'S';
                        spec->argclass = 12;
                    }
                } else if (conversion == 'S' && spec->lengthmod == LEN_H) {
                    spec->conversion = 's';
                    spec->argclass = 11;
                }

                if (!positional) {
                    if (spec->width == -2) {
                        spec->width = va_arg(args, int32_t);
                    }
                    if (spec->precision == -2) {
                        spec->precision = va_arg(args, int32_t);
                    }
                    uint64_t value = NextArgument(spec->argclass, args);
                    FormatSpec(spec, value, writer);
                    inSpec = false;
                } else {
                    spec->length = (p + 1) - spec->start;
                    if (!inOrderHead) {
                        inOrderHead = spec;
                    } else {
                        inOrderTail->nextInOrder = spec;
                    }
                    inOrderTail = spec;
                    spec->nextInOrder = nullptr;
                    uint32_t used = argumentCount++;
                    if (spec->flags & FLAG_WIDTH_ARG) {
                        argumentCount = used + 2;
                    }
                    if (spec->flags & FLAG_PRECISION_ARG) {
                        argumentCount++;
                    }
                    if (spec->conversion != '%') {
                        if (!byIndexHead) {
                            byIndexHead = spec;
                            byIndexTail = spec;
                            spec->nextByIndex = nullptr;
                        } else if ((byIndexHead->index != -1) == (spec->index != -1)) {
                            if (spec->index == -1) {
                                byIndexTail->nextByIndex = spec;
                                byIndexTail = spec;
                                spec->nextByIndex = nullptr;
                            } else {
                                SPEC* before = nullptr;
                                SPEC* at = byIndexHead;
                                while (at && spec->index > at->index) {
                                    before = at;
                                    at = at->nextByIndex;
                                }
                                if (!before) {
                                    byIndexHead = spec;
                                } else {
                                    before->nextByIndex = spec;
                                }
                                spec->nextByIndex = at;
                                if (!at) {
                                    byIndexTail = spec;
                                }
                            }
                        }
                    }
                    spec = nullptr;
                    inSpec = false;
                }
            }
        } else if (c == '%') {
            inSpec = true;
            if (!spec) {
                if (specCount > 0xB) {
                    return 0x57;
                }
                spec = &specs[specCount++];
            }
            ResetSpec(spec);
            spec->start = p;
        } else if (!positional) {
            writer->Fill(c, 1);
        }

        c = p[1];
        p++;
        if (c == '\0') {
            break;
        }
    }

    if (positional) {
        // number the unnumbered specs in order, then check the numbering has no gaps
        int64_t arguments[12];
        if (byIndexHead) {
            int8_t next = byIndexHead->index == -1 ? 0 : -1;
            for (SPEC* s = byIndexHead; s; s = s->nextByIndex) {
                if (s->index == -1) {
                    if (s->width == -2) {
                        s->flags |= FLAG_WIDTH_ARG;
                        s->width = next++;
                    }
                    if (s->precision == -2) {
                        s->flags |= FLAG_PRECISION_ARG;
                        s->precision = next++;
                    }
                    s->index = next++;
                } else {
                    int32_t previous = next;
                    next = s->index;
                    if (s->index - previous > 1) {
                        return 0x57;
                    }
                }
            }
        }

        uint32_t fetched = 0;
        int64_t slot = 0;
        for (SPEC* s = byIndexHead; s; s = s->nextByIndex) {
            while (slot <= s->index) {
                int64_t value = 0;
                if (s->index == static_cast<int32_t>(fetched)) {
                    switch (s->argclass) {
                    case 1: case 2: case 3: case 4: case 5: case 7:
                        value = static_cast<int64_t>(va_arg(args, int32_t));
                        break;
                    case 6: case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15:
                        value = va_arg(args, int64_t);
                        break;
                    default:
                        break;
                    }
                } else {
                    value = static_cast<int64_t>(va_arg(args, int32_t));
                }
                arguments[slot] = value;
                fetched++;
                slot++;
            }
        }
        while (fetched < argumentCount) {
            arguments[fetched++] = static_cast<int64_t>(va_arg(args, int32_t));
        }

        const char* q = format;
        SPEC* s = inOrderHead;
        while (*q != '\0') {
            if (!s) {
                writer->Fill(*q++, 1);
                continue;
            }
            for (int64_t n = s->start - q; n != 0; n--) {
                writer->Fill(*q++, 1);
            }
            if (s->index != -1) {
                if (s->flags & FLAG_WIDTH_ARG) {
                    s->width = static_cast<int32_t>(arguments[s->width]);
                }
                if (s->flags & FLAG_PRECISION_ARG) {
                    s->precision = static_cast<int32_t>(arguments[s->precision]);
                }
                FormatSpec(s, static_cast<uint64_t>(arguments[static_cast<uint8_t>(s->index)]), writer);
            }
            q += s->length;
            s = s->nextInOrder;
        }
    }

    return 0;
}

// ref: FUN_1402610c0
uint64_t SStrVPrintfCount(char* dest, uint64_t maxchars, const char* format, va_list args) {
    if (!dest || !format || maxchars == 0) {
        return 0xFFFFFFFF;
    }
    SStrBufferWriter writer;
    writer.m_start = dest;
    writer.m_cur = dest;
    writer.m_size = maxchars;
    SStrFormat(format, args, false, &writer);
    uint64_t written = static_cast<uint64_t>(writer.m_cur - writer.m_start);
    if (writer.m_size <= written) {
        writer.m_cur = writer.m_start + (writer.m_size - 1);
        written = writer.m_size - 1;
    }
    *writer.m_cur = '\0';
    return written;
}

void SStrVPrintf(char* dest, uint64_t maxchars, const char* format, va_list args) {
    SStrVPrintfCount(dest, maxchars, format, args);
}

// SStrVPrintfCount's twin that reports a bad format: 0xFFFFFFFF, the buffer left as written.
// ref: FUN_140265ea0
uint64_t SStrVPrintfChecked(char* dest, uint64_t maxchars, const char* format, va_list args) {
    if (dest && format && maxchars != 0) {
        SStrBufferWriter writer;
        writer.m_start = dest;
        writer.m_cur = dest;
        writer.m_size = maxchars;
        if (SStrFormat(format, args, false, &writer) == 0) {
            uint64_t written = static_cast<uint64_t>(writer.m_cur - writer.m_start);
            if (writer.m_size <= written) {
                writer.m_cur = writer.m_start + (writer.m_size - 1);
                written = writer.m_size - 1;
            }
            *writer.m_cur = '\0';
            return written;
        }
    }
    return 0xFFFFFFFF;
}

// ref: FUN_1415293a0
void SStrPrintf260(char* dest, const char* format, ...) {
    va_list args;
    va_start(args, format);
    SStrVPrintfChecked(dest, 0x104, format, args);
    va_end(args);
}

namespace {

// vtable 0x142620780: the buffer writer that also counts everything it was handed, for
// snprintf's full-length result. Its wide-run slot (0x140260d50, shared with the counting
// writers whose count lives at +0x08) adds the count to the buffer start; reproduced as is.
class SStrCountingWriter : public SStrWriter {
    public:
    char* m_start;      // 0x08
    char* m_cur;        // 0x10
    uint64_t m_size;    // 0x18
    uint64_t m_total;   // 0x20

    // ref: FUN_140260db0
    void Write(const char* text, int64_t count) override {
        this->m_total += count;
        if (!this->m_start) {
            return;
        }
        while (count != 0) {
            count--;
            if (this->m_size <= static_cast<uint64_t>(this->m_cur - this->m_start)) {
                return;
            }
            *this->m_cur++ = *text++;
        }
    }

    // ref: FUN_140260d60
    void Fill(char c, int64_t count) override {
        this->m_total += count;
        if (!this->m_start) {
            return;
        }
        while (count != 0) {
            count--;
            if (this->m_size <= static_cast<uint64_t>(this->m_cur - this->m_start)) {
                return;
            }
            *this->m_cur++ = c;
        }
    }

    // ref: FUN_140260d50
    void WriteWide(const uint16_t* text, int64_t count) override {
        (void)text;
        this->m_start += count;
    }
};

}

// snprintf: writes at most size - 1 characters and terminates, returning the length the
// whole output would have; -1 for a null format, a null buffer with a size, or a bad format.
// ref: FUN_140265e00
int32_t SStrVSnprintf(char* dest, uint64_t size, const char* format, va_list args) {
    if (format && (dest || size == 0)) {
        SStrCountingWriter writer;
        writer.m_total = 0;
        writer.m_start = dest;
        writer.m_cur = dest;
        writer.m_size = size;
        if (SStrFormat(format, args, false, &writer) == 0) {
            if (writer.m_size != 0) {
                if (writer.m_size <= writer.m_total) {
                    writer.m_start[writer.m_size - 1] = '\0';
                    return static_cast<int32_t>(writer.m_total);
                }
                *writer.m_cur = '\0';
            }
            return static_cast<int32_t>(writer.m_total);
        }
    }
    return -1;
}

// ref: FUN_1402534b0
void SStrPrintf(char* dest, uint64_t maxchars, const char* format, ...) {
    va_list args;
    va_start(args, format);
    SStrVPrintfCount(dest, maxchars, format, args);
    va_end(args);
}

// A fixed 64-byte destination; the call site's buffer is a char[64].
// ref: FUN_140216f20
void SStrPrintf64(char* dest, const char* format, ...) {
    va_list args;
    va_start(args, format);
    SStrVPrintfCount(dest, 0x40, format, args);
    va_end(args);
}

// The same with a fixed 128-byte destination.
// ref: FUN_1402544f0
void SStrPrintf128(char* dest, const char* format, ...) {
    va_list args;
    va_start(args, format);
    SStrVPrintfCount(dest, 0x80, format, args);
    va_end(args);
}
