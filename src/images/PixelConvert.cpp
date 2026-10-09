#include "images/Images.hpp"

#include <cstring>

// The pixel format converters (0x1424e2640..0x1424e7680): a table of (mode, source format,
// destination format) -> converter, filled the first time it is asked, and the DXT / BC4 / BC5
// decoders behind it. Every block decoder has a fast path for whole blocks and a clamped path for
// edges and cube maps (six faces side by side), as the reference does.

namespace {

// The reference's static tables 0x142d5e680..0x142d5e800, kept as one block: some readers index
// past the table they mean into the next one, and must see the same bytes.
const uint8_t s_staticBlock[0x180] = {
    0x07, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00,
    0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0,
    0x03, 0x0c, 0x30, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x10, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x02, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0xff, 0xff, 0xff, 0xff,
};

const uint32_t* const s_blockShift = reinterpret_cast<const uint32_t*>(s_staticBlock + 0x70);  // DAT_142d5e6f0
const uint32_t* const s_unitBytes = reinterpret_cast<const uint32_t*>(s_staticBlock + 0xe0);   // DAT_142d5e760

// DAT_142ccab98: >= 2 when the row-pointer setup may use SSE4.1 (it computes the same values).
const int32_t s_simdLevel = 1;

// --- palettes ------------------------------------------------------------------------------

uint32_t Expand5(uint32_t v) {
    return (v << 3) | (v >> 2);
}

uint32_t Expand6(uint32_t v) {
    return (v << 2) | (v >> 4);
}

// The two endpoints of a DXT block expanded to 8 bits a channel.
struct ENDPOINTS {
    uint32_t r0, g0, b0, r1, g1, b1;
    ENDPOINTS(uint16_t c0, uint16_t c1) {
        r0 = Expand5(c0 >> 11);
        g0 = Expand6((c0 >> 5) & 0x3f);
        b0 = Expand5(c0 & 0x1f);
        r1 = Expand5(c1 >> 11);
        g1 = Expand6((c1 >> 5) & 0x3f);
        b1 = Expand5(c1 & 0x1f);
    }
};

uint32_t Argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b) {
    return (a << 24) | ((r & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff);
}

// The four colours of a DXT3/DXT5 block (always the four-colour mode) as ARGB4444.
// ref: FUN_1424e58c0
void ColorsArgb4444(const uint16_t* block, uint16_t colors[4]) {
    ENDPOINTS e(block[0], block[1]);
    uint32_t b0 = (block[0] & 0x1f) << 3;
    uint32_t b1 = (block[1] & 0x1f) << 3;
    colors[0] = static_cast<uint16_t>(((e.r0 & 0xfff0) << 4) | (e.g0 & 0xfff0) | ((b0 & 0xff) >> 4) | 0xf000);
    colors[1] = static_cast<uint16_t>(((e.r1 & 0xfff0) << 4) | (e.g1 & 0xfff0) | ((b1 & 0xff) >> 4) | 0xf000);
    colors[2] = static_cast<uint16_t>((((e.r0 * 2 + 1 + e.r1) / 3 & 0xf0) << 4) | ((e.g0 * 2 + 1 + e.g1) / 3 & 0xf0)
        | (((e.b0 * 2 + 1 + e.b1) / 3 >> 4) & 0xf) | 0xf000);
    colors[3] = static_cast<uint16_t>((((e.r0 + 1 + e.r1 * 2) / 3 & 0xf0) << 4) | ((e.g0 + 1 + e.g1 * 2) / 3 & 0xf0)
        | (((e.b1 * 2 + 1 + e.b0) / 3 >> 4) & 0xf) | 0xf000);
}

// The four colours of a DXT1 block as ARGB1555; c0 <= c1 is the three-colour mode, whose fourth
// colour is transparent black.
// ref: FUN_1424e5ae0
void ColorsArgb1555(const uint16_t* block, uint16_t colors[4]) {
    ENDPOINTS e(block[0], block[1]);
    uint32_t r2, g2, b2, r3, g3, b3, a3;
    if (block[1] < block[0]) {
        r2 = (e.r0 * 2 + 1 + e.r1) / 3;
        g2 = (e.g0 * 2 + 1 + e.g1) / 3;
        b2 = (e.b0 * 2 + 1 + e.b1) / 3;
        r3 = (e.r1 * 2 + 1 + e.r0) / 3;
        g3 = (e.g1 * 2 + 1 + e.g0) / 3;
        b3 = (e.b1 * 2 + 1 + e.b0) / 3;
        a3 = 0x2000;
    } else {
        r2 = (e.r0 + e.r1) >> 1;
        g2 = (e.g0 + e.g1) >> 1;
        b2 = (e.b0 + e.b1) >> 1;
        r3 = 0;
        g3 = 0;
        b3 = 0;
        a3 = 0;
    }
    colors[0] = static_cast<uint16_t>((((e.r0 & 0xf8) << 5 | (e.g0 & 0xf8) | 0xe000) << 2) | ((e.b0 & 0xff) >> 3));
    colors[1] = static_cast<uint16_t>((((e.r1 & 0xf8) << 5 | (e.g1 & 0xf8) | 0xe000) << 2) | ((e.b1 & 0xff) >> 3));
    colors[2] = static_cast<uint16_t>((((r2 & 0xf8) << 5 | (g2 & 0xf8) | 0xe000) << 2) | ((b2 & 0xff) >> 3));
    colors[3] = static_cast<uint16_t>((((r3 & 0xf8) << 5 | (g3 & 0xf8) | a3) << 2) | ((b3 & 0xff) >> 3));
}

// The four colours of a DXT1 block as RGB565 (the fourth black in the three-colour mode).
// ref: FUN_1424e5d70
void ColorsRgb565(const uint16_t* block, uint16_t colors[4]) {
    ENDPOINTS e(block[0], block[1]);
    uint32_t r2, g2, b2, r3, g3, b3;
    if (block[1] < block[0]) {
        r2 = (e.r0 * 2 + 1 + e.r1) / 3;
        g2 = (e.g0 * 2 + 1 + e.g1) / 3;
        b2 = (e.b1 + 1 + e.b0 * 2) / 3;
        r3 = (e.r1 * 2 + 1 + e.r0) / 3;
        g3 = (e.g1 * 2 + 1 + e.g0) / 3;
        b3 = (e.b1 * 2 + 1 + e.b0) / 3;
    } else {
        r2 = (e.r0 + e.r1) >> 1;
        g2 = (e.g0 + e.g1) >> 1;
        b2 = (e.b0 + e.b1) >> 1;
        r3 = 0;
        g3 = 0;
        b3 = 0;
    }
    colors[0] = static_cast<uint16_t>((((e.r0 & 0xf8) << 5 | (e.g0 & 0xfc)) << 3) | ((e.b0 & 0xff) >> 3));
    colors[1] = static_cast<uint16_t>((((e.r1 & 0xf8) << 5 | (e.g1 & 0xfc)) << 3) | ((e.b1 & 0xff) >> 3));
    colors[2] = static_cast<uint16_t>((((r2 & 0xf8) << 5 | (g2 & 0xfc)) << 3) | ((b2 & 0xff) >> 3));
    colors[3] = static_cast<uint16_t>((((r3 & 0xf8) << 5 | (g3 & 0xfc)) << 3) | ((b3 & 0xff) >> 3));
}

// Lerps a destination ARGB8888 pixel's colour toward a source's by `alpha` (/256), keeping the
// destination's alpha; 0xff copies the colour.
// ref: FUN_1424e5fe0
void BlendPixel(uint32_t* dest, int32_t alpha, const uint32_t* source) {
    if (alpha == 0xff) {
        *dest ^= (*dest ^ *source) & 0xffffff;
        return;
    }
    auto d = reinterpret_cast<uint8_t*>(dest);
    auto s = reinterpret_cast<const uint8_t*>(source);
    uint8_t b2 = static_cast<uint8_t>(((static_cast<int32_t>(s[2]) - d[2]) * alpha >> 8) + d[2]);
    uint8_t b1 = static_cast<uint8_t>(((static_cast<int32_t>(s[1]) - d[1]) * alpha >> 8) + d[1]);
    uint8_t b0 = static_cast<uint8_t>(((static_cast<int32_t>(s[0]) - d[0]) * alpha >> 8) + d[0]);
    *dest = (static_cast<uint32_t>(b2) << 16) | (static_cast<uint32_t>(b1) << 8) | b0 | (*dest & 0xff000000);
}

// The 3-bit index of pixel `i` of a BC4-style block (indices from byte 2 on).
uint32_t RampIndex(const uint8_t* block, uint32_t bit) {
    uint32_t byte = bit >> 3;
    uint32_t shift = bit & 7;
    return ((static_cast<uint32_t>(block[byte + 1 + 2]) << (8 - shift)) | (block[byte + 2] >> shift)) & 7;
}

// --- block walks ---------------------------------------------------------------------------

// Whole 4x4 blocks: `decode` writes a block's 16 pixels, row by row, into the four row pointers.
template <uint32_t BlockBytes, uint32_t PixelBytes, class Decode>
void BlocksFast(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch,
    Decode decode) {
    uint32_t width = static_cast<uint32_t>(rect[0]);
    for (int32_t y = 0; y < rect[1]; y += 4) {
        uint8_t* rows[4];
        rows[0] = dest;
        rows[1] = rows[0] + destPitch;
        rows[2] = rows[1] + destPitch;
        rows[3] = rows[2] + destPitch;
        uint8_t* next = rows[3] + destPitch;
        const uint8_t* block = source;
        for (int32_t x = 0; x < static_cast<int32_t>(width); x += 4) {
            uint8_t pixels[16 * PixelBytes];
            decode(block, pixels);
            for (int32_t r = 0; r < 4; r++) {
                std::memcpy(rows[r], pixels + r * 4 * PixelBytes, 4 * PixelBytes);
                rows[r] += 4 * PixelBytes;
            }
            block += BlockBytes;
        }
        source += sourcePitch;
        dest = next;
    }
}

// Edges and cube maps: blocks clipped to the image, per face. As in the reference, the
// destination rows restart at the face's first row for every row of blocks, and a face's source
// starts `BlockBytes * 2` per face pixel in.
template <uint32_t BlockBytes, uint32_t PixelBytes, class Decode>
void BlocksClamped(const int32_t* rect, const uint8_t* source, int32_t sourcePitch, uint8_t* dest, int32_t destPitch,
    Decode decode) {
    int32_t height = rect[1];
    int32_t width = rect[0];
    int32_t faceWidth = width;
    if (width == height * 6) {
        faceWidth = width / 6;
    }
    int64_t sourceFace = static_cast<int64_t>(faceWidth) * (BlockBytes == 8 ? 8 : 16);
    int64_t destFace = static_cast<int64_t>(faceWidth) * PixelBytes;
    int64_t faces = width == height * 6 ? 6 : 1;
    do {
        uint32_t rowsLeft = static_cast<uint32_t>(height - 1);
        for (int32_t y = 0; y < height; y += 4) {
            uint8_t* rows[4];
            for (int32_t r = 0; r < 4; r++) {
                rows[r] = dest + static_cast<uint32_t>(r * destPitch);
            }
            const uint8_t* block = source + static_cast<uint32_t>((y >> 2) * sourcePitch);
            for (int32_t x = 0; x < faceWidth; x += 4) {
                uint32_t lastRow = static_cast<int32_t>(rowsLeft) < 3 ? rowsLeft : 3;
                uint32_t columnsLeft = static_cast<uint32_t>(faceWidth - x - 1);
                uint32_t lastColumn = static_cast<int32_t>(columnsLeft) < 3 ? columnsLeft : 3;
                uint8_t pixels[16 * PixelBytes];
                decode(block, pixels);
                for (uint32_t r = 0; r <= lastRow; r++) {
                    std::memcpy(rows[r], pixels + r * 4 * PixelBytes, (lastColumn + 1) * PixelBytes);
                    rows[r] += (lastColumn + 1) * PixelBytes;
                }
                block += BlockBytes;
            }
            rowsLeft -= 4;
        }
        source += sourceFace;
        dest += destFace;
    } while (--faces != 0);
}

bool WholeBlocks(const int32_t* rect) {
    return rect[0] >= 4 && rect[1] >= 4 && (rect[0] & 3) == 0 && (rect[1] & 3) == 0;
}

// --- block decoders ------------------------------------------------------------------------

struct DecodeDxt1Argb8888 {
    void operator()(const uint8_t* block, uint8_t* pixels) const {
        uint32_t colors[4] = {};
        DxtColorsDxt1(reinterpret_cast<const uint16_t*>(block), colors);
        auto out = reinterpret_cast<uint32_t*>(pixels);
        for (int r = 0; r < 4; r++) {
            uint32_t indices = block[4 + r];
            for (int c = 0; c < 4; c++) {
                *out++ = colors[indices & 3];
                indices >>= 2;
            }
        }
    }
};

template <void (*Colors)(const uint16_t*, uint16_t*)>
struct DecodeDxt1Sixteen {
    void operator()(const uint8_t* block, uint8_t* pixels) const {
        uint16_t colors[4];
        Colors(reinterpret_cast<const uint16_t*>(block), colors);
        auto out = reinterpret_cast<uint16_t*>(pixels);
        for (int r = 0; r < 4; r++) {
            uint32_t indices = block[4 + r];
            for (int c = 0; c < 4; c++) {
                *out++ = colors[indices & 3];
                indices >>= 2;
            }
        }
    }
};

struct DecodeDxt3Argb4444 {
    void operator()(const uint8_t* block, uint8_t* pixels) const {
        uint16_t colors[4];
        ColorsArgb4444(reinterpret_cast<const uint16_t*>(block + 8), colors);
        auto out = reinterpret_cast<uint16_t*>(pixels);
        for (int r = 0; r < 4; r++) {
            uint32_t indices = block[0xc + r];
            uint32_t alpha = reinterpret_cast<const uint16_t*>(block)[r];
            for (int c = 0; c < 4; c++) {
                *out++ = static_cast<uint16_t>((colors[indices & 3] & 0xfff) | (static_cast<uint16_t>(alpha) << 12));
                indices >>= 2;
                alpha >>= 4;
            }
        }
    }
};

struct DecodeDxt3Argb8888 {
    void operator()(const uint8_t* block, uint8_t* pixels) const {
        uint32_t colors[4] = {};
        DxtColorsDxt3(reinterpret_cast<const uint16_t*>(block + 8), colors);
        uint8_t* out = pixels;
        for (int r = 0; r < 4; r++) {
            uint32_t indices = block[0xc + r];
            uint32_t alpha = reinterpret_cast<const uint16_t*>(block)[r];
            for (int c = 0; c < 4; c++) {
                auto color = reinterpret_cast<const uint8_t*>(&colors[indices & 3]);
                out[3] = static_cast<uint8_t>(alpha << 4);
                out[2] = color[2];
                out[1] = color[1];
                out[0] = color[0];
                out += 4;
                indices >>= 2;
                alpha >>= 4;
            }
        }
    }
};

struct DecodeDxt5Argb4444 {
    void operator()(const uint8_t* block, uint8_t* pixels) const {
        uint16_t colors[4];
        ColorsArgb4444(reinterpret_cast<const uint16_t*>(block + 8), colors);
        uint8_t ramp[8] = { block[0], block[1] };
        DxtAlphaRamp(ramp);
        auto out = reinterpret_cast<uint16_t*>(pixels);
        uint32_t bit = 0;
        for (int r = 0; r < 4; r++) {
            uint32_t indices = block[0xc + r];
            for (int c = 0; c < 4; c++) {
                *out++ = static_cast<uint16_t>((static_cast<uint16_t>(ramp[RampIndex(block, bit)] >> 4) << 12)
                    | (colors[indices & 3] & 0xfff));
                bit += 3;
                indices >>= 2;
            }
        }
    }
};

struct DecodeDxt5Argb8888 {
    void operator()(const uint8_t* block, uint8_t* pixels) const {
        uint32_t colors[4] = {};
        DxtColorsDxt3(reinterpret_cast<const uint16_t*>(block + 8), colors);
        uint8_t ramp[8] = { block[0], block[1] };
        DxtAlphaRamp(ramp);
        uint8_t* out = pixels;
        uint32_t bit = 0;
        for (int r = 0; r < 4; r++) {
            uint32_t indices = block[0xc + r];
            for (int c = 0; c < 4; c++) {
                auto color = reinterpret_cast<const uint8_t*>(&colors[indices & 3]);
                out[3] = ramp[RampIndex(block, bit)];
                out[2] = color[2];
                out[1] = color[1];
                out[0] = color[0];
                out += 4;
                bit += 3;
                indices >>= 2;
            }
        }
    }
};

// BC4 into ARGB4444's alpha. The low twelve bits are whatever 16-bit value the reference finds
// in a stack slot it never initialized: the low bits of a pointer argument. Kept, so that the
// colour bits match.
struct DecodeBc4Argb4444 {
    uint16_t m_low;
    void operator()(const uint8_t* block, uint8_t* pixels) const {
        uint8_t ramp[8] = { block[0], block[1] };
        DxtAlphaRamp(ramp);
        auto out = reinterpret_cast<uint16_t*>(pixels);
        uint32_t bit = 0;
        for (int i = 0; i < 16; i++) {
            *out++ = static_cast<uint16_t>((static_cast<uint16_t>(ramp[RampIndex(block, bit)] >> 4) << 12) | (this->m_low & 0xfff));
            bit += 3;
        }
    }
};

struct DecodeBc4Argb8888 {
    void operator()(const uint8_t* block, uint8_t* pixels) const {
        uint8_t ramp[8] = { block[0], block[1] };
        DxtAlphaRamp(ramp);
        uint8_t* out = pixels;
        uint32_t bit = 0;
        for (int i = 0; i < 16; i++) {
            out[0] = 0;
            out[1] = 0;
            out[2] = 0;
            out[3] = ramp[RampIndex(block, bit)];
            out += 4;
            bit += 3;
        }
    }
};

struct DecodeBc5Argb4444 {
    void operator()(const uint8_t* block, uint8_t* pixels) const {
        uint8_t red[8] = { block[0], block[1] };
        DxtAlphaRamp(red);
        uint8_t green[8] = { block[8], block[9] };
        DxtAlphaRamp(green);
        auto out = reinterpret_cast<uint16_t*>(pixels);
        uint32_t bit = 0;
        for (int i = 0; i < 16; i++) {
            *out++ = static_cast<uint16_t>(((red[RampIndex(block, bit)] & 0xfff0) << 4)
                | (green[RampIndex(block + 8, bit)] & 0xfff0) | 0xf000);
            bit += 3;
        }
    }
};

struct DecodeBc5Argb8888 {
    void operator()(const uint8_t* block, uint8_t* pixels) const {
        uint8_t red[8] = { block[0], block[1] };
        DxtAlphaRamp(red);
        uint8_t green[8] = { block[8], block[9] };
        DxtAlphaRamp(green);
        auto out = reinterpret_cast<uint32_t*>(pixels);
        uint32_t bit = 0;
        for (int i = 0; i < 16; i++) {
            *out++ = 0xff000000u | (static_cast<uint32_t>(red[RampIndex(block, bit)]) << 16)
                | (static_cast<uint32_t>(green[RampIndex(block + 8, bit)]) << 8);
            bit += 3;
        }
    }
};

// --- converters ----------------------------------------------------------------------------

// Row copies at 1, 2, 4, 8 and 16 bytes a pixel.
template <uint32_t PixelBytes>
void CopyRows(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    uint64_t row = static_cast<uint64_t>(static_cast<int64_t>(rect[0])) * PixelBytes;
    if (row == sourcePitch && row == destPitch) {
        std::memcpy(dest, source, static_cast<int64_t>(rect[0] * rect[1]) * PixelBytes);
        return;
    }
    for (int32_t y = rect[1]; y != 0; y--) {
        std::memcpy(dest, source, row);
        source += sourcePitch;
        dest += destPitch;
    }
}

// ref: FUN_1424e7410
void Copy8(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    CopyRows<1>(rect, source, sourcePitch, dest, destPitch);
}

// ref: FUN_1424e7540
void Copy16(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    CopyRows<2>(rect, source, sourcePitch, dest, destPitch);
}

// ref: FUN_1424e6720
void Copy32(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    CopyRows<4>(rect, source, sourcePitch, dest, destPitch);
}

// ref: FUN_1424e75e0
void Copy64(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    CopyRows<8>(rect, source, sourcePitch, dest, destPitch);
}

// ref: FUN_1424e74a0
void Copy128(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    CopyRows<16>(rect, source, sourcePitch, dest, destPitch);
}

// Block rows of a block format: `UnitBytes` per pixel column of a 4-row block.
template <uint32_t UnitBytes>
void CopyBlockRows(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    int32_t width = rect[0] < 4 ? 4 : rect[0];
    int32_t rows = rect[1] >> 2 < 1 ? 1 : rect[1] >> 2;
    uint64_t row = static_cast<uint64_t>(static_cast<uint32_t>(width)) * UnitBytes;
    if (row == sourcePitch && row == destPitch) {
        std::memcpy(dest, source, static_cast<int64_t>(rows * width) * UnitBytes);
        return;
    }
    for (; rows != 0; rows--) {
        std::memcpy(dest, source, row);
        source += sourcePitch;
        dest += destPitch;
    }
}

// ref: FUN_1424e6c40
void CopyDxt1(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    CopyBlockRows<2>(rect, source, sourcePitch, dest, destPitch);
}

// ref: FUN_1424e6e60
void CopyDxt35(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    CopyBlockRows<4>(rect, source, sourcePitch, dest, destPitch);
}

// ref: FUN_1424e5770
void CopyBc4(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    int32_t width = rect[0] < 4 ? 4 : rect[0];
    int32_t rows = rect[1] >> 2 < 1 ? 1 : rect[1] >> 2;
    uint32_t row = static_cast<uint32_t>(width * 2);
    if (row == sourcePitch && row == destPitch) {
        std::memcpy(dest, source, static_cast<uint32_t>(rows * width * 2));
        return;
    }
    for (; rows != 0; rows--) {
        std::memcpy(dest, source, row);
        source += sourcePitch;
        dest += destPitch;
    }
}

// ref: FUN_1424e5810
void CopyBc5(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    int32_t width = rect[0] < 4 ? 4 : rect[0];
    int32_t rows = rect[1] >> 2 < 1 ? 1 : rect[1] >> 2;
    uint32_t row = static_cast<uint32_t>(width * 4);
    if (row == sourcePitch && row == destPitch) {
        std::memcpy(dest, source, static_cast<uint32_t>(rows * width * 4));
        return;
    }
    for (; rows != 0; rows--) {
        std::memcpy(dest, source, row);
        source += sourcePitch;
        dest += destPitch;
    }
}

// BC5 with its two channels' halves swapped in each block.
// ref: FUN_1424e2640
void SwapBc5(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    int32_t width = rect[0] < 4 ? 4 : rect[0];
    int32_t rows = rect[1] >> 2 < 1 ? 1 : rect[1] >> 2;
    do {
        rows--;
        for (uint32_t offset = 0; offset < static_cast<uint32_t>(width * 4); offset += 0x10) {
            uint64_t low;
            uint64_t high;
            std::memcpy(&high, source + offset + 8, 8);
            std::memcpy(&low, source + offset, 8);
            std::memcpy(dest + offset, &high, 8);
            std::memcpy(dest + offset + 8, &low, 8);
        }
        source += sourcePitch;
        dest += destPitch;
    } while (rows != 0);
}

// ARGB8888 <-> ABGR8888: red and blue swapped.
// ref: FUN_1424e65c0
void SwapRedBlue(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    for (int32_t y = rect[1]; y != 0; y--) {
        for (int32_t x = 0; x < rect[0]; x++) {
            dest[x * 4 + 3] = source[x * 4 + 3];
            dest[x * 4 + 2] = source[x * 4 + 0];
            dest[x * 4 + 1] = source[x * 4 + 1];
            dest[x * 4 + 0] = source[x * 4 + 2];
        }
        source += sourcePitch;
        dest += destPitch;
    }
}

// Mode 1: the source's colour where it is not fully transparent.
// ref: FUN_1424e67c0
void OverlayOpaque(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    for (int32_t y = rect[1]; y != 0; y--) {
        auto out = reinterpret_cast<uint32_t*>(dest);
        auto in = reinterpret_cast<const uint32_t*>(source);
        for (int32_t x = 0; x < rect[0]; x++) {
            uint32_t pixel = in[x];
            if ((pixel & 0xff000000) != 0) {
                out[x] ^= (pixel ^ out[x]) & 0xffffff;
            }
        }
        source += sourcePitch;
        dest += destPitch;
    }
}

// Mode 2: blended by the source's alpha.
// ref: FUN_1424e6840
void BlendByAlpha(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    for (int32_t y = rect[1]; y != 0; y--) {
        for (int32_t x = 0; x < rect[0]; x++) {
            uint8_t alpha = source[x * 4 + 3];
            if (alpha != 0) {
                BlendPixel(reinterpret_cast<uint32_t*>(dest + x * 4), alpha,
                    reinterpret_cast<const uint32_t*>(source + x * 4));
            }
        }
        source += sourcePitch;
        dest += destPitch;
    }
}

// ARGB8888's alpha alone.
// ref: FUN_1424e6370
void ExtractAlpha(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    for (int32_t y = 0; y < rect[1]; y++) {
        uint8_t* out = dest + static_cast<uint32_t>(y * static_cast<int32_t>(destPitch));
        const uint8_t* in = source + static_cast<uint32_t>(y * static_cast<int32_t>(sourcePitch)) + 3;
        for (int32_t x = 0; x < rect[0]; x++) {
            *out++ = *in;
            in += 4;
        }
    }
}

uint8_t ExpandNibble(uint8_t value) {
    return static_cast<uint8_t>((value & 0xf) ^ (value << 4));
}

// ARGB4444 to ABGR8888.
// ref: FUN_1424e6400
void Argb4444ToAbgr8888(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest,
    uint32_t destPitch) {
    for (int32_t y = rect[1]; y != 0; y--) {
        auto in = reinterpret_cast<const uint16_t*>(source);
        uint8_t* out = dest;
        for (int32_t x = 0; x < rect[0]; x++) {
            uint16_t pixel = *in++;
            uint8_t high = static_cast<uint8_t>(pixel >> 8);
            out[3] = static_cast<uint8_t>(((high >> 4) << 4) | (high >> 4));
            out[2] = ExpandNibble(static_cast<uint8_t>(pixel));
            out[1] = ExpandNibble(static_cast<uint8_t>(pixel >> 4));
            out[0] = ExpandNibble(high);
            out += 4;
        }
        source += sourcePitch;
        dest += destPitch;
    }
}

// ARGB4444 to ARGB8888.
// ref: FUN_1424e64e0
void Argb4444ToArgb8888(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest,
    uint32_t destPitch) {
    for (int32_t y = rect[1]; y != 0; y--) {
        auto in = reinterpret_cast<const uint16_t*>(source);
        uint8_t* out = dest;
        for (int32_t x = 0; x < rect[0]; x++) {
            uint16_t pixel = *in++;
            uint8_t high = static_cast<uint8_t>(pixel >> 8);
            out[3] = static_cast<uint8_t>(((high >> 4) << 4) | (high >> 4));
            out[2] = ExpandNibble(high);
            out[1] = ExpandNibble(static_cast<uint8_t>(pixel >> 4));
            out[0] = ExpandNibble(static_cast<uint8_t>(pixel));
            out += 4;
        }
        source += sourcePitch;
        dest += destPitch;
    }
}

// ARGB8888 packed into a 16-bit format: each byte shifted right, then into its place. Two pixels
// a step once the image is wider than one.
// ref: FUN_1424e7680
void PackArgb8888(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint16_t* dest, uint32_t destPitch,
    uint8_t alphaRight, uint8_t redRight, uint8_t greenRight, uint8_t blueRight, uint8_t alphaLeft, uint8_t redLeft,
    uint8_t greenLeft, uint8_t blueLeft) {
    int32_t width = rect[0];
    int32_t rows = rect[1];
    if (width < 2) {
        for (; rows != 0; rows--) {
            const uint8_t* in = source;
            uint16_t* out = dest;
            for (int32_t x = 0; x < width; x++) {
                *out++ = static_cast<uint16_t>(((in[0] >> (blueRight & 0x1f)) << (blueLeft & 0x1f))
                    | ((in[1] >> (greenRight & 0x1f)) << (greenLeft & 0x1f))
                    | ((in[3] >> (alphaRight & 0x1f)) << (alphaLeft & 0x1f))
                    | ((in[2] >> (redRight & 0x1f)) << (redLeft & 0x1f)));
                in += 4;
                width = rect[0];
            }
            source += sourcePitch;
            dest = reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(dest) + destPitch);
        }
        return;
    }
    for (; rows != 0; rows--) {
        const uint8_t* in = source;
        for (int32_t x = 0; x < width; x += 2) {
            uint32_t pair = (static_cast<uint32_t>(in[0] >> (blueRight & 0x1f)) << (blueLeft & 0x1f))
                | (static_cast<uint32_t>(in[3] >> (alphaRight & 0x1f)) << (alphaLeft & 0x1f))
                | (static_cast<uint32_t>(in[1] >> (greenRight & 0x1f)) << (greenLeft & 0x1f))
                | (static_cast<uint32_t>(in[2] >> (redRight & 0x1f)) << (redLeft & 0x1f))
                | (static_cast<uint32_t>(in[4] >> (blueRight & 0x1f)) << ((blueLeft + 0x10) & 0x1f))
                | (static_cast<uint32_t>(in[7] >> (alphaRight & 0x1f)) << ((alphaLeft + 0x10) & 0x1f))
                | (static_cast<uint32_t>(in[6] >> (redRight & 0x1f)) << ((redLeft + 0x10) & 0x1f))
                | (static_cast<uint32_t>(in[5] >> (greenRight & 0x1f)) << ((greenLeft + 0x10) & 0x1f));
            std::memcpy(dest + (x >> 1) * 2, &pair, 4);
            in += 8;
            width = rect[0];
        }
        source += sourcePitch;
        dest = reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(dest) + destPitch);
    }
}

