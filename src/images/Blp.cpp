#include "images/Images.hpp"

#include <cstring>

// The BLP2 reader (0x1424e0a80..0x1424e24a0): the header parsed in place, and the levels decoded
// into a mip chain: palettized levels through the palette (to ARGB8888, or dithered down to the
// 16-bit formats), DXT levels copied or converted, raw levels copied.

const uint8_t* PixelStaticBlock();

namespace {

// The 1-bit and 4-bit alpha expansions, and the ARGB2565 alpha plane's masks and shifts, inside
// the reference's static block (0x142d5e6b8, 0x142d5e6c8, 0x142d5e6d0, 0x142d5e6d8, 0x142d5e6e8).
const uint8_t* Alpha4() {
    return PixelStaticBlock() + 0x38;
}

const uint8_t* Alpha1() {
    return PixelStaticBlock() + 0x48;
}

// ARGB4444's 1-bit alpha words, indexed (as the reference does) by the masked bit's value rather
// than by 0 or 1, so the higher bits read the neighbouring tables (0x142d5e6cc).
uint16_t Alpha1Word(uint32_t index) {
    uint16_t word;
    std::memcpy(&word, PixelStaticBlock() + 0x4c + index * 2, 2);
    return word;
}

const uint8_t* PlaneMasks() {
    return PixelStaticBlock() + 0x50;
}

uint32_t PlaneShift(uint32_t index) {
    uint32_t shift;
    std::memcpy(&shift, PixelStaticBlock() + 0x58 + index * 4, 4);
    return shift;
}

uint8_t NibbleShift(uint32_t index) {
    return PixelStaticBlock()[0x68 + index * 4];
}

const uint32_t* s_pixelFormats = reinterpret_cast<const uint32_t*>(PixelStaticBlock());  // DAT_142d5e680

// Two rows of error terms (1026 entries of three ints) per dithering converter.
struct DITHERROWS {
    uint8_t m_bytes[2 * 0x3018];
};
DITHERROWS s_ditherArgb1555;    // DAT_1447b5090
DITHERROWS s_ditherArgb2565;    // DAT_1447c10f0
DITHERROWS s_ditherArgb4444;    // DAT_1447af060
DITHERROWS s_ditherRgb565;      // DAT_1447bb0c0

int32_t& Term(uint8_t* row, uint32_t entry, uint32_t channel) {
    return *reinterpret_cast<int32_t*>(row + entry * 12 + channel * 4);
}

// Floyd-Steinberg-style error diffusion of palettized pixels down to 16 bits. Channels are 8.16
// fixed point; `bits` are each channel's output width. The pixel's error goes 7/16 right, and
// into the next row's terms: 5/16 down-left, 3/16 down, 1/16 down-right.
template <class Pack>
void Dither(DITHERROWS* rows, const BLPIMAGE* image, uint16_t* dest, const uint8_t* source, uint32_t width,
    uint32_t height, const uint32_t bits[3], Pack pack) {
    std::memset(rows->m_bytes, 0, static_cast<uint64_t>(width + 2) * 0xc);
    for (uint32_t y = 0; y < height; y++) {
        uint8_t* read = rows->m_bytes + static_cast<uint64_t>(y & 1) * 0x3018;
        uint8_t* write = rows->m_bytes + static_cast<uint64_t>((y - 1) & 1) * 0x3018;
        std::memset(write, 0, 0x18);
        for (uint32_t x = 0; x < width; x++) {
            auto color = reinterpret_cast<const uint8_t*>(&image->m_palette[*source]);
            int32_t value[3];
            uint32_t out[3];
            int32_t error[3];
            for (uint32_t c = 0; c < 3; c++) {
                value[c] = static_cast<int32_t>(static_cast<uint32_t>(color[2 - c]) * 0x10000) + (Term(read, x + 1, c) >> 4);
                uint32_t shift = 0x18 - bits[c];
                uint32_t half = 1u << (shift - 1);
                uint32_t rounded = static_cast<uint32_t>(value[c]) + half;
                uint32_t mask = ~((1u << shift) - 1);
                int32_t q = static_cast<int32_t>(rounded) >> shift;
                uint32_t max = (1u << bits[c]) - 1;
                out[c] = q < 0 ? 0 : (q > static_cast<int32_t>(max) ? max : static_cast<uint32_t>(q));
                error[c] = value[c] - static_cast<int32_t>(rounded & mask);
            }
            *dest++ = pack(out);
            source++;
            for (uint32_t c = 0; c < 3; c++) {
                Term(read, x + 2, c) += error[c] * 7;
                Term(write, x + 2, c) = error[c];
                Term(write, x, c) += error[c] * 5;
                Term(write, x + 1, c) += error[c] * 3;
            }
        }
    }
}

// Palettized to ARGB1555 (dithered), then the alpha bit from the level's alpha data.
// ref: FUN_1424e0e20
void PaletteToArgb1555(const BLPIMAGE* image, uint16_t* dest, const uint8_t* source, uint32_t width, uint32_t height) {
    static const uint32_t bits[3] = { 5, 5, 5 };
    Dither(&s_ditherArgb1555, image, dest, source, width, height, bits,
        [](const uint32_t* c) { return static_cast<uint16_t>(((c[0] << 5 | c[1]) << 5) | c[2]); });
    source += static_cast<uint64_t>(width) * height;
    uint32_t count = width * height;
    uint32_t depth = image->m_alphaDepth;
    if (depth == 1) {
        uint32_t bit = 0;
        for (uint32_t i = 0; i < count; i++) {
            *dest |= static_cast<uint16_t>(((1u << (bit & 0x1f)) & static_cast<uint16_t>(*source)) << ((0xf - bit) & 0x1f));
            if (bit + 1 < 8) {
                bit++;
            } else {
                bit = 0;
                source++;
            }
            dest++;
        }
    } else if (depth == 4) {
        uint32_t nibble = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (nibble == 0) {
                *dest = static_cast<uint16_t>(((*source & 0xfff8) << 0xc) | *dest);
                nibble = image->m_alphaDepth;
            } else {
                *dest = static_cast<uint16_t>(((*source & 0xff80) << 8) | *dest);
                source++;
                nibble = 0;
            }
            dest++;
        }
    } else if (depth == 8 || depth == 0x48) {
        for (uint32_t i = 0; i < count; i++) {
            *dest = static_cast<uint16_t>(*dest | ((*source & 0xff80) << 8));
            source++;
            dest++;
        }
    }
}

