#include "storm/Unicode.hpp"

// ref: FUN_14024d4f0
int32_t UnicodeConvertUtf16ToUtf8(char* dest, uint32_t destSize, const uint16_t* source, uint32_t sourceChars,
    int32_t* destBytes, int32_t* sourceUsed) {
    // DAT_14261e5a0
    static const uint32_t s_firstByteMark[7] = { 0x00, 0x00, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC };

    const uint16_t* end = sourceChars == 0x7FFFFFFF
        ? reinterpret_cast<const uint16_t*>(~uintptr_t(0))
        : source + sourceChars;
    const uint16_t* in = source;
    char* out = dest;
    char* outEnd = dest + destSize;
    int32_t result = 0;

    if (in >= end) {
        result = -1;
        goto done;
    }

    for (;;) {
        uint32_t ch = *in;
        uint32_t step = 1;
        uint32_t length;

        if (ch >= 0xD800 && ch <= 0xDBFF) {
            if (in + 1 >= end) {
                result = -1;
                goto done;
            }
            uint32_t low = in[1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                ch = (ch << 10) + 0xFCA02400 + low;  // ((ch - 0xD800) << 10) + (low - 0xDC00) + 0x10000
                step = 2;
            } else {
                length = 3;  // a lone high surrogate is encoded as itself
                goto encode;
            }
        } else if (ch > 0xDBFF) {
            length = ch < 0x10000 ? 3 : 0;
            if (length) {
                goto encode;
            }
        }

        if (ch < 0x80) {
            length = 1;
            if (ch == 0) {
                if (out < outEnd) {
                    *out++ = '\0';
                    result = 0;
                } else {
                    result = 1;
                }
                goto done;
            }
        } else if (ch < 0x800) {
            length = 2;
        } else if (ch < 0x10000) {
            length = 3;
        } else if (ch < 0x200000) {
            length = 4;
        } else if (ch < 0x4000000) {
            length = 5;
        } else if (ch <= 0x7FFFFFFF) {
            length = 6;
        } else {
            ch = 0xFFFD;
            length = 2;
        }

    encode:
        if (out + length >= outEnd) {
            *out = '\0';
            result = static_cast<int32_t>(length);
            goto done;
        }
        {
            char* p = out + length;
            for (uint32_t i = length; i > 1; i--) {
                *--p = static_cast<char>((ch & 0x3F) | 0x80);
                ch >>= 6;
            }
            *--p = static_cast<char>(static_cast<uint8_t>(ch) | s_firstByteMark[length]);
            out = p + length;
        }

        in += step;
        if (in >= end) {
            result = -1;
            goto done;
        }
    }

done:
    if (sourceUsed) {
        *sourceUsed = static_cast<int32_t>(in - source);
    }
    if (destBytes) {
        *destBytes = static_cast<int32_t>(out - dest);
    }
    return result;
}

namespace {

// DAT_14261e4a0: trailing bytes after each lead byte
const uint8_t s_trailingBytes[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5,
};
// DAT_14261e480: what the lead and trailing bytes' marker bits add up to, per sequence length
const uint32_t s_offsets[6] = { 0x00000000, 0x00003080, 0x000E2080, 0x03C82080, 0xFA082080, 0x82082080 };

}