// ref: FUN_1424e6660
void Argb8888ToArgb1555(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest,
    uint32_t destPitch) {
    PackArgb8888(rect, source, sourcePitch, reinterpret_cast<uint16_t*>(dest), destPitch, 7, 3, 3, 3, 15, 10, 5, 0);
}

// ref: FUN_1424e66c0
void Argb8888ToArgb4444(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest,
    uint32_t destPitch) {
    PackArgb8888(rect, source, sourcePitch, reinterpret_cast<uint16_t*>(dest), destPitch, 4, 4, 4, 4, 12, 8, 4, 0);
}

// ref: FUN_1424e68f0
void Argb8888ToRgb565(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest,
    uint32_t destPitch) {
    PackArgb8888(rect, source, sourcePitch, reinterpret_cast<uint16_t*>(dest), destPitch, 8, 3, 2, 3, 0, 11, 5, 0);
}

// DXT1 into ARGB1555.
// ref: FUN_1424e4310
void Dxt1ToArgb1555Clamped(const int32_t* rect, const uint8_t* source, int32_t sourcePitch, uint8_t* dest,
    int32_t destPitch) {
    BlocksClamped<8, 2>(rect, source, sourcePitch, dest, destPitch, DecodeDxt1Sixteen<ColorsArgb1555>());
}

