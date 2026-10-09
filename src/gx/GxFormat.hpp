#ifndef GX_GX_FORMAT_HPP
#define GX_GX_FORMAT_HPP

// Per-format facts of the Gx texture formats (GXTEXFMT_*): bits a pixel, block bytes, row
// pitch, and the pixel converter's numbering.

#include <cstdint>

// The pixel converter's format for a Gx format (DAT_142d41c80).
uint32_t GxFormatPixelFormat(uint8_t format);
// Bits a pixel (4 for the 8-byte block formats).
uint32_t GxFormatBits(uint8_t format);
// Bytes a block for the block formats, bytes a pixel for the rest.
uint32_t GxFormatBlockBytes(uint8_t format);
// The bytes of a row of `width` pixels (block formats: a row of blocks per 4 pixel rows, width
// rounded up to 4).
uint32_t GxFormatRowPitch(uint8_t format, uint32_t width);

#endif
