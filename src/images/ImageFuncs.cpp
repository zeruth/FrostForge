#include "images/Images.hpp"

#include <cstring>

#include "storm/Memory.hpp"
#include "storm/String.hpp"

namespace {

const char s_funcsFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Images\\funcs.cpp";

// Formats stored as 4x4 blocks: DXT1, DXT3, DXT5, BC4, BC5 (the reference's 0xc83 mask).
bool IsBlockFormat(uint32_t format) {
    return format < 0xc && ((0xc83u >> format) & 1) != 0;
}

}

// ref: FUN_1406a71c0
uint32_t ImageMipLevels(uint32_t width, uint32_t height, bool cube) {
    uint32_t w = width;
    uint32_t levels = 1;
    if (cube) {
        w = width / 6;
    }
    while (w > 1 || height > 1) {
        levels++;
        w = w >> 1 != 0 ? w >> 1 : 1;
        height = height >> 1 != 0 ? height >> 1 : 1;
    }
    return levels;
}

// ref: FUN_1406a7390
uint32_t ImageLevelSize(uint32_t level, uint32_t width, uint32_t height, uint32_t format, bool cube) {
    uint32_t w = width >> (level & 0x1f);
    if (w == 0) {
        w = 1;
    }
    uint32_t h = height >> (level & 0x1f);
    if (h == 0) {
        h = 1;
    }
    if (IsBlockFormat(format)) {
        uint32_t blockW;
        uint32_t blockH = h > 4 ? h : 4;
        if (!cube) {
            blockW = w > 4 ? w : 4;
        } else {
            blockW = blockH * 6;
        }
        w = (blockW + 3) & ~3u;
        h = (blockH + 3) & ~3u;
    }
    if (format == IMAGEFMT_ARGB2565) {
        uint32_t alpha = (w * h) >> 2;
        if (alpha == 0) {
            alpha = 1;
        }
        return alpha + w * h * 2;
    }
    switch (format) {
    case 0:
    case 10:
        return (4 * w * h) >> 3;
    case 1:
    case 6:
    case 7:
    case 11:
        return (w * 8 * h) >> 3;
    case 2:
        return (w * 0x20 * h) >> 3;
    case 3:
    case 4:
    case 5:
        return (w * 0x10 * h) >> 3;
    case 0xc:
        return (w * 0x40 * h) >> 3;
    default:
        return 0;
    }
}

// The reference inlines the level size here; it is the same per level.
// ref: FUN_1406a7220
uint32_t ImageChainSize(uint32_t levels, uint32_t width, uint32_t height, uint32_t format, bool cube) {
    uint32_t size = 0;
    for (uint32_t level = 0; level < levels; level++) {
        size += ImageLevelSize(level, width, height, format, cube);
    }
    return size;
}

// ref: FUN_1406a7b90
MIPBITS* MipBitsCreate(uint32_t format, uint32_t width, uint32_t height, bool cube) {
    uint32_t levels = ImageMipLevels(width, height, cube);
    uint32_t data = ImageChainSize(levels, width, height, format, cube);
    uint32_t total = data + levels * 8;
    auto block = static_cast<uint8_t*>(SMemAlloc(total + 0x10, 0x18, s_funcsFile, SStrHashFnv1a(s_funcsFile), 100, 0));
    std::memset(block + total, 0, 0x10);
    uint64_t offset = ((reinterpret_cast<uintptr_t>(block) + levels * 8 + 0xf) & ~static_cast<uintptr_t>(0xf))
        - reinterpret_cast<uintptr_t>(block);
    auto bits = reinterpret_cast<MIPBITS*>(block);
    bits[0] = nullptr;
    bits[1] = nullptr;
    for (uint32_t level = 0; level < levels; level++) {
        bits[level] = block + offset;
        offset += ImageLevelSize(level, width, height, format, cube);
    }
    return bits;
}

// ref: FUN_1406a7cf0
void MipBitsFree(MIPBITS** bits) {
    if (*bits != nullptr) {
        SMemFree(*bits, s_funcsFile, 0xa0, 0);
        *bits = nullptr;
    }
}

// ref: FUN_1406a7d30
MIPBITS* MipBitsCreatePointers(uint32_t width, uint32_t height, bool cube) {
    uint32_t levels = ImageMipLevels(width, height, cube);
    return static_cast<MIPBITS*>(SMemAlloc(levels * 8, 0x18, s_funcsFile, SStrHashFnv1a(s_funcsFile), 0x81, 0));
}