// ref: FUN_1424e6950
void Dxt1ToArgb1555(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    if (!WholeBlocks(rect)) {
        Dxt1ToArgb1555Clamped(rect, source, static_cast<int32_t>(sourcePitch), dest, static_cast<int32_t>(destPitch));
        return;
    }
    BlocksFast<8, 2>(rect, source, sourcePitch, dest, destPitch, DecodeDxt1Sixteen<ColorsArgb1555>());
}

// ref: FUN_1424e4590
void Dxt1ToArgb8888Clamped(const int32_t* rect, const uint8_t* source, int32_t sourcePitch, uint8_t* dest,
    int32_t destPitch) {
    // This one's face offsets are a face's width times 8 and 4, like the rest.
    BlocksClamped<8, 4>(rect, source, sourcePitch, dest, destPitch, DecodeDxt1Argb8888());
}

// ref: FUN_1424e6ac0
void Dxt1ToArgb8888(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    if (!WholeBlocks(rect)) {
        Dxt1ToArgb8888Clamped(rect, source, static_cast<int32_t>(sourcePitch), dest, static_cast<int32_t>(destPitch));
        return;
    }
    BlocksFast<8, 4>(rect, source, sourcePitch, dest, destPitch, DecodeDxt1Argb8888());
}

// ref: FUN_1424e4840
void Dxt1ToRgb565Clamped(const int32_t* rect, const uint8_t* source, int32_t sourcePitch, uint8_t* dest,
    int32_t destPitch) {
    BlocksClamped<8, 2>(rect, source, sourcePitch, dest, destPitch, DecodeDxt1Sixteen<ColorsRgb565>());
}