// ref: FUN_14024d840
int32_t UnicodeConvertUtf8ToUtf16(wchar_t* dest, uint32_t destSize, const char* source, uint32_t sourceBytes,
    int32_t* destChars, int32_t* sourceUsed) {
    if (!source || sourceBytes == 0) {
        return -1;
    }

    auto in = reinterpret_cast<const uint8_t*>(source);
    const uint8_t* end = sourceBytes == 0x7FFFFFFF
        ? reinterpret_cast<const uint8_t*>(~uintptr_t(0))
        : in + sourceBytes;
    wchar_t* out = dest;
    wchar_t* outEnd = dest + destSize;
    int32_t result = 0;

    for (;;) {
        if (in >= end) {
            result = -1;
            break;
        }
        uint32_t trailing = s_trailingBytes[*in];
        if (in + trailing >= end) {
            result = -1 - static_cast<int32_t>(trailing);
            break;
        }

        uint32_t ch = 0;
        for (uint32_t i = 0; i < trailing; i++) {
            ch = (ch + in[i]) << 6;
        }
        ch += in[trailing];
        ch -= s_offsets[trailing];

        if (out >= outEnd) {
            result = 1;
            break;
        }
        if (ch < 0x10000) {
            *out++ = static_cast<wchar_t>(ch);
            if (ch == 0) {
                break;
            }
        } else if (ch < 0x110000) {
            if (out + 1 >= outEnd) {
                result = 1;
                break;
            }
            out[0] = static_cast<wchar_t>(((ch - 0x10000) >> 10) - 0x2800);
            out[1] = static_cast<wchar_t>(((ch - 0x10000) & 0x3FF) + 0xDC00);
            out += 2;
        } else {
            *out++ = static_cast<wchar_t>(0xFFFD);
        }
        in += trailing + 1;
    }

    if (sourceUsed) {
        *sourceUsed = static_cast<int32_t>(in - reinterpret_cast<const uint8_t*>(source));
    }
    if (destChars) {
        *destChars = static_cast<int32_t>(out - dest);
    }
    return result;
}

// ref: FUN_14024de70
uint8_t* SUniSPutUTF8(uint32_t ch, uint8_t* out) {
    if (!out) {
        return out;
    }
    auto byte = static_cast<uint8_t>(ch);
    if (ch < 0x80) {
        out[0] = byte;
        out[1] = 0;
        return out + 1;
    }
    if (ch < 0x800) {
        out[0] = static_cast<uint8_t>(ch >> 6) | 0xC0;
        out[1] = (byte & 0x3F) | 0x80;
        out[2] = 0;
        return out + 2;
    }
    if (ch < 0x10000) {
        out[0] = static_cast<uint8_t>(ch >> 12) | 0xE0;
        out[3] = 0;
        out[1] = (static_cast<uint8_t>(ch >> 6) & 0x3F) | 0x80;
        out[2] = (byte & 0x3F) | 0x80;
        return out + 3;
    }
    if (ch < 0x200000) {
        out[0] = static_cast<uint8_t>(ch >> 18) | 0xF0;
        out[4] = 0;
        out[1] = (static_cast<uint8_t>(ch >> 12) & 0x3F) | 0x80;
        out[2] = (static_cast<uint8_t>(ch >> 6) & 0x3F) | 0x80;
        out[3] = (byte & 0x3F) | 0x80;
        return out + 4;
    }
    auto top = static_cast<uint8_t>(ch >> 24);
    if (ch < 0x400000) {
        out[0] = top | 0xF8;
        out[5] = 0;
        out[1] = (static_cast<uint8_t>(ch >> 18) & 0x3F) | 0x80;
        out[2] = (static_cast<uint8_t>(ch >> 12) & 0x3F) | 0x80;
        out[3] = (static_cast<uint8_t>(ch >> 6) & 0x3F) | 0x80;
        out[4] = (byte & 0x3F) | 0x80;
        return out + 5;
    }
    if (ch < 0x80000000) {
        out[0] = (top >> 6) | 0xFC;
        out[6] = 0;
        out[1] = (top & 0x3F) | 0x80;
        out[2] = (static_cast<uint8_t>(ch >> 18) & 0x3F) | 0x80;
        out[3] = (static_cast<uint8_t>(ch >> 12) & 0x3F) | 0x80;
        out[4] = (static_cast<uint8_t>(ch >> 6) & 0x3F) | 0x80;
        out[5] = (byte & 0x3F) | 0x80;
        return out + 6;
    }
    out[0] = 0;
    return out;
}