// Palettized to RGB565 (dithered) and a 2-bit alpha plane after it.
// ref: FUN_1424e1340
void PaletteToArgb2565(const BLPIMAGE* image, uint16_t* dest, const uint8_t* source, uint32_t width, uint32_t height) {
    static const uint32_t bits[3] = { 5, 6, 5 };
    Dither(&s_ditherArgb2565, image, dest, source, width, height, bits,
        [](const uint32_t* c) { return static_cast<uint16_t>(((c[0] << 6 | c[1]) << 5) | c[2]); });
    auto plane = reinterpret_cast<uint8_t*>(dest + static_cast<uint64_t>(width) * height);
    source += static_cast<uint64_t>(width) * height;
    uint32_t count = width * height;
    uint32_t depth = image->m_alphaDepth;
    // The alpha bits are not masked to two before they are shifted in, as in the reference.
    if (depth == 1) {
        for (uint64_t i = 0; i < count; i++) {
            uint32_t slot = static_cast<uint32_t>(i & 3);
            plane[i >> 2] = static_cast<uint8_t>(((static_cast<uint8_t>(source[i >> 3] >> (i & 7)) * 2) << (PlaneShift(slot) & 0x1f))
                | (~PlaneMasks()[slot] & plane[i >> 2]));
        }
    } else if (depth == 4) {
        for (uint64_t i = 0; i < count; i++) {
            uint32_t slot = static_cast<uint32_t>(i & 3);
            plane[i >> 2] = static_cast<uint8_t>(((source[i >> 1] >> (NibbleShift(static_cast<uint32_t>(i & 1)) & 0x1f)) << (PlaneShift(slot) & 0x1f))
                | (~PlaneMasks()[slot] & plane[i >> 2]));
        }
    } else if ((depth == 8 || depth == 0x48) && count != 0) {
        for (uint64_t i = 0; i < count; i++) {
            uint32_t slot = static_cast<uint32_t>(i & 3);
            plane[i >> 2] = static_cast<uint8_t>(((source[i] >> 6) << (PlaneShift(slot) & 0x1f)) | (~PlaneMasks()[slot] & plane[i >> 2]));
        }
    }
}