// ref: FUN_1424e6cf0
void Dxt1ToRgb565(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    if (!WholeBlocks(rect)) {
        Dxt1ToRgb565Clamped(rect, source, static_cast<int32_t>(sourcePitch), dest, static_cast<int32_t>(destPitch));
        return;
    }
    BlocksFast<8, 2>(rect, source, sourcePitch, dest, destPitch, DecodeDxt1Sixteen<ColorsRgb565>());
}

// ref: FUN_1424e4ac0
void Dxt3ToArgb4444Clamped(const int32_t* rect, const uint8_t* source, int32_t sourcePitch, uint8_t* dest,
    int32_t destPitch) {
    BlocksClamped<16, 2>(rect, source, sourcePitch, dest, destPitch, DecodeDxt3Argb4444());
}

// ref: FUN_1424e6f10
void Dxt3ToArgb4444(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    if (!WholeBlocks(rect)) {
        Dxt3ToArgb4444Clamped(rect, source, static_cast<int32_t>(sourcePitch), dest, static_cast<int32_t>(destPitch));
        return;
    }
    BlocksFast<16, 2>(rect, source, sourcePitch, dest, destPitch, DecodeDxt3Argb4444());
}

// ref: FUN_1424e4d90
void Dxt3ToArgb8888Clamped(const int32_t* rect, const uint8_t* source, int32_t sourcePitch, uint8_t* dest,
    int32_t destPitch) {
    BlocksClamped<16, 4>(rect, source, sourcePitch, dest, destPitch, DecodeDxt3Argb8888());
}

