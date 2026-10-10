#include "model2/M2Shaders.hpp"

#include "shadereffect/ShaderEffectManager.hpp"

#include "storm/ErrorReport.hpp"

namespace {

// The pixel shaders (DAT_142cfb3d0) and vertex shaders (DAT_142cfb470) the registered shader
// table holds for models.
const uint32_t s_pixelShaders[37] = {
    43, 27, 49, 50, 51, 59, 37, 38, 28, 39, 33, 42, 52, 44, 45, 54, 30, 31, 48,
    53, 46, 29, 58, 32, 57, 56, 35, 55, 36, 47, 132, 133, 134, 34, 137, 41, 40,
};
const uint32_t s_vertexShaders[19] = {
    46, 40, 52, 47, 42, 41, 48, 50, 51, 38, 55, 49, 39, 37, 53, 54, 15, 7, 8,
};

// The M2Effect shader ids (0x142cfb4c0): the pixel shader, the vertex shader kind and the
// texture coordinates of each.
struct M2EFFECT {
    int32_t m_pixel;
    int32_t m_vertex;
    int32_t m_8;
    int32_t m_c;
};
const M2EFFECT s_effects[36] = {
    { 12, 3, 1, 1 }, { 13, 3, 1, 1 }, { 14, 3, 1, 1 }, { 15, 6, 2, 2 }, { 16, 3, 1, 1 }, { 13, 7, 1, 1 },
    { 16, 7, 1, 1 }, { 17, 3, 1, 1 }, { 18, 3, 1, 1 }, { 19, 6, 2, 2 }, { 20, 7, 1, 1 }, { 21, 3, 1, 1 },
    { 22, 3, 1, 1 }, { 23, 3, 1, 1 }, { 23, 7, 1, 1 }, { 20, 2, 1, 1 }, { 24, 3, 1, 1 }, { 25, 6, 2, 2 },
    { 26, 0, 0, 0 }, { 33, 9, 0, 0 }, { 27, 11, 2, 2 }, { 6, 12, 1, 1 }, { 28, 2, 1, 1 }, { 29, 7, 1, 1 },
    { 25, 11, 2, 2 }, { 33, 13, 0, 0 }, { 30, 14, 2, 1 }, { 31, 2, 1, 2 }, { 32, 14, 2, 1 }, { 34, 7, 1, 1 },
    { 35, 15, 2, 2 }, { 35, 16, 2, 2 }, { 0, 0, 0, 0 }, { 7, 12, 1, 1 }, { 1, 9, 1, 1 }, { 36, 12, 1, 1 },
};

const M2EFFECT* Effect(uint16_t shaderId) {
    return &s_effects[shaderId & 0x7fff];
}

}

// The pixel shader kind a shader id draws with (FUN_1411fb3f0).
// ref: FUN_1411fb3f0
uint32_t M2GetPixelShaderKind(int32_t textureCount, uint16_t shaderId) {
    if (static_cast<int16_t>(shaderId) < 0) {
        return static_cast<uint32_t>(Effect(shaderId)->m_pixel);
    }
    if (textureCount == 1) {
        return (shaderId & 0x70) != 0 ? 1 : 0;
    }
    if ((shaderId & 0x70) == 0) {
        switch (shaderId & 7) {
        case 0:
            return 5;
        case 3:
        case 7:
            return 0xd;
        case 4:
            return 3;
        case 6:
            return 4;
        default:
            return 2;
        }
    }
    switch (shaderId & 7) {
    case 0:
        return 0xb;
    case 3:
        return 8;
    case 4:
        return 7;
    case 6:
        return 9;
    case 7:
        return 10;
    default:
        return 6;
    }
}

const char* const g_m2ProjectedEffects[10] = {
    "Projected_Unlit_Opaque_Single_Texture",
    "Projected_Lit_Opaque_Single_Texture",
    "Projected_Unlit_Two_Texture",
    "Projected_Lit_Two_Texture",
    "Projected_Unlit_Three_Texture",
    "Projected_Lit_Three_Texture",
    "",
    "",
    "Projected_Unlit_Add_Single_Texture",
    "Projected_Lit_Add_Single_Texture",
};

const uint8_t g_m2ProjectedBlendLit[8] = { 1, 1, 1, 1, 1, 0, 0, 1 };

// ref: FUN_1411fb670
uint32_t M2GetVertexShaderKind(uint16_t textureCount, uint16_t shaderId, uint16_t* texCoords) {
    *texCoords = 0;
    if (static_cast<int16_t>(shaderId) < 0) {
        uint32_t kind = static_cast<uint32_t>(Effect(shaderId)->m_vertex);
        switch (kind) {
            case 1:
            case 4:
            case 0xd:
                *texCoords = 1;
                break;
            case 3:
            case 6:
            case 0xb:
                *texCoords = 2;
                break;
            case 5:
                *texCoords = 3;
                break;
            default:
                break;
        }
        return kind;
    }
    if (textureCount == 1) {
        if (static_cast<int8_t>(shaderId) < 0) {
            *texCoords = 1;
            return 1;
        }
        return (shaderId & 0x4000) != 0 ? 10 : 0;
    }
    if (0 <= static_cast<int8_t>(shaderId)) {
        if (shaderId & 8) {
            *texCoords = 2;
            return 3;
        }
        return ((shaderId >> 14) & 1) != 0 ? 2 : 7;
    }
    if (shaderId & 8) {
        *texCoords = 3;
        return 5;
    }
    *texCoords = 1;
    return 4;
}

// ref: FUN_1411fb770
uint32_t M2GetVertexShaderId(uint16_t textureCount, uint16_t shaderId) {
    uint16_t texCoords;
    return s_vertexShaders[M2GetVertexShaderKind(textureCount, shaderId, &texCoords)];
}

// ref: FUN_1411fb4e0
uint32_t M2GetPixelShaderId(uint16_t textureCount, uint16_t shaderId) {
    if (static_cast<int16_t>(shaderId) < 0) {
        return s_pixelShaders[Effect(shaderId)->m_pixel];
    }
    if (textureCount == 1) {
        return s_pixelShaders[(shaderId & 0x70) != 0 ? 1 : 0];
    }
    if ((shaderId & 0x70) == 0) {
        switch (shaderId & 7) {
            case 0:
                return s_pixelShaders[5];
            case 3:
            case 7:
                return s_pixelShaders[13];
            case 4:
                return s_pixelShaders[3];
            case 6:
                return s_pixelShaders[4];
            default:
                return s_pixelShaders[2];
        }
    }
    switch (shaderId & 7) {
        case 0:
            return s_pixelShaders[11];
        case 3:
            return s_pixelShaders[8];
        case 4:
            return s_pixelShaders[7];
        case 6:
            return s_pixelShaders[9];
        case 7:
            return s_pixelShaders[10];
        default:
            return s_pixelShaders[6];
    }
}

// ref: FUN_1411a5a40
CShaderEffect* M2FallbackEffect() {
    CShaderEffect* effect = ShaderEffectFind("Diffuse_T1Combiners_Opaque");
    if (effect == nullptr) {
        effect = ShaderEffectCreate("Diffuse_T1Combiners_Opaque", 0x2e, 0x2b);
        if (effect == nullptr) {
            // FUN_14024d330
            ErrorDisplayFormatted(0x85100086, nullptr, 0, "Can not create fallback shader");
        }
    }
    return effect;
}