// ref: FUN_1406a7de0
void MipBitsInit(uint32_t format, uint32_t width, uint32_t height, bool cube, MIPBITS* bits) {
    uint32_t levels = ImageMipLevels(width, height, cube);
    uint32_t offset = 0;
    for (uint32_t level = 0; level < levels; level++) {
        bits[level] = reinterpret_cast<uint8_t*>(bits) + offset + levels * 8;
        offset += ImageLevelSize(level, width, height, format, cube);
    }
}

// ref: FUN_1406a7ec0
uint32_t ImageGcd(uint32_t a, uint32_t b) {
    while (b != 0) {
        uint32_t next = a % b;
        a = b;
        b = next;
    }
    return a;
}

namespace {

// The general case: each destination pixel covers a fractional span of source pixels, weighted
// by how much of each it covers.
// ref: FUN_1406a74d0
void ResampleArea(uint32_t* dest, uint32_t destWidth, uint32_t destHeight, const uint8_t* source, uint32_t sourceWidth,
    uint32_t sourceHeight) {
    uint32_t gx = sourceWidth;
    if (destWidth != 0) {
        gx = ImageGcd(destWidth, sourceWidth);
    }
    uint64_t xParts = static_cast<uint64_t>(destWidth) / gx;
    uint32_t gy = sourceHeight;
    if (destHeight != 0) {
        gy = ImageGcd(destHeight, sourceHeight);
    }
    uint64_t yParts = static_cast<uint64_t>(destHeight) / gy;
    uint32_t xStep = sourceWidth / gx;
    uint32_t yStep = sourceHeight / gy;
    uint32_t row = 0;           // the source row the destination row starts in
    uint32_t topWeight = static_cast<uint32_t>(yParts);
    uint32_t* out = dest;
    for (uint32_t y = 0; y < destHeight;) {
        y++;
        uint64_t yEnd = static_cast<uint64_t>(y * yStep);
        uint32_t lastRow = static_cast<uint32_t>(yEnd / yParts);
        int32_t bottomWeight = static_cast<int32_t>(yEnd % yParts);
        uint32_t column = 0;
        uint32_t leftWeight = static_cast<uint32_t>(xParts);
        for (uint32_t x = 0; x < destWidth;) {
            x++;
            uint64_t xEnd = static_cast<uint64_t>(x * xStep);
            uint32_t lastColumn = static_cast<uint32_t>(xEnd / xParts);
            int32_t rightWeight = static_cast<int32_t>(xEnd % xParts);
            int64_t alpha = 0;
            int64_t weight = 0;
            int64_t sum0 = 0;
            int64_t sum1 = 0;
            int64_t sum2 = 0;
            int64_t weighted0 = 0;
            int64_t weighted1 = 0;
            int64_t weighted2 = 0;
            uint32_t pixel;
            if (lastRow < row) {
                pixel = static_cast<uint32_t>(static_cast<uint8_t>(sum2 / weight) << 16
                    | static_cast<uint8_t>(sum1 / weight) << 8 | static_cast<uint8_t>(sum0 / weight));
            } else {
                const uint8_t* line = source + static_cast<uint64_t>(row * sourceWidth) * 4;
                uint64_t rowWeight = topWeight;
                for (uint32_t r = row; r <= lastRow; r++) {
                    int32_t wy = static_cast<int32_t>(rowWeight);
                    if (r == lastRow && (wy = bottomWeight) == 0) {
                        break;
                    }
                    uint64_t colWeight = leftWeight;
                    for (uint32_t c = column; c <= lastColumn; c++) {
                        int32_t wx = static_cast<int32_t>(colWeight);
                        if (c == lastColumn && (wx = rightWeight) == 0) {
                            break;
                        }
                        uint32_t w = static_cast<uint32_t>(wx * wy);
                        uint8_t b1 = line[1 + c * 4];
                        uint8_t a = line[3 + c * 4];
                        uint8_t b0 = line[c * 4];
                        uint8_t b2 = line[2 + c * 4];
                        alpha += static_cast<uint64_t>(a * w);
                        weighted2 += static_cast<uint64_t>(b2 * a * w);
                        weighted1 += static_cast<uint64_t>(b1 * a * w);
                        weighted0 += static_cast<uint64_t>(b0 * a * w);
                        weight += w;
                        sum2 += static_cast<uint64_t>(b2 * w);
                        sum1 += static_cast<uint64_t>(b1 * w);
                        sum0 += static_cast<uint64_t>(w * b0);
                        colWeight = xParts;
                    }
                    rowWeight = yParts;
                    line += static_cast<uint64_t>(sourceWidth) * 4;
                }
                if (alpha == 0) {
                    pixel = static_cast<uint32_t>(static_cast<uint8_t>(sum2 / weight) << 16
                        | static_cast<uint8_t>(sum1 / weight) << 8 | static_cast<uint8_t>(sum0 / weight));
                } else {
                    pixel = static_cast<uint32_t>(static_cast<uint8_t>(alpha / weight)) << 24
                        | static_cast<uint32_t>(static_cast<uint8_t>(weighted2 / alpha)) << 16
                        | static_cast<uint32_t>(static_cast<uint8_t>(weighted1 / alpha)) << 8
                        | static_cast<uint8_t>(weighted0 / alpha);
                }
            }
            leftWeight = static_cast<uint32_t>(xParts) - rightWeight;
            *out++ = pixel;
            column = lastColumn;
        }
        topWeight = static_cast<uint32_t>(yParts) - bottomWeight;
        row = lastRow;
    }
}

}

