#ifndef WORLD_WORLD_SCENE_RENDER_HPP
#define WORLD_WORLD_SCENE_RENDER_HPP

// WoW/Source/WorldClient2/WorldSceneRender.cpp: CWorldSceneRender (0x1410 bytes), what a view
// draws through: its frame buffers and borrowed targets, a shader effect context (0x9b0), a
// thread pool group for its jobs, the 25 buffers it streams geometry through and three
// projection slots.

#include "client/FrameBufferCache.hpp"
#include <cstddef>

class CWorldScene;
class CWorldView;
struct WORLDRENDERPARAMS;
#include <cstdint>

class ShaderEffectContext;
class GxRenderContext;
struct GXRENDERTARGETS;
struct THREADPOOLGROUP;

// The pass state cleared by FUN_1413ce660 (0x108).
struct WORLDRENDERPASS {
    uint8_t m_00[0xc];
    uint8_t m_0c[0x30];             // 0x0c
    float m_3c[2];                  // 0x3c 1, 0
    uint8_t m_44[0xc];              // 0x44
    float m_50[2];                  // 0x50 1, 0
    uint8_t m_58[0xc];              // 0x58
    float m_64[2];                  // 0x64 1, 0
    uint8_t m_6c[0xc];              // 0x6c
    float m_78;                     // 0x78 1
    float m_7c[2];                  // 0x7c 1, 0
    uint8_t m_84[0xc];              // 0x84
    float m_90[2];                  // 0x90 1, 0
    uint8_t m_98[0xc];              // 0x98
    float m_a4[2];                  // 0xa4 1, 0
    uint8_t m_ac[0xc];              // 0xac
    float m_b8;                     // 0xb8 1
    uint8_t m_bc[0x14];
    uint64_t m_d0[7];               // 0xd0
};
static_assert(sizeof(WORLDRENDERPASS) == 0x108, "WORLDRENDERPASS is 0x108 bytes");

// A projection slot (0xc0): a count of up to 25, two matrices and a rotation.
struct WORLDRENDERPROJECTION {
    void* m_00;                     // 0x00
    uint64_t m_08;                  // 0x08
    uint32_t m_10;                  // 0x10 3
    uint32_t m_14;                  // 0x14 25
    float m_matrix[16];             // 0x18 identity
    float m_matrix2[16];            // 0x58 identity
    uint32_t m_98;                  // 0x98
    uint32_t m_9c;                  // 0x9c
    float m_rotation[4];            // 0xa0 (0, 0, 0, 1)
    uint32_t m_b0;                  // 0xb0
    uint32_t m_b4;                  // 0xb4 3
    uint16_t m_b8;                  // 0xb8
    uint8_t m_ba;                   // 0xba
    uint8_t m_padbb[5];
};
static_assert(sizeof(WORLDRENDERPROJECTION) == 0xc0, "WORLDRENDERPROJECTION is 0xc0 bytes");

// Two frame buffers and a borrowed target a pass resolves through (0x250); the device's loss
// clears their textures (FUN_1412bc9b0).
struct WORLDRENDERRESOLVE {
    uint64_t m_00;                          // 0x000
    FRAMEBUFFER m_buffer;                   // 0x008
    FRAMEBUFFERTEMP m_temp;                 // 0x0f8
    uint64_t m_130;                         // 0x130
    uint64_t m_138;                         // 0x138
    FRAMEBUFFER m_buffer2;                  // 0x140
    uint64_t m_230[4];                      // 0x230

    // ref: FUN_1412bc9b0
    void OnDeviceDestroyed();
    // Its buffers and borrowed target handed back.
    // ref: FUN_1412b8160
    void Release();
};
static_assert(sizeof(WORLDRENDERRESOLVE) == 0x250, "WORLDRENDERRESOLVE is 0x250 bytes");

class CWorldSceneRender {
    public:
    enum : uint32_t {
        BUFFER_COUNT = 25,
    };

