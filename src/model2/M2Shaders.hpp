#ifndef MODEL2_M2_SHADERS_HPP
#define MODEL2_M2_SHADERS_HPP

// Which registered vertex and pixel shaders a batch's shader id selects
// (Engine/Source/Model2/M2SharedProcedural.cpp): plain ids by their combiner bits and texture
// count, ids with the top bit set by the M2Effect table.

#include <cstdint>

class CShaderEffect;

// ref: FUN_1411fb770
uint32_t M2GetVertexShaderId(uint16_t textureCount, uint16_t shaderId);
uint32_t M2GetPixelShaderKind(int32_t textureCount, uint16_t shaderId);
// ref: FUN_1411fb4e0
uint32_t M2GetPixelShaderId(uint16_t textureCount, uint16_t shaderId);
// The vertex shader kind of a shader id and how many texture coordinates it reads
// (FUN_1411fb670).
uint32_t M2GetVertexShaderKind(uint16_t textureCount, uint16_t shaderId, uint16_t* texCoords);
// "Diffuse_T1Combiners_Opaque", made when missing (FUN_1411a5a40).
CShaderEffect* M2FallbackEffect();

// The projected effects by texture count and lighting (0x142cfb060), and whether a blend mode
// is lit when projected (DAT_1427228d0).
extern const char* const g_m2ProjectedEffects[10];
extern const uint8_t g_m2ProjectedBlendLit[8];

#endif
