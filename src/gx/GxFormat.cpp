#include "gx/GxFormat.hpp"

namespace {

const uint32_t s_pixelFormats[0x24] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0x15, 0x15, 0x19, 0, 1, 0x18, 0x17, 0xc,
    0xd, 0xe, 0, 0xf, 0x10, 0x11, 0x12, 0, 0x13, 0x14, 0, 0x16, 1, 2, 0x15, 0 };

}

// ref: FUN_14228a0a0
uint32_t GxFormatPixelFormat(uint8_t format) {
    return s_pixelFormats[format];
}

// ref: FUN_14229d040
uint32_t GxFormatBits(uint8_t format) {
    const uint32_t bits[0x23] = { 0, 0x20, 0x20, 0x10, 0x10, 0x10, 0x10, 4, 8, 8, 4, 8, 8, 8, 0x10, 0x20, 0x20, 0x10,
        0x10, 0x20, 0x40, 0x40, 0x20, 0x20, 0x20, 0x80, 0x20, 0x10, 0x20, 0x20, 0x40, 8, 0x20, 0x20, 8 };
    return bits[format];
}

// ref: FUN_140318270
uint32_t GxFormatBlockBytes(uint8_t format) {
    const uint32_t bytes[0x23] = { 0, 4, 4, 2, 2, 2, 2, 8, 0x10, 0x10, 8, 0x10, 1, 1, 2, 4, 4, 2, 2, 4, 8, 8, 4, 4, 4,
        0x10, 4, 2, 4, 4, 8, 0x10, 4, 4, 1 };
    return bytes[format];
}

// ref: FUN_140318130
uint32_t GxFormatRowPitch(uint8_t format, uint32_t width) {
    if (format == 7 || format == 8 || format == 9 || format == 10 || format == 11) {
        width = (width + 3) & ~3u;
    }
    const uint32_t bytes[0x23] = { 0, 4, 4, 2, 2, 2, 2, 2, 4, 4, 2, 4, 1, 1, 2, 4, 4, 2, 2, 4, 8, 8, 4, 4, 4, 0x10, 4,
        2, 4, 4, 8, 4, 4, 4, 1 };
    return width * bytes[format];
}