    float m_matrix[16];                     // 0x000 identity
    uint64_t m_040[4];                      // 0x040
    uint8_t m_060[0x10];
    FRAMEBUFFER m_buffer070;                // 0x070
    FRAMEBUFFER m_buffer160;                // 0x160
    uint8_t m_250[0x60];
    FRAMEBUFFERTEMP m_temp2b0;              // 0x2b0
    FRAMEBUFFER m_buffer2e8;                // 0x2e8
    uint8_t m_3d8[0x10];
    uint64_t m_3e8;                         // 0x3e8
    FRAMEBUFFER m_buffers3f0[5];            // 0x3f0
    WORLDRENDERPASS m_pass;                 // 0x8a0
    uint64_t m_9a8;                         // 0x9a8
    uint64_t m_9b0;                         // 0x9b0
    uint64_t m_9b8;                         // 0x9b8
    ShaderEffectContext* m_effectContext;   // 0x9c0
    uint8_t m_9c8[0xb];
    uint8_t m_9d3;                          // 0x9d3 handed back to the frame (FUN_1413c5ce0)
    uint8_t m_9d4;                          // 0x9d4
    uint8_t m_pad9d5[3];
    FRAMEBUFFERTEMP m_temp9d8;              // 0x9d8
    FRAMEBUFFER m_bufferA10;                // 0xa10
    uint8_t m_b00;                          // 0xb00
    uint8_t m_padb01[7];
    uint64_t m_b08;                         // 0xb08
    uint64_t m_b10;                         // 0xb10
    uint8_t m_b18;                          // 0xb18
    uint8_t m_padb19[7];
    FRAMEBUFFERTEMP m_tempsB20[4];          // 0xb20
    FRAMEBUFFER m_bufferC00;                // 0xc00
    WORLDRENDERRESOLVE m_resolve;           // 0xcf0
    uint64_t m_f40;                         // 0xf40
    uint64_t m_f48;
    uint8_t m_f50[0xc8];                    // 0xf50
    THREADPOOLGROUP* m_group;               // 0x1018
    void* m_buffers[BUFFER_COUNT];          // 0x1020 device objects (released through slot 0x98)
    ShaderEffectContext* m_contexts[BUFFER_COUNT];  // 0x10e8 one per buffer, made as needed
    WORLDRENDERPROJECTION m_projections[3]; // 0x11b0
    uint64_t m_13f0;                        // 0x13f0
    uint64_t m_13f8;                        // 0x13f8
    float m_1400;                           // 0x1400 0.7519
    uint32_t m_1404;
    uint64_t m_1408;                        // 0x1408

    // ref: FUN_1413cdca0
    CWorldSceneRender();
    // ref: FUN_1413ce740
    ~CWorldSceneRender();
    // The streamed buffers and their contexts, released (FUN_1413cea70).
    // ref: FUN_1413cea70
    void ReleaseBuffers();
    // The frame's scene, view, map, M2 scene, buffers and flags, and its camera matrix.
    // ref: FUN_1413d8ac0
    void Begin(CWorldScene* scene, CWorldView* view, WORLDRENDERPARAMS* params);
    // The frame's passes.
    // (forwards to FUN_1413cf390's translation)
    void Draw();
    // ref: FUN_1413d6ec0
    void SetEnvironmentConstants(GxRenderContext* context, ShaderEffectContext* effect, bool farFog);
    // ref: FUN_1413d6d00
    void ClearFrame(GXRENDERTARGETS* targets, bool clearDepth);
    // A pass's deferred command list, or null when the pass draws directly.
    // ref: FUN_1413cecc0
    void* GetCommandList(uint32_t slot);
    // ref: FUN_1413cee80
    bool UsesCommandLists(uint32_t slot);
    // (forwards to FUN_1413d47c0's translation)
    void DrawOpaque(FRAMEBUFFER* buffer);
    // (forwards to FUN_1413d07b0's translation)
    void DrawTransparent(GXRENDERTARGETS* targets, GXRENDERTARGETS* targets2);
    // ref: FUN_1413d4070
    void DrawFrontPass(GxRenderContext* context, ShaderEffectContext* effect);
    // ref: FUN_1413d8700
    void DrawProjectedPass(GxRenderContext* context, ShaderEffectContext* effect, bool second);
    // ref: FUN_1413d2380
    void DrawAlphaM2(GxRenderContext* context, ShaderEffectContext* effect);
    // ref: FUN_1413cf2c0
    bool PrepareParticulates();
    // The world scene's ray-traced shadow targets (FUN_1413ceb50) and its ray tracing jobs
    // (FUN_1413d8c80).
    // (forwards to FUN_1413ceb50's translation)
    void SetupRayTracing(bool rayTracing, FRAMEBUFFERTEMP* temp, void** objects);
    // (forwards to FUN_1413d8c80's translation)
    void DispatchRayTracing(bool rayTracing);
};
static_assert(offsetof(CWorldSceneRender, m_buffer070) == 0x70, "CWorldSceneRender layout");
static_assert(offsetof(CWorldSceneRender, m_temp2b0) == 0x2b0, "CWorldSceneRender layout");
static_assert(offsetof(CWorldSceneRender, m_buffers3f0) == 0x3f0, "CWorldSceneRender layout");
static_assert(offsetof(CWorldSceneRender, m_pass) == 0x8a0, "CWorldSceneRender layout");
static_assert(offsetof(CWorldSceneRender, m_effectContext) == 0x9c0, "CWorldSceneRender layout");
static_assert(offsetof(CWorldSceneRender, m_temp9d8) == 0x9d8, "CWorldSceneRender layout");
static_assert(offsetof(CWorldSceneRender, m_tempsB20) == 0xb20, "CWorldSceneRender layout");
static_assert(offsetof(CWorldSceneRender, m_resolve) == 0xcf0, "CWorldSceneRender layout");
static_assert(offsetof(CWorldSceneRender, m_f40) == 0xf40, "CWorldSceneRender layout");
static_assert(offsetof(CWorldSceneRender, m_group) == 0x1018, "CWorldSceneRender layout");
static_assert(offsetof(CWorldSceneRender, m_projections) == 0x11b0, "CWorldSceneRender layout");
static_assert(sizeof(CWorldSceneRender) == 0x1410, "CWorldSceneRender is 0x1410 bytes");

#endif