// Palettized to ARGB4444 (dithered), then the alpha nibble.
// ref: FUN_1424e17c0
void PaletteToArgb4444(const BLPIMAGE* image, uint16_t* dest, const uint8_t* source, uint32_t width, uint32_t height) {
    static const uint32_t bits[3] = { 4, 4, 4 };
    Dither(&s_ditherArgb4444, image, dest, source, width, height, bits,
        [](const uint32_t* c) { return static_cast<uint16_t>(((c[0] << 4 | c[1]) << 4) | c[2]); });
    source += static_cast<uint64_t>(width) * height;
    uint32_t count = width * height;
    uint32_t depth = image->m_alphaDepth;
    if (depth == 1) {
        uint32_t mask = 1;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t next = mask * 2;
            *dest |= Alpha1Word(*source & mask);
            if (next >= 0x100) {
                source++;
                mask = 1;
            } else {
                mask = next;
            }
            dest++;
        }
    } else if (depth == 4) {
        for (uint32_t i = 0; i < count; i++) {
            uint16_t bits4;
            if ((i & 1) == 0) {
                bits4 = static_cast<uint16_t>(*source << 0xc);
            } else {
                bits4 = static_cast<uint16_t>((*source & 0xfff0) << 8);
                source++;
            }
            *dest |= bits4;
            dest++;
        }
    } else if (depth == 8 || depth == 0x48) {
        for (uint32_t i = 0; i < count; i++) {
            *dest = static_cast<uint16_t>(*dest | ((*source & 0xfff0) << 8));
            source++;
            dest++;
        }
    }
}

// Palettized to RGB565 (dithered).
// ref: FUN_1424e1cc0
void PaletteToRgb565(const BLPIMAGE* image, uint16_t* dest, const uint8_t* source, uint32_t width, uint32_t height) {
    static const uint32_t bits[3] = { 5, 6, 5 };
    Dither(&s_ditherRgb565, image, dest, source, width, height, bits,
        [](const uint32_t* c) { return static_cast<uint16_t>(((c[0] << 6 | c[1]) << 5) | c[2]); });
}

// Palettized to ARGB8888, opaque, then the alpha from the level's alpha data.
// ref: FUN_1424e1fb0
void PaletteToArgb8888(const BLPIMAGE* image, uint32_t* dest, const uint8_t* source, uint32_t count) {
    uint32_t* out = dest;
    for (uint32_t i = 0; i < count; i++) {
        *out = image->m_palette[*source++];
        reinterpret_cast<uint8_t*>(out)[3] = 0xff;
        out++;
    }
    uint32_t depth = image->m_alphaDepth;
    if (depth == 1) {
        uint32_t groups = count >> 3;
        const uint8_t* alpha = source;
        for (uint32_t g = 0; g < groups; g++) {
            uint8_t bits = *alpha++;
            auto bytes = reinterpret_cast<uint8_t*>(dest + g * 8);
            for (uint32_t k = 0; k < 8; k++) {
                bytes[k * 4 + 3] = Alpha1()[(bits >> k) & 1];
            }
        }
        dest += static_cast<uint64_t>(groups) * 8;
        uint32_t rest = count & 7;
        if (rest != 0) {
            uint32_t bits = source[count >> 3];
            auto bytes = reinterpret_cast<uint8_t*>(dest);
            for (uint32_t k = 0; k < rest; k++) {
                bytes[k * 4 + 3] = Alpha1()[bits & 1];
                bits >>= 1;
            }
        }
    } else if (depth == 4) {
        auto bytes = reinterpret_cast<uint8_t*>(dest);
        for (uint32_t pair = count >> 1; pair != 0; pair--) {
            bytes[3] = Alpha4()[*source & 0xf];
            uint8_t value = *source++;
            bytes[7] = Alpha4()[value >> 4];
            bytes += 8;
        }
        if ((count & 1) != 0) {
            bytes[3] = Alpha4()[*source & 0xf];
        }
    } else if ((depth == 8 || depth == 0x48) && count != 0) {
        auto bytes = reinterpret_cast<uint8_t*>(dest);
        for (uint32_t i = 0; i < count; i++) {
            bytes[3] = *source++;
            bytes += 4;
        }
    }
}

}