// ref: FUN_1424e7150
void Dxt3ToArgb8888(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    if (!WholeBlocks(rect)) {
        Dxt3ToArgb8888Clamped(rect, source, static_cast<int32_t>(sourcePitch), dest, static_cast<int32_t>(destPitch));
        return;
    }
    BlocksFast<16, 4>(rect, source, sourcePitch, dest, destPitch, DecodeDxt3Argb8888());
}

// ref: FUN_1424e5080
void Dxt5ToArgb4444Clamped(const int32_t* rect, const uint8_t* source, int32_t sourcePitch, uint8_t* dest,
    int32_t destPitch) {
    BlocksClamped<16, 2>(rect, source, sourcePitch, dest, destPitch, DecodeDxt5Argb4444());
}

// ref: FUN_1424e30e0
void Dxt5ToArgb4444Blocks(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest,
    uint32_t destPitch) {
    BlocksFast<16, 2>(rect, source, sourcePitch, dest, destPitch, DecodeDxt5Argb4444());
}

// ref: 0x1424e73b0
void Dxt5ToArgb4444(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    if (WholeBlocks(rect)) {
        Dxt5ToArgb4444Blocks(rect, source, sourcePitch, dest, destPitch);
        return;
    }
    Dxt5ToArgb4444Clamped(rect, source, static_cast<int32_t>(sourcePitch), dest, static_cast<int32_t>(destPitch));
}