// Integer ratios are a plain box filter; anything else goes to the area resampler.
// ref: FUN_1406a78a0
void ImageResample(uint32_t* dest, uint32_t destWidth, uint32_t destHeight, const uint8_t* source,
    uint32_t sourceWidth, uint32_t sourceHeight) {
    uint64_t xStep = static_cast<uint64_t>(sourceWidth) / destWidth;
    uint64_t yStep = static_cast<uint64_t>(sourceHeight) / destHeight;
    int32_t xs = static_cast<int32_t>(xStep);
    int32_t ys = static_cast<int32_t>(yStep);
    if (static_cast<uint32_t>(xs) * destWidth != sourceWidth || static_cast<uint32_t>(ys) * destHeight != sourceHeight) {
        ResampleArea(dest, destWidth, destHeight, source, sourceWidth, sourceHeight);
        return;
    }
    uint32_t* out = dest;
    for (uint32_t row = destHeight; row != 0; row--) {
        const uint8_t* cell = source;
        for (uint32_t column = destWidth; column != 0; column--) {
            int64_t weighted0 = 0;
            int64_t weighted1 = 0;
            int64_t weighted2 = 0;
            int64_t alpha = 0;
            int64_t sum0 = 0;
            int64_t sum1 = 0;
            int64_t sum2 = 0;
            int64_t count = 0;
            uint32_t pixel;
            if (ys == 0) {
                pixel = static_cast<uint32_t>(static_cast<uint8_t>(sum2 / count) << 16
                    | static_cast<uint8_t>(sum1 / count) << 8 | static_cast<uint8_t>(sum0 / count));
            } else {
                const uint8_t* line = cell;
                for (uint64_t y = yStep; y != 0; y--) {
                    if (xs != 0) {
                        count += xStep;
                        const uint8_t* p = line;
                        for (uint64_t x = xStep; x != 0; x--) {
                            uint8_t a = p[3];
                            uint8_t b1 = p[1];
                            uint8_t b2 = p[2];
                            uint8_t b0 = p[0];
                            alpha += a;
                            sum2 += b2;
                            sum1 += b1;
                            sum0 += b0;
                            weighted2 += static_cast<uint32_t>(b2) * a;
                            weighted1 += static_cast<uint32_t>(b1) * a;
                            weighted0 += static_cast<uint32_t>(b0) * a;
                            p += 4;
                        }
                    }
                    line += static_cast<uint64_t>(sourceWidth) * 4;
                }
                if (alpha == 0) {
                    pixel = static_cast<uint32_t>(static_cast<uint8_t>(sum2 / count) << 16
                        | static_cast<uint8_t>(sum1 / count) << 8 | static_cast<uint8_t>(sum0 / count));
                } else {
                    pixel = static_cast<uint32_t>(static_cast<uint8_t>(alpha / static_cast<int64_t>(static_cast<uint32_t>(ys * xs)))) << 24
                        | static_cast<uint32_t>(static_cast<uint8_t>(weighted2 / alpha)) << 16
                        | static_cast<uint32_t>(static_cast<uint8_t>(weighted1 / alpha)) << 8
                        | static_cast<uint8_t>(weighted0 / alpha);
                }
            }
            cell += xStep * 4;
            *out++ = pixel;
        }
        source = cell + static_cast<uint64_t>((ys - 1) * sourceWidth) * 4;
    }
}
