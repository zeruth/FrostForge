// The image library against hand-worked values: mip sizes, the DXT palettes, block decoding,
// the converters, and a synthetic BLP through the parser and decoder. Exits non-zero on failure.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "images/Images.hpp"

namespace {

int s_failures;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        s_failures++;
    }
}

}

int main() {
    // Sizes: DXT1 is 4 bits a pixel, blocks at least 4x4; ARGB8888 4 bytes.
    Check(ImageMipLevels(256, 64, false) == 9, "levels down to 1x1");
    Check(ImageMipLevels(6 * 32, 32, true) == 6, "a cube counts one face");
    Check(ImageLevelSize(0, 256, 256, IMAGEFMT_DXT1, false) == 0x8000, "DXT1 256x256");
    Check(ImageLevelSize(7, 256, 256, IMAGEFMT_DXT1, false) == 8, "DXT1 2x2 is one block");
    Check(ImageLevelSize(0, 64, 32, IMAGEFMT_ARGB8888, false) == 64 * 32 * 4, "ARGB8888");
    Check(ImageLevelSize(0, 8, 8, IMAGEFMT_DXT5, false) == 64, "DXT5 8x8 is four blocks");

    MIPBITS* bits = MipBitsCreate(IMAGEFMT_ARGB8888, 4, 4, false);
    Check(bits[0] != nullptr && (reinterpret_cast<uintptr_t>(bits[0]) & 0xf) == 0, "level 0 aligned");
    Check(bits[1] == bits[0] + 64 && bits[2] == bits[1] + 16, "levels packed");
    MipBitsFree(&bits);
    Check(bits == nullptr, "freed");

    // The alpha ramp: 8 values for a0 > a1, 6 plus 0 and 255 otherwise.
    uint8_t ramp[8] = { 210, 70 };
    DxtAlphaRamp(ramp);
    Check(ramp[2] == 190 && ramp[4] == 150 && ramp[7] == 90, "eight-value ramp");
    uint8_t ramp6[8] = { 50, 100 };
    DxtAlphaRamp(ramp6);
    Check(ramp6[2] == 60 && ramp6[5] == 90 && ramp6[6] == 0 && ramp6[7] == 255, "six-value ramp");

    // DXT1, red over blue: the four-colour mode.
    uint16_t block[4] = { 0xf800, 0x001f, 0, 0 };
    uint32_t colors[4];
    DxtColorsDxt1(block, colors);
    Check(colors[0] == 0xffff0000 && colors[1] == 0xff0000ff, "endpoints");
    Check(colors[2] == 0xffaa0055 && colors[3] == 0xff5500aa, "thirds");
    // c0 <= c1: three colours and transparent black.
    uint16_t three[4] = { 0x001f, 0xf800, 0, 0 };
    DxtColorsDxt1(three, colors);
    Check(colors[2] == 0xff7f007f && colors[3] == 0, "three-colour mode");

    // A DXT1 block of index 2 everywhere (0xaa), to ARGB8888.
    uint8_t dxt1[8] = { 0x00, 0xf8, 0x1f, 0x00, 0xaa, 0xaa, 0xaa, 0xaa };
    uint32_t pixels[16];
    int32_t rect[2] = { 4, 4 };
    Check(PixelConvert(rect, 0, dxt1, 8, 7, reinterpret_cast<uint8_t*>(pixels), 16, 2), "DXT1 converter exists");
    bool all = true;
    for (uint32_t p : pixels) {
        all = all && p == 0xffaa0055;
    }
    Check(all, "DXT1 decodes");

    // ARGB4444 to ARGB8888: nibbles doubled.
    uint16_t argb4444 = 0x8f40;
    uint32_t out = 0;
    int32_t one[2] = { 1, 1 };
    Check(PixelConvert(one, 0, reinterpret_cast<uint8_t*>(&argb4444), 2, 3, reinterpret_cast<uint8_t*>(&out), 4, 2), "4444 converter");
    Check(out == 0x88ff4400, "4444 to 8888");
    Check(!PixelConvert(one, 0, reinterpret_cast<uint8_t*>(&argb4444), 2, 4, reinterpret_cast<uint8_t*>(&out), 4, 2),
        "no converter for 1555 to 8888");

    // A palettized 2x2 BLP with 8-bit alpha, through the parser and decoder.
    std::vector<uint8_t> file(0x494 + 8, 0);
    std::memcpy(file.data(), "BLP2", 4);
    file[4] = 1;
    file[8] = 1;        // palettized
    file[9] = 8;        // alpha depth
    file[10] = 2;
    *reinterpret_cast<uint32_t*>(&file[0xc]) = 2;
    *reinterpret_cast<uint32_t*>(&file[0x10]) = 2;
    *reinterpret_cast<uint32_t*>(&file[0x14]) = 0x494;
    *reinterpret_cast<uint32_t*>(&file[0x54]) = 8;
    auto palette = reinterpret_cast<uint32_t*>(&file[0x94]);
    palette[1] = 0x00102030;
    palette[2] = 0x00405060;
    const uint8_t level[8] = { 1, 2, 2, 1, 0x11, 0x22, 0x33, 0x44 };
    std::memcpy(&file[0x494], level, 8);
    BLPIMAGE image = {};
    Check(BlpImageParse(&image, file.data(), static_cast<uint32_t>(file.size()), true), "parse");
    Check(image.m_mipCount == 1 && image.m_compression == 1 && image.m_type == 1, "header fields");
    MIPBITS* decoded = MipBitsCreate(IMAGEFMT_ARGB8888, 2, 2, false);
    Check(BlpImageDecode(&image, IMAGEFMT_ARGB8888, decoded, 0), "decode");
    auto argb = reinterpret_cast<uint32_t*>(decoded[0]);
    Check(argb[0] == 0x11102030 && argb[1] == 0x22405060 && argb[3] == 0x44102030, "palette and alpha");
    MipBitsFree(&decoded);

    if (s_failures == 0) {
        std::printf("ImagesTest: all passed\n");
    }
    return s_failures == 0 ? 0 : 1;
}