// ref: FUN_1424e53f0
void Dxt5ToArgb8888Clamped(const int32_t* rect, const uint8_t* source, int32_t sourcePitch, uint8_t* dest,
    int32_t destPitch) {
    BlocksClamped<16, 4>(rect, source, sourcePitch, dest, destPitch, DecodeDxt5Argb8888());
}

// ref: FUN_1424e3340
void Dxt5ToArgb8888Blocks(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest,
    uint32_t destPitch) {
    BlocksFast<16, 4>(rect, source, sourcePitch, dest, destPitch, DecodeDxt5Argb8888());
}

// ref: 0x1424e73e0
void Dxt5ToArgb8888(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    if (WholeBlocks(rect)) {
        Dxt5ToArgb8888Blocks(rect, source, sourcePitch, dest, destPitch);
        return;
    }
    Dxt5ToArgb8888Clamped(rect, source, static_cast<int32_t>(sourcePitch), dest, static_cast<int32_t>(destPitch));
}

// ref: FUN_1424e35a0
void Bc4ToArgb4444Clamped(const int32_t* rect, const uint8_t* source, int32_t sourcePitch, uint8_t* dest,
    int32_t destPitch) {
    // The stale value: the source pointer's low bits, refreshed from the face's source after each
    // row of blocks.
    int32_t height = rect[1];
    int32_t width = rect[0];
    int32_t faceWidth = width == height * 6 ? width / 6 : width;
    int64_t faces = width == height * 6 ? 6 : 1;
    uint16_t low = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(source));
    do {
        uint32_t rowsLeft = static_cast<uint32_t>(height - 1);
        for (int32_t y = 0; y < height; y += 4) {
            uint8_t* rows[4];
            for (int32_t r = 0; r < 4; r++) {
                rows[r] = dest + static_cast<uint32_t>(r * destPitch);
            }
            const uint8_t* block = source + static_cast<uint32_t>((y >> 2) * sourcePitch);
            if (faceWidth > 0) {
                for (int32_t x = 0; x < faceWidth; x += 4) {
                    uint32_t lastRow = static_cast<int32_t>(rowsLeft) < 3 ? rowsLeft : 3;
                    uint32_t columnsLeft = static_cast<uint32_t>(faceWidth - x - 1);
                    uint32_t lastColumn = static_cast<int32_t>(columnsLeft) < 3 ? columnsLeft : 3;
                    uint8_t pixels[32];
                    DecodeBc4Argb4444{ low }(block, pixels);
                    for (uint32_t r = 0; r <= lastRow; r++) {
                        std::memcpy(rows[r], pixels + r * 8, (lastColumn + 1) * 2);
                        rows[r] += (lastColumn + 1) * 2;
                    }
                    block += 8;
                }
                low = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(source));
            }
            rowsLeft -= 4;
        }
        source += static_cast<int64_t>(faceWidth) * 8;
        dest += static_cast<int64_t>(faceWidth) * 2;
    } while (--faces != 0);
}

