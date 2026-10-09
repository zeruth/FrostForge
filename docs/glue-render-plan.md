# Glue render path

What stands between the glue's frames (created, laid out and updated since 2be195a) and
pixels on screen, in the order it is being ported. Each step names the reference function
it starts from.

## Gx draw path (D3D12 render context, vtable 0x14291a188)

Each context slot is a thunk into GraphicsCommandList (context at +0x1d80).

| step | what | reference |
|---|---|---|
| G1 | transient GxBufs: create (slot 0x200), lock (0x180), unlock (0x188) | FUN_142290c90, FUN_1422cd800, FUN_1422cd9f0 |
| G2 | vertex streams (slot 0x128) and their formats | FUN_14228f830, FUN_1422dfe60 |
| G3 | index buffer (slot 0x130) | FUN_1422dca20 |
| G4 | pipelines: desc -> GxPipeline (PSO, root signature), bind (slot 0x88) | FUN_14228b580, FUN_14228b3b0, FUN_1422dc9e0 |
| G5 | texture and sampler binding (slot 0x30) | FUN_1422e43f0, FUN_1422c9640 |
| G6 | draws (slots 0x148 / 0x150) with the state commit | FUN_1422d1120, FUN_1422d11f0, FUN_1422d9140, FUN_1422d5460 |

## Textures

CTexture / TextureCreate and the BLP decode, uploaded through the existing Tex.cpp.

## UI

| step | what | reference |
|---|---|---|
| U1 | CSimpleTexture and its renderables (CSimpleRenderable, 0x90) | |
| U2 | CRenderBatch build and render | FUN_14061fe80, FUN_14061f160, FUN_140620e20 |
| U3 | CSimpleTop render (slot 0xf8) | FUN_1405f09a0 |
| U4 | font strings and GxuFont | |
| U5 | widgets: Button, CheckButton, EditBox, Slider, ScrollFrame, StatusBar | |
| U6 | ModelFFX / M2 and particles | |
| U7 | mouse dispatch | FUN_1405f1530, FUN_1405ed600 |
| U8 | glue Lua API, C_Timer, sound, the settings panels | |