// ref: FUN_14024dbd0
uint32_t SUniSGetUTF8(const uint8_t* string, int32_t* bytes) {
    if (bytes) {
        *bytes = 0;
    }
    if (!string) {
        return 0xFFFFFFFF;
    }

    uint8_t lead = *string++;
    if (lead == 0) {
        return 0xFFFFFFFF;
    }
    if (bytes) {
        (*bytes)++;
    }

    uint32_t ch = lead;
    int32_t trailing;
    if ((lead & 0xFE) == 0xFC) {
        ch &= 0x01;
        trailing = 5;
    } else if ((lead & 0xFC) == 0xF8) {
        ch &= 0x03;
        trailing = 4;
    } else if ((lead & 0xF8) == 0xF0) {
        ch &= 0x07;
        trailing = 3;
    } else if ((lead & 0xF0) == 0xE0) {
        ch &= 0x0F;
        trailing = 2;
    } else if ((lead & 0xE0) == 0xC0) {
        ch &= 0x1F;
        trailing = 1;
    } else {
        return (lead & 0x80) ? 0x80000000 : ch;
    }

    for (int32_t i = 0; i < trailing; i++) {
        uint8_t next = *string++;
        if ((next & 0xC0) != 0x80) {
            return 0x80000000;
        }
        if (bytes) {
            (*bytes)++;
        }
        ch = (next & 0x3F) | (ch << 6);
    }
    return ch;
}

// ref: FUN_14024da50
int32_t UnicodeCountUtf8ToUtf16(const char* source, uint32_t sourceBytes, int32_t* sourceUsed) {
    if (sourceBytes == 0 || !source) {
        if (sourceUsed) {
            *sourceUsed = 0;
        }
        return -1;
    }
    auto start = reinterpret_cast<const uint8_t*>(source);
    const uint8_t* end = sourceBytes == 0x7FFFFFFF
        ? reinterpret_cast<const uint8_t*>(~uintptr_t(0))
        : start + sourceBytes;
    const uint8_t* in = start;
    int32_t count = 0;
    for (;;) {
        if (in >= end) {
            count = -1;
            break;
        }
        uint8_t lead = *in;
        uint8_t trailing = s_trailingBytes[lead];
        if (in + trailing >= end) {
            count = -1 - trailing;
            break;
        }
        int32_t value = 0;
        for (int32_t i = trailing; i > 0; i--) {
            value = (value + static_cast<int8_t>(*in++)) << 6;
        }
        value += static_cast<int8_t>(*in++);
        uint32_t code = static_cast<uint32_t>(value) - s_offsets[trailing];
        if (code < 0x10000) {
            count++;
            if (value == static_cast<int32_t>(s_offsets[trailing])) {
                break;
            }
        } else if (code < 0x110000) {
            count += 2;
        } else {
            count++;
        }
    }
    if (sourceUsed) {
        *sourceUsed = static_cast<int32_t>(in - start);
    }
    return count;
}

// ref: FUN_14024dcf0
uint32_t SUniSGetUTF8Strict(const uint8_t* string, int32_t* bytes) {
    int32_t local = 0;
    int32_t* count = bytes != nullptr ? bytes : &local;
    *count = 0;
    if (string == nullptr) {
        return 0xffffffff;
    }
    uint8_t first = *string++;
    if (first == 0) {
        return 0xffffffff;
    }
    (*count)++;
    uint32_t code = first;
    int32_t extra = 0;
    if ((first & 0xfe) == 0xfc) {
        code = first & 1;
        extra = 5;
    } else if ((first & 0xfc) == 0xf8) {
        code = first & 3;
        extra = 4;
    } else if ((first & 0xf8) == 0xf0) {
        code = first & 7;
        extra = 3;
    } else if ((first & 0xf0) == 0xe0) {
        code = first & 0xf;
        extra = 2;
    } else if ((first & 0xe0) == 0xc0) {
        code = first & 0x1f;
        extra = 1;
    } else if (first & 0x80) {
        return 0x80000000;
    }
    for (int32_t i = 0; i < extra; i++) {
        uint8_t next = *string++;
        if ((next & 0xc0) != 0x80) {
            return 0x80000000;
        }
        (*count)++;
        code = (next & 0x3f) | (code << 6);
    }
    if (code == 0x80000000 || code == 0xffffffff) {
        return code;
    }
    if (code < 0x110000 && *count < 5 && 0x7ff < code - 0xd800) {
        int32_t needed = 1;
        if (0x7f < code) {
            if (code < 0x800) {
                needed = 2;
            } else {
                if (0xffff < code) {
                    return code;
                }
                needed = 3;
            }
        }
        if (*count <= needed) {
            return code;
        }
    }
    return 0x80000000;
}
