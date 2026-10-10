#include "model2/M2Ribbon.hpp"

#include "util/Unimplemented.hpp"

// Engine/Source/Model2 ribbons (0x141296530..0x1412979xx) over the GpuStripEmitter
// (Engine/Source/Services/GpuStripEmitter.cpp). Not ported yet: nothing makes a ribbon until the
// model's emitters are created in InitializeLoaded.

// ref: FUN_141296640
void RibbonEmitterDelete(CRibbonEmitter* ribbon) {
    (void)ribbon;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412966b0
void RibbonEmitterSetParentMatrix(CRibbonEmitter* ribbon, const C44Matrix* matrix) {
    (void)ribbon;
    (void)matrix;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141296bc0
void RibbonEmitterSetAlpha(CRibbonEmitter* ribbon, float alpha) {
    (void)ribbon;
    (void)alpha;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141296be0
void RibbonEmitterSetColor(CRibbonEmitter* ribbon, float r, float g, float b) {
    (void)ribbon;
    (void)r;
    (void)g;
    (void)b;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141296cc0
void RibbonEmitterSetVisible(CRibbonEmitter* ribbon, bool visible) {
    (void)ribbon;
    (void)visible;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412970b0
void RibbonEmitterSetTextureTransform(CRibbonEmitter* ribbon, uint32_t layer, const C4Quaternion* rotation,
    const C3Vector* scale, const C3Vector* translation) {
    (void)ribbon;
    (void)layer;
    (void)rotation;
    (void)scale;
    (void)translation;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141296df0
void RibbonEmitterSetMatrix(CRibbonEmitter* ribbon, const float* matrix, const void* a3) {
    (void)ribbon;
    (void)matrix;
    (void)a3;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141297490
void RibbonEmitterUpdate(CRibbonEmitter* ribbon, float delta, bool hidden) {
    (void)ribbon;
    (void)delta;
    (void)hidden;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141296530
CRibbonEmitter* RibbonEmitterConstruct(void* memory) {
    (void)memory;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_1412966c0
void RibbonEmitterInitialize(CRibbonEmitter* ribbon, uint32_t material, const uint32_t* color, CTexture** textures, const uint32_t* blend, const uint32_t* types) {
    (void)ribbon;
    (void)material;
    (void)color;
    (void)textures;
    (void)blend;
    (void)types;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141296cf0
void RibbonEmitterReset(CRibbonEmitter* ribbon) {
    (void)ribbon;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141296f20
void RibbonEmitterSetQuality(CRibbonEmitter* ribbon, uint32_t quality) {
    (void)ribbon;
    (void)quality;
    FROST_UNIMPLEMENTED();
}

uint16_t RibbonEmitterGetQuality(const CRibbonEmitter* ribbon) {
    (void)ribbon;
    FROST_UNIMPLEMENTED();
    return 0;
}

// ref: FUN_141297050
void RibbonEmitterSetTexture(CRibbonEmitter* ribbon, uint32_t layer, CTexture* texture) {
    (void)ribbon;
    (void)layer;
    (void)texture;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412970f0
void RibbonEmitterSetEnabled(CRibbonEmitter* ribbon, bool enabled) {
    (void)ribbon;
    (void)enabled;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141296f00
void RibbonEmitterSetColorOverride(CRibbonEmitter* ribbon, float r, float g, float b) {
    (void)ribbon;
    (void)r;
    (void)g;
    (void)b;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141296840
bool RibbonEmitterIsIdle(const CRibbonEmitter* ribbon) {
    auto bytes = reinterpret_cast<const uint8_t*>(ribbon);
    return (bytes[0x1a2] & 4) != 0
        && *reinterpret_cast<const int32_t*>(bytes + 0x68) - *reinterpret_cast<const int32_t*>(bytes + 0x70)
            == *reinterpret_cast<const int32_t*>(bytes + 0xcc);
}