// ref: FUN_1424e26d0
void Bc4ToArgb4444(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    if (!WholeBlocks(rect)) {
        Bc4ToArgb4444Clamped(rect, source, static_cast<int32_t>(sourcePitch), dest, static_cast<int32_t>(destPitch));
        return;
    }
    DecodeBc4Argb4444 decode = { static_cast<uint16_t>(reinterpret_cast<uintptr_t>(rect) & 0xfff) };
    BlocksFast<8, 2>(rect, source, sourcePitch, dest, destPitch, decode);
}

// ref: FUN_1424e38f0
void Bc4ToArgb8888Clamped(const int32_t* rect, const uint8_t* source, int32_t sourcePitch, uint8_t* dest,
    int32_t destPitch) {
    BlocksClamped<8, 4>(rect, source, sourcePitch, dest, destPitch, DecodeBc4Argb8888());
}

// ref: FUN_1424e2940
void Bc4ToArgb8888(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    if (!WholeBlocks(rect)) {
        Bc4ToArgb8888Clamped(rect, source, static_cast<int32_t>(sourcePitch), dest, static_cast<int32_t>(destPitch));
        return;
    }
    BlocksFast<8, 4>(rect, source, sourcePitch, dest, destPitch, DecodeBc4Argb8888());
}

// ref: FUN_1424e3c00
void Bc5ToArgb4444Clamped(const int32_t* rect, const uint8_t* source, int32_t sourcePitch, uint8_t* dest,
    int32_t destPitch) {
    BlocksClamped<16, 2>(rect, source, sourcePitch, dest, destPitch, DecodeBc5Argb4444());
}

// ref: FUN_1424e2b80
void Bc5ToArgb4444Blocks(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest,
    uint32_t destPitch) {
    BlocksFast<16, 2>(rect, source, sourcePitch, dest, destPitch, DecodeBc5Argb4444());
}

// ref: 0x1424e2e10
void Bc5ToArgb4444(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    if (WholeBlocks(rect)) {
        Bc5ToArgb4444Blocks(rect, source, sourcePitch, dest, destPitch);
        return;
    }
    Bc5ToArgb4444Clamped(rect, source, static_cast<int32_t>(sourcePitch), dest, static_cast<int32_t>(destPitch));
}

// ref: FUN_1424e3fb0
void Bc5ToArgb8888Clamped(const int32_t* rect, const uint8_t* source, int32_t sourcePitch, uint8_t* dest,
    int32_t destPitch) {
    BlocksClamped<16, 4>(rect, source, sourcePitch, dest, destPitch, DecodeBc5Argb8888());
}

// ref: FUN_1424e2e40
void Bc5ToArgb8888Blocks(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest,
    uint32_t destPitch) {
    BlocksFast<16, 4>(rect, source, sourcePitch, dest, destPitch, DecodeBc5Argb8888());
}

// ref: 0x1424e30b0
void Bc5ToArgb8888(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest, uint32_t destPitch) {
    if (WholeBlocks(rect)) {
        Bc5ToArgb8888Blocks(rect, source, sourcePitch, dest, destPitch);
        return;
    }
    Bc5ToArgb8888Clamped(rect, source, static_cast<int32_t>(sourcePitch), dest, static_cast<int32_t>(destPitch));
}

// [source format][destination format][mode] (DAT_1447c7120), filled once (DAT_1447cc5a0).
PIXELCONVERTER s_converters[0x1a][0x1a][4];
bool s_convertersReady;

void Set(int32_t source, int32_t dest, int32_t mode, PIXELCONVERTER converter) {
    s_converters[source][dest][mode] = converter;
}

}

// ref: FUN_14030e7e0
void DxtAlphaRamp(uint8_t ramp[8]) {
    uint32_t a0 = ramp[0];
    uint32_t a1 = ramp[1];
    if (a1 < a0) {
        ramp[2] = static_cast<uint8_t>((a0 * 6 + 3 + a1) / 7);
        ramp[3] = static_cast<uint8_t>((a1 * 2 + a0 * 5 + 3) / 7);
        ramp[4] = static_cast<uint8_t>((a0 * 4 + 3 + a1 * 3) / 7);
        ramp[5] = static_cast<uint8_t>((a1 * 4 + 3 + a0 * 3) / 7);
        ramp[6] = static_cast<uint8_t>((a0 * 2 + 3 + a1 * 5) / 7);
        ramp[7] = static_cast<uint8_t>((a0 + 3 + a1 * 6) / 7);
    } else {
        ramp[2] = static_cast<uint8_t>((a1 + 2 + a0 * 4) / 5);
        ramp[3] = static_cast<uint8_t>((a1 * 2 + 2 + a0 * 3) / 5);
        ramp[4] = static_cast<uint8_t>((a0 * 2 + 2 + a1 * 3) / 5);
        ramp[5] = static_cast<uint8_t>((a0 + 2 + a1 * 4) / 5);
        ramp[6] = 0;
        ramp[7] = 0xff;
    }
}