// ref: FUN_1424e24a0
bool BlpImageParse(BLPIMAGE* image, const uint8_t* data, uint32_t size, bool skipLowRes) {
    image->m_flags = 0;
    uint32_t width = *reinterpret_cast<const uint32_t*>(data + 0xc);
    image->m_width = width;
    uint32_t height = *reinterpret_cast<const uint32_t*>(data + 0x10);
    image->m_height = height;
    if (skipLowRes && (data[0xb] & 0x20) != 0) {
        // A second, full-resolution BLP follows the levels of a low-resolution one.
        image->m_flags = 2;
        auto sizes = reinterpret_cast<const int32_t*>(data + 0x54);
        int32_t last = 0;
        for (int32_t i = 0; sizes[i] != 0; i++) {
            last = i;
        }
        uint32_t end = static_cast<uint32_t>(sizes[last]) + *reinterpret_cast<const uint32_t*>(data + 0x14 + last * 4);
        data += end;
        size -= end;
        width = *reinterpret_cast<const uint32_t*>(data + 0xc);
        image->m_width = width;
        height = *reinterpret_cast<const uint32_t*>(data + 0x10);
        image->m_height = height;
    }
    uint32_t type = (data[0xb] >> 2) & 3;
    image->m_type = type;
    if (type == 0) {
        image->m_type = (width == height * 6) + 1;
    }
    uint32_t levels = ImageMipLevels(width, height, image->m_type == 2);
    uint32_t count = 0;
    auto offsets = reinterpret_cast<const uint32_t*>(data + 0x14);
    for (uint32_t i = 0; i < 0x10; i++) {
        uint32_t mipSize = offsets[0x10 + i];
        uint8_t* mip;
        if (mipSize == 0 || levels <= i) {
            mipSize = 0;
            mip = nullptr;
        } else {
            if (size < offsets[i] + mipSize) {
                return false;
            }
            count++;
            mip = const_cast<uint8_t*>(data) + offsets[i];
        }
        image->m_mips[i] = mip;
        image->m_mipSizes[i] = mipSize;
    }
    image->m_mipCount = count;
    image->m_compression = data[8];
    image->m_format = data[10];
    if (data[10] == 8) {
        image->m_format = 2;
    }
    image->m_alphaDepth = data[9];
    if ((data[0xb] & 0x10) != 0) {
        image->m_flags |= 1;
    }
    if (data[8] == 1) {
        image->m_palette = reinterpret_cast<const uint32_t*>(data + 0x94);
    }
    return true;
}

// ref: FUN_1424e2200
void BlpImageInit(BLPIMAGE* image, MIPBITS* mips, uint32_t width, uint32_t height, uint32_t mipCount,
    uint32_t compression, uint32_t format, uint32_t alphaDepth, uint32_t type) {
    for (uint32_t i = 0; i < 0x10; i++) {
        image->m_mips[i] = i < mipCount ? mips[i] : nullptr;
        image->m_mipSizes[i] = 0;
    }
    image->m_compression = compression;
    image->m_format = format;
    image->m_alphaDepth = alphaDepth;
    image->m_width = width;
    image->m_height = height;
    image->m_mipCount = mipCount;
    image->m_type = type;
    image->m_flags = 0;
    image->m_palette = nullptr;
    if (type == 0) {
        image->m_type = (width == height * 6) + 1;
    }
}

