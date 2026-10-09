#ifndef GX_GX_SHADER_HPP
#define GX_GX_SHADER_HPP

// GxShader (Engine/Source/Gx/src/GxShader.cpp): the compiled shaders, loaded from the game's
// files by FileDataID out of their "TAFG" containers, one blob per shader family.

#include "blz/Vector.hpp"
#include <cstdint>

// The scratch a permute is unpacked into: blz::embedded_vector<uint8_t, 0x4000> (tag ".E").
extern const char s_shaderBufferTag[];
using GXSHADERBUFFER = blz::embedded_vector<uint8_t, 0x4000, s_shaderBufferTag>;

// The family and caching mode Gx loads shaders for (what DeviceCreate passes).
struct GXSHADERSETTINGS {
    int32_t m_family;               // 0x00: 0 dx_4_0, 1 dx_5_0, 2 dx_6_0, 3 mtl_1_1
    uint8_t m_cacheless;            // 0x04: load on use rather than all at once
    uint8_t m_05;                   // 0x05
};

// A shader permutation as the device builds pipelines from it.
struct GXSHADERPERMUTE {
    int32_t m_00;                   // 0x00
    int32_t m_04;                   // 0x04
    uint32_t m_08;                  // 0x08
    uint8_t m_0c[0xc];
    uint64_t m_18;                  // 0x18
    uint64_t m_20;                  // 0x20
    uint8_t m_28[8];
    uint16_t m_30;                  // 0x30
    uint8_t m_32[6];
    uint64_t m_38;                  // 0x38
    uint32_t m_bytecodeSize;        // 0x40
    int32_t m_bytecodeOffset;       // 0x44: from this field to the bytecode
};

// Sets the family and (unless cacheless) loads every shader of it (FUN_142294200).
void GxShaderSetFamily(const GXSHADERSETTINGS* settings);

// The FileDataID of one of the engine's compute shaders (FUN_142294110).
int32_t GxShaderComputeId(int32_t index);

// The FileDataIDs of the family's vertex, geometry and pixel shaders by index (FUN_1422941d0,
// FUN_142294140, FUN_142294170).
int32_t GxShaderVertexId(int32_t index);
int32_t GxShaderGeometryId(int32_t index);
int32_t GxShaderPixelId(int32_t index);

// A shader's permutation, loaded when needed and unpacked 16-byte aligned into `buffer`; null
// when it cannot be (FUN_142293cb0).
const GXSHADERPERMUTE* GxShaderGetPermute(int32_t id, uint16_t permute, GXSHADERBUFFER* buffer);

// Formats a shader id for messages (FUN_14230d6e0).
const char* GxShaderName(int32_t id, char* buffer, uint32_t size);

// Starts and stops the shader compiler service the family uses (FUN_1422952c0, FUN_142295310).
void GxShaderServiceStart();
void GxShaderServiceStop();

#endif
