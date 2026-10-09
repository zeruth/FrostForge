#ifndef IMAGES_IMAGES_HPP
#define IMAGES_IMAGES_HPP

// Engine/Source/Images: mipmapped pixel storage (funcs.cpp), the BLP reader, and the pixel
// format converters textures are uploaded through.

#include <cstdint>

// The storage formats of a mip chain (what the BLP reader and the texture system decode to).
enum : uint32_t {
    IMAGEFMT_DXT1 = 0,
    IMAGEFMT_DXT3 = 1,
    IMAGEFMT_ARGB8888 = 2,
    IMAGEFMT_ARGB1555 = 3,
    IMAGEFMT_ARGB4444 = 4,
    IMAGEFMT_RGB565 = 5,
    IMAGEFMT_A8 = 6,
    IMAGEFMT_DXT5 = 7,
    IMAGEFMT_UNKNOWN8 = 8,
    IMAGEFMT_ARGB2565 = 9,          // RGB565 followed by a 2-bit alpha plane
    IMAGEFMT_BC4 = 10,
    IMAGEFMT_BC5 = 11,
    IMAGEFMT_RGBA16 = 12,
};

// A mip chain: a pointer per level, then the levels themselves, from one block.
typedef uint8_t* MIPBITS;

// Levels down to 1x1; a cube map's six faces sit side by side (width 6 x height).
uint32_t ImageMipLevels(uint32_t width, uint32_t height, bool cube);
uint32_t ImageLevelSize(uint32_t level, uint32_t width, uint32_t height, uint32_t format, bool cube);
uint32_t ImageChainSize(uint32_t levels, uint32_t width, uint32_t height, uint32_t format, bool cube);
MIPBITS* MipBitsCreate(uint32_t format, uint32_t width, uint32_t height, bool cube);
// Lays the level pointers out over an existing block made by MipBitsCreate.
void MipBitsInit(uint32_t format, uint32_t width, uint32_t height, bool cube, MIPBITS* bits);
void MipBitsFree(MIPBITS** bits);
// Only the pointer table, for levels that live elsewhere.
MIPBITS* MipBitsCreatePointers(uint32_t width, uint32_t height, bool cube);
uint32_t ImageGcd(uint32_t a, uint32_t b);
// Box-filters an ARGB8888 image to a smaller size, alpha weighted.
void ImageResample(uint32_t* dest, uint32_t destWidth, uint32_t destHeight, const uint8_t* source,
    uint32_t sourceWidth, uint32_t sourceHeight);

// A converter: rect {width, height}, source and its pitch, destination and its pitch.
typedef void (*PIXELCONVERTER)(const int32_t* rect, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest,
    uint32_t destPitch);

// Converts between the pixel formats (the Gx texture format numbering). `mode` 0 converts,
// 1 overlays where the source is opaque, 2 blends by the source's alpha.
bool PixelConvert(const int32_t* rect, int32_t mode, const uint8_t* source, uint32_t sourcePitch, int32_t sourceFormat,
    uint8_t* dest, uint32_t destPitch, int32_t destFormat);
// The pitch of a row (of blocks, for the block formats) of a pixel format.
uint32_t PixelRowPitch(int32_t format, uint32_t width, uint32_t height);

// The DXT palettes: the BC4-style alpha ramp (8 bytes from the first two), and the colour
// endpoints of a block as ARGB8888.
void DxtAlphaRamp(uint8_t ramp[8]);
void DxtColorsDxt1(const uint16_t* block, uint32_t colors[4]);
void DxtColorsDxt3(const uint16_t* block, uint32_t colors[4]);

// A parsed BLP (0xe8 bytes): the levels in place in the file's data.
struct BLPIMAGE {
    uint8_t* m_mips[16];            // 0x00
    uint32_t m_mipSizes[16];        // 0x80
    uint32_t m_width;               // 0xc0
    uint32_t m_height;              // 0xc4
    uint32_t m_mipCount;            // 0xc8
    uint32_t m_compression;         // 0xcc: 1 palette, 2 DXT, 3 ARGB8888, 4 raw
    uint32_t m_format;              // 0xd0: IMAGEFMT_*
    uint32_t m_alphaDepth;          // 0xd4
    uint32_t m_type;                // 0xd8: 1 2D, 2 cube
    uint32_t m_flags;               // 0xdc: 1 has mips listed, 2 an embedded lower-resolution copy
    const uint32_t* m_palette;      // 0xe0
};
static_assert(sizeof(BLPIMAGE) == 0xe8, "BLPIMAGE is 0xe8 bytes");

bool BlpImageParse(BLPIMAGE* image, const uint8_t* data, uint32_t size, bool skipLowRes);
void BlpImageInit(BLPIMAGE* image, MIPBITS* mips, uint32_t width, uint32_t height, uint32_t mipCount,
    uint32_t compression, uint32_t format, uint32_t alphaDepth, uint32_t type);
// Decodes the levels from `firstMip` on into `bits` in `format`.
bool BlpImageDecode(BLPIMAGE* image, uint32_t format, MIPBITS* bits, uint32_t firstMip);

#endif