// The four colours of a DXT1 block as ARGB8888 (three-colour mode: the fourth transparent black).
// ref: FUN_14030a2b0
void DxtColorsDxt1(const uint16_t* block, uint32_t colors[4]) {
    ENDPOINTS e(block[0], block[1]);
    colors[0] = Argb(0xff, e.r0, e.g0, e.b0);
    colors[1] = Argb(0xff, e.r1, e.g1, e.b1);
    if (block[1] < block[0]) {
        colors[2] = Argb(0xff, (e.r0 * 2 + 1 + e.r1) / 3, (e.g0 * 2 + 1 + e.g1) / 3, (e.b0 * 2 + 1 + e.b1) / 3);
        colors[3] = Argb(0xff, (e.r1 * 2 + 1 + e.r0) / 3, (e.g1 * 2 + 1 + e.g0) / 3, (e.b1 * 2 + 1 + e.b0) / 3);
        return;
    }
    colors[2] = Argb(0xff, (e.r1 + e.r0) >> 1, (e.g1 + e.g0) >> 1, (e.b1 + e.b0) >> 1);
    colors[3] = 0;
}

// The four colours of a DXT3/DXT5 block (always four-colour) as ARGB8888.
// ref: FUN_14030a120
void DxtColorsDxt3(const uint16_t* block, uint32_t colors[4]) {
    ENDPOINTS e(block[0], block[1]);
    colors[0] = Argb(0xff, e.r0, e.g0, e.b0);
    colors[1] = Argb(0xff, e.r1, e.g1, e.b1);
    colors[2] = Argb(0xff, (e.r0 * 2 + 1 + e.r1) / 3, (e.g0 * 2 + 1 + e.g1) / 3, (e.b0 * 2 + 1 + e.b1) / 3);
    colors[3] = Argb(0xff, (e.r1 * 2 + 1 + e.r0) / 3, (e.g1 * 2 + 1 + e.g0) / 3, (e.b1 * 2 + 1 + e.b0) / 3);
}

// ref: FUN_1424e6060
bool PixelConvert(const int32_t* rect, int32_t mode, const uint8_t* source, uint32_t sourcePitch, int32_t sourceFormat,
    uint8_t* dest, uint32_t destPitch, int32_t destFormat) {
    if (!s_convertersReady) {
        Set(2, 2, 1, &OverlayOpaque);
        Set(1, 2, 0, &SwapRedBlue);
        Set(2, 1, 0, &SwapRedBlue);
        Set(0x16, 0x16, 0, &CopyBc5);
        Set(2, 2, 2, &BlendByAlpha);
        Set(2, 4, 0, &Argb8888ToArgb1555);
        Set(2, 3, 0, &Argb8888ToArgb4444);
        Set(2, 5, 0, &Argb8888ToRgb565);
        Set(3, 2, 0, &Argb4444ToArgb8888);
        Set(3, 1, 0, &Argb4444ToAbgr8888);
        Set(7, 7, 0, &CopyDxt1);
        Set(8, 8, 0, &CopyDxt35);
        Set(9, 9, 0, &CopyDxt35);
        Set(10, 10, 0, &CopyBc4);
        Set(11, 0x16, 0, &SwapBc5);
        Set(7, 5, 0, &Dxt1ToRgb565);
        Set(7, 4, 0, &Dxt1ToArgb1555);
        Set(7, 2, 0, &Dxt1ToArgb8888);
        Set(7, 13, 0, &Dxt1ToArgb8888);
        Set(8, 3, 0, &Dxt3ToArgb4444);
        Set(8, 2, 0, &Dxt3ToArgb8888);
        Set(8, 13, 0, &Dxt3ToArgb8888);
        Set(9, 3, 0, &Dxt5ToArgb4444);
        Set(9, 2, 0, &Dxt5ToArgb8888);
        Set(9, 13, 0, &Dxt5ToArgb8888);
        Set(10, 3, 0, &Bc4ToArgb4444);
        Set(10, 2, 0, &Bc4ToArgb8888);
        Set(11, 2, 0, &Bc5ToArgb8888);
        Set(0x16, 2, 0, &Bc5ToArgb8888);
        Set(13, 13, 0, &Copy64);
        Set(14, 14, 0, &Copy64);
        Set(17, 17, 0, &Copy128);
        Set(21, 21, 0, &Copy8);
        Set(2, 21, 0, &ExtractAlpha);
        Set(11, 11, 0, &CopyBc5);
        Set(15, 15, 0, &Copy32);
        Set(1, 1, 0, &Copy32);
        Set(2, 2, 0, &Copy32);
        Set(3, 3, 0, &Copy16);
        Set(4, 4, 0, &Copy16);
        Set(11, 3, 0, &Bc5ToArgb4444);
        Set(0x16, 3, 0, &Bc5ToArgb4444);
        Set(6, 6, 0, &Copy16);
        Set(12, 12, 0, &Copy32);
        Set(16, 16, 0, &Copy32);
        Set(18, 18, 0, &Copy32);
        Set(20, 20, 0, &Copy32);
        Set(23, 23, 0, &Copy16);
        Set(24, 24, 0, &Copy16);
        Set(25, 25, 0, &Copy16);
        Set(5, 5, 0, &Copy16);
        s_convertersReady = true;
    }
    PIXELCONVERTER converter = s_converters[sourceFormat][destFormat][mode];
    if (converter == nullptr) {
        return false;
    }
    converter(rect, source, sourcePitch, dest, destPitch);
    return true;
}

// ref: FUN_1424e6320
uint32_t PixelRowPitch(int32_t format, uint32_t width, uint32_t height) {
    uint32_t w = width;
    if (static_cast<uint32_t>(format - 7) < 5) {
        if (width == height * 6) {
            w = (height > 4 ? height : 4) * 6;
        } else {
            w = width > 4 ? width : 4;
        }
    }
    return (w >> (static_cast<uint8_t>(s_blockShift[format]) & 0x1f)) * s_unitBytes[format];
}

// The tables' neighbours, for the readers that run past them.
const uint8_t* PixelStaticBlock() {
    return s_staticBlock;
}