// ref: FUN_1424e0aa0
bool BlpImageDecode(BLPIMAGE* image, uint32_t format, MIPBITS* bits, uint32_t firstMip) {
    if (firstMip >= image->m_mipCount) {
        return false;
    }
    uint32_t width = image->m_width >> (firstMip & 0x1f);
    uint32_t height = image->m_height >> (firstMip & 0x1f);
    MipBitsInit(format, width > 1 ? width : 1, height > 1 ? height : 1, image->m_type == 2, bits);
    for (uint32_t mip = firstMip; mip < image->m_mipCount; mip++) {
        uint8_t* out = bits[mip - firstMip];
        const uint8_t* in = image->m_mips[mip];
        bool ok = false;
        uint32_t compression = image->m_compression;
        if (compression == 0) {
            return false;
        }
        if (compression == 1) {
            uint32_t mipWidth = image->m_width >> (mip & 0x1f);
            uint32_t mipHeight = image->m_height >> (mip & 0x1f);
            if (format != 2 && format != 3 && format != 4 && format != 5 && format != 9) {
                return false;
            }
            uint32_t w = mipWidth > 1 ? mipWidth : 1;
            uint32_t h = mipHeight > 1 ? mipHeight : 1;
            if (format == 2) {
                uint32_t count = h * w;
                if ((image->m_alphaDepth & 0x3f) == 8) {
                    auto pixel = reinterpret_cast<uint32_t*>(out);
                    for (uint32_t i = 0; i < count; i++) {
                        *pixel = image->m_palette[in[i]];
                        reinterpret_cast<uint8_t*>(pixel)[3] = in[i + count];
                        pixel++;
                    }
                } else {
                    PaletteToArgb8888(image, reinterpret_cast<uint32_t*>(out), in, count);
                }
            } else if (format == 3) {
                PaletteToArgb1555(image, reinterpret_cast<uint16_t*>(out), in, w, h);
            } else if (format == 4) {
                PaletteToArgb4444(image, reinterpret_cast<uint16_t*>(out), in, w, h);
            } else if (format == 5) {
                PaletteToRgb565(image, reinterpret_cast<uint16_t*>(out), in, w, h);
            } else {
                PaletteToArgb2565(image, reinterpret_cast<uint16_t*>(out), in, w, h);
            }
            ok = true;
        } else if (compression == 2) {
            switch (format) {
            case 0:
            case 1:
            case 7:
            case 10:
            case 11:
                std::memcpy(out, in, image->m_mipSizes[mip]);
                ok = true;
                break;
            case 2:
            case 3:
            case 4:
            case 5:
            case 0xc: {
                uint32_t mipWidth = image->m_width >> (mip & 0x1f);
                uint32_t w = mipWidth > 1 ? mipWidth : 1;
                uint32_t mipHeight = image->m_height >> (mip & 0x1f);
                uint32_t h = mipHeight > 1 ? mipHeight : 1;
                int32_t sourceFormat = static_cast<int32_t>(s_pixelFormats[image->m_format]);
                int32_t destFormat = static_cast<int32_t>(s_pixelFormats[format]);
                uint32_t destPitch = PixelRowPitch(destFormat, w, h);
                uint32_t sourcePitch = PixelRowPitch(sourceFormat, w, h);
                int32_t rect[2] = { static_cast<int32_t>(w), static_cast<int32_t>(h) };
                ok = PixelConvert(rect, 0, in, sourcePitch, sourceFormat, out, destPitch, destFormat);
                break;
            }
            default:
                break;
            }
        } else if (compression == 3 || compression == 4) {
            std::memcpy(out, in, image->m_mipSizes[mip]);
            ok = true;
        }
        if (!ok) {
            return false;
        }
    }
    return true;
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_1447af060 = (void*)&s_ditherArgb4444;
extern "C" void* const gen_p_1447b5090 = (void*)&s_ditherArgb1555;
extern "C" void* const gen_p_1447bb0c0 = (void*)&s_ditherRgb565;
extern "C" void* const gen_p_1447c10f0 = (void*)&s_ditherArgb2565;
// ---- end of generated globals ----
