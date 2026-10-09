#ifndef SHADEREFFECT_SHADER_EFFECT_MANAGER_HPP
#define SHADEREFFECT_SHADER_EFFECT_MANAGER_HPP

// Engine/Source/ShaderEffect/ShaderEffectManager.cpp: the named shader effects the models
// draw with (a vertex and a pixel shader, and the pipelines built for them), made when the
// device comes up, and the frame-start merge of every transient pipeline map.

#include <cstdint>

#include "blz/ChainedHash.hpp"
#include "os/Sync.hpp"

// CShaderEffect (0x60 bytes).
class CShaderEffect {
    public:
    uint64_t m_nameHash;                        // 0x00: FNV-1a 64 of the name
    blz::chained_hash_header m_pipelines;       // 0x08
    blz::chained_hash_header m_newPipelines;    // 0x28: built since the last frame start
    OSMUTEX m_lock;                             // 0x48
    uint8_t m_50;                               // 0x50
    uint8_t m_pad51[3];
    uint32_t m_vertexShader;                    // 0x54
    uint32_t m_pixelShader;                     // 0x58
    uint32_t m_pad5c;

    CShaderEffect();
    // ref: FUN_1412ab6a0
    ~CShaderEffect();

    // ref: FUN_1412ad1b0
    bool Initialize(uint32_t vertexShader, uint32_t pixelShader);
    // Destroys the pipelines in both maps (FUN_1412ac4a0).
    void ReleasePipelines();
};
static_assert(sizeof(CShaderEffect) == 0x60, "CShaderEffect is 0x60 bytes");

// The effect of a name, made with its shaders when missing (FUN_1412a30e0); null when the effect
// could not be set up.
CShaderEffect* ShaderEffectCreate(const char* name, uint32_t vertexShader, uint32_t pixelShader);
// The effect of a name, or null (FUN_1412a38a0).
CShaderEffect* ShaderEffectFind(const char* name);
// Destroys an effect and clears the pointer (FUN_1412a3680).
void ShaderEffectDestroy(CShaderEffect** effect);

// Registers the device and frame callbacks (FUN_1412a3990) and removes them, destroying the
// effects (FUN_1412a3640).
void ShaderEffectManagerInitialize();
void ShaderEffectManagerShutdown();

#endif
