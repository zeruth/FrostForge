#include "gx/Gx.hpp"
#include "gx/GxRenderTargets.hpp"
#include "gx/d3d12/DeviceD3d12.hpp"
#include "util/Unimplemented.hpp"
#include <cstring>

// (DAT_14348b658 in the reference. Not shared with translated code yet: the forwarded per-frame
// functions that draw through it whited out the screen when it was, 2026-10-12.)
GxRenderContext* g_gxRenderContext;

// ref: FUN_14228c430
gxDx12Backend::TargetView* GxTexTargetView(gxDx12Backend::Tex* tex) {
    if (tex == nullptr) {
        return nullptr;
    }
    return static_cast<gxDx12Backend::TargetView*>(static_cast<CGxDevice*>(g_gxDevice)->TexTargetView(tex));
}

// ---- GXRENDERTARGETS ---------------------------------------------------------------------

// ref: FUN_14228dec0
void GxRenderTargetsClear(GXRENDERTARGETS* targets) {
    std::memset(targets, 0, sizeof(GXRENDERTARGETS));
}

// ref: FUN_14228dbf0
bool GxRenderTargetsUpdate(GXRENDERTARGETS* cached, const GXRENDERTARGETS* targets) {
    if (cached->m_mask != targets->m_mask) {
        *cached = *targets;
        return false;
    }
    uint32_t first = (~(targets->m_mask >> 4)) & 1;
    uint32_t end = first + static_cast<uint8_t>(targets->m_count);
    if (end > 5) {
        end = 5;
    }
    if (first >= end) {
        return true;
    }
    uint32_t changed = 0;
    for (uint32_t i = first; i < end; i++) {
        changed |= cached->m_ids[i] ^ targets->m_ids[i];
        cached->m_ids[i] = targets->m_ids[i];
    }
    if (changed != 0) {
        std::memcpy(&cached->m_views[first], &targets->m_views[first], static_cast<size_t>(targets->m_count) * 8);
        std::memcpy(&cached->m_texs[first], &targets->m_texs[first], static_cast<size_t>(targets->m_count) * 8);
    }
    return changed == 0;
}

// ref: FUN_14228ddb0
int32_t* GxRenderTargetsSize(const GXRENDERTARGETS* targets, int32_t* size) {
    const gxDx12Backend::Tex* tex = targets->m_texs[1];
    uint8_t mip;
    if (tex != nullptr) {
        mip = targets->m_views[1]->m_mip;
    } else {
        tex = targets->m_texs[0];
        if (tex == nullptr) {
            size[0] = 0;
            size[1] = 0;
            return size;
        }
        mip = targets->m_views[0]->m_mip;
    }
    int32_t width = static_cast<int32_t>(tex->m_width) >> (mip & 0x1f);
    int32_t height = static_cast<int32_t>(tex->m_height) >> (mip & 0x1f);
    size[0] = width < 2 ? 1 : width;
    size[1] = height < 2 ? 1 : height;
    return size;
}

// Puts a target into a set and updates its bit in the mask; depth sets bit 4, and bit 5 too
// unless its format (0x1d, 0x1e) has no stencil. The count is left for GxRenderTargetsCount.
// ref: FUN_14228da90
void GxRenderTargetsAttach(GXRENDERTARGETS* targets, uint8_t index, gxDx12Backend::Tex* tex) {
    gxDx12Backend::TargetView* view = GxTexTargetView(tex);
    targets->m_count = 0;
    targets->m_texs[index] = tex;
    targets->m_views[index] = view;
    targets->m_ids[index] = view != nullptr ? view->m_id : 0;
    if (index == 0) {
        uint16_t mask = targets->m_mask;
        if (tex == nullptr) {
            targets->m_mask = mask & 0xffcf;
            return;
        }
        if (static_cast<uint8_t>(tex->m_format - 0x1d) > 1) {
            targets->m_mask = (mask & 0xffdf) | 0x10;
        } else {
            targets->m_mask = mask | 0x30;
        }
        return;
    }
    if (tex == nullptr) {
        targets->m_mask &= static_cast<uint16_t>(~(1u << ((index - 1) & 0x1f)));
    } else {
        targets->m_mask |= static_cast<uint16_t>(1u << ((index - 1) & 0x1f));
    }
}

// Counts the targets the mask names (depth and stencil as one); true when there are any.
// ref: FUN_14228dd30
bool GxRenderTargetsCount(GXRENDERTARGETS* targets) {
    uint16_t mask = targets->m_mask;
    uint16_t count = static_cast<uint16_t>(((mask & 0x30) != 0) + ((mask >> 3) & 1) + ((mask >> 2) & 1) + ((mask >> 1) & 1) +
        (mask & 1));
    targets->m_count = count;
    return count != 0;
}

// ref: FUN_14228de80
void GxRenderTargetsSet(GXRENDERTARGETS* targets, uint32_t index, gxDx12Backend::Tex* tex) {
    targets->m_texs[index] = tex;
    gxDx12Backend::TargetView* view = GxTexTargetView(tex);
    targets->m_views[index] = view;
    targets->m_ids[index] = view->m_id;
}

// ref: FUN_14228e2d0
void GxRenderContextSetTargetConstants(GxRenderContext* context, const GXRENDERTARGETS* targets) {
    int32_t size[2];
    GxRenderTargetsSize(targets, size);
    float* constants = context->LockConstants(2, 6, 0x20);
    float invWidth = 1.0f / static_cast<float>(size[0]);
    float invHeight = 1.0f / static_cast<float>(size[1]);
    constants[4] = invWidth;
    constants[5] = invHeight;
    constants[6] = 0.0f;
    constants[7] = 0.0f;
    constants[0] = static_cast<float>(size[0]);
    constants[1] = static_cast<float>(size[1]);
    constants[2] = invWidth;
    constants[3] = invHeight;
    context->UnlockConstants(2, 6);
}

// ref: FUN_141344390
float* GxColorToFloat4(float* out, uint32_t color) {
    const float scale = 1.0f / 255.0f;
    out[0] = static_cast<float>((color >> 16) & 0xff) * scale;
    out[1] = static_cast<float>((color >> 8) & 0xff) * scale;
    out[2] = static_cast<float>(color & 0xff) * scale;
    out[3] = static_cast<float>(color >> 24) * scale;
    return out;
}

// ---- GxRenderContext ---------------------------------------------------------------------

// ref: FUN_14228f2a0
GxRenderContext::GxRenderContext() {
    std::memset(this->m_state, 0, sizeof(this->m_state));
    this->Field<uint64_t>(0x34) = 0x3f800000;
    this->Field<uint64_t>(0x48) = 0x3f800000;
    this->Field<uint64_t>(0x5c) = 0x3f800000;
    this->Field<uint32_t>(0x70) = 0x3f800000;
    this->Field<uint8_t>(0x18) = 1;
    this->Field<uint8_t>(0x74) = 1;
    this->Field<uint32_t>(0x1f8) = 0;
    this->Field<uint32_t>(0x1c) = 0;
    this->Field<uint64_t>(0x20) = 0x3f800000;
    this->Field<uint64_t>(0x28) = 0x3f800000;
    this->Field<uint32_t>(0x30) = 0x3f800000;
    this->Field<uint64_t>(0x78) = 0;
    this->Field<uint32_t>(0x80) = 0x3f800000;
    this->Field<uint32_t>(0x84) = 0x3f800000;
    // Two arrays of four identity matrices (the constructor at LAB_140225a40), at +0xf8 and +0x1fc.
    for (uint32_t i = 0; i < 4; i++) {
        for (size_t base : { static_cast<size_t>(0xf8), static_cast<size_t>(0x1fc) }) {
            float* matrix = &this->Field<float>(base + i * 0x40);
            matrix[0] = 1.0f;
            matrix[5] = 1.0f;
            matrix[10] = 1.0f;
            matrix[15] = 1.0f;
        }
    }
}

// ref: FUN_14228f3e0
GxRenderContext::~GxRenderContext() {
}

// ref: FUN_14228f680
void GxRenderContext::SetViewport(const float* viewport) {
    float* current = &this->Field<float>(8);
    if (viewport[0] != current[0] || viewport[1] != current[1] || viewport[2] != current[2] || viewport[3] != current[3]) {
        this->Field<uint8_t>(0x18) = 1;
        this->Field<uint8_t>(0x74) = 1;
        std::memcpy(current, viewport, 0x10);
    }
}

// ref: FUN_14228fa20
void GxRenderContext::Slot018(uint8_t stage, uint32_t first, uint32_t count, void** textures) {
    for (uint32_t i = 0; i < count; i++) {
        this->BindTexture(stage, i + first, static_cast<gxDx12Backend::Tex*>(textures[i]), 0);
    }
}

// ref: FUN_14228fb50
void GxRenderContext::GetDepthRange(float* a2, float* a3, float* a4, float* a5, float* a6, float* a7) {
    *a6 = 0.0f;
    *a4 = 0.0f;
    *a2 = 0.0f;
    *a7 = 1.0f;
    *a5 = 1.0f;
    *a3 = 1.0f;
}

// The slots the D3D12 context does not override are _guard_check_icall (empty) or _purecall.
void GxRenderContext::Slot008() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::BindTexture(uint8_t, uint32_t, gxDx12Backend::Tex*, uint8_t) {
    FROST_UNIMPLEMENTED();  // FUN_1422e3fd0 -> FUN_1422e3d20: binds a texture to a stage
}
void GxRenderContext::Slot020(uint32_t, void*) {
}
void GxRenderContext::SetSampler(uint8_t, uint32_t, uint64_t) {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::SetTexture(uint8_t, uint32_t, gxDx12Backend::Tex*) {
    FROST_UNIMPLEMENTED();
}
// Each of the three stages: its texture slots, then its remaining unordered view slots, as the
// device's capabilities count them (+0x70 and +0x60 per stage).
// ref: FUN_14228fa90
void GxRenderContext::UnbindTextures() {
    for (uint32_t stage = 0; stage < 3; stage++) {
        uint32_t slot = 0;
        while (slot < *reinterpret_cast<uint32_t*>(GxDeviceCaps() + 0x70 + stage * 4)) {
            this->SetTexture(static_cast<uint8_t>(stage), slot, nullptr);
            slot++;
        }
        while (slot < *reinterpret_cast<uint32_t*>(GxDeviceCaps() + 0x60 + stage * 4)) {
            this->BindTexture(static_cast<uint8_t>(stage), slot, nullptr, 0);
            slot++;
        }
    }
}

// ref: FUN_14228f790
void GxRenderContext::GetScissor(float* rect) {
    std::memcpy(rect, &this->Field<float>(0x78), 0x10);
}

// ref: FUN_14228f960
void GxRenderContext::SetScissor(const float* rect) {
    float* current = &this->Field<float>(0x78);
    if (rect[0] != current[0] || rect[1] != current[1] || rect[2] != current[2] || rect[3] != current[3]) {
        this->Field<uint8_t>(0x74) = 1;
        std::memcpy(current, rect, 0x10);
    }
}
void GxRenderContext::Slot040() {
}
void GxRenderContext::Slot048() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot050() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot058() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot060() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot068() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot070() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot078() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot080() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::SetPipeline(GxPipeline*) {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::SetRenderTargets(GXRENDERTARGETS*) {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::ClearAndSetTargets(const GXRENDERTARGETSCLEAR*) {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot0a0() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Clear(GXRENDERTARGETS*, uint16_t, uint32_t, float, uint32_t, uint8_t) {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot0b0() {
}
void GxRenderContext::Slot0b8() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot0c0() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot0c8() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot0d0() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot0d8() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot0e0() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot0e8() {
}
void GxRenderContext::Slot0f0() {
}
void GxRenderContext::Slot0f8() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot100() {
}
void GxRenderContext::Slot108() {
}
void GxRenderContext::Slot110() {
}
void GxRenderContext::Slot118() {
}
void GxRenderContext::SetVertexBuffers(uint32_t, uint32_t, const GXVERTEXSTREAM*) {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::SetIndexBuffer(GxBuf*, uint8_t) {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot138() {
}
void GxRenderContext::SetShadingRateCombiners(const uint8_t*) {
}
void GxRenderContext::Draw(uint32_t, uint32_t, uint32_t, uint32_t, uint8_t) {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::DrawIndexed(uint32_t, uint32_t, uint32_t, int32_t, uint32_t, uint8_t) {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot158() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot160() {
    FROST_UNIMPLEMENTED();
}
float* GxRenderContext::LockConstants(uint8_t, uint32_t, uint32_t) {
    FROST_UNIMPLEMENTED();
    return nullptr;
}
void GxRenderContext::UnlockConstants(uint8_t, uint32_t) {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot178() {
    FROST_UNIMPLEMENTED();
}
void* GxRenderContext::BufLock(GxBuf*) {
    FROST_UNIMPLEMENTED();
    return nullptr;
}
bool GxRenderContext::BufUnlock(GxBuf*, uint32_t) {
    FROST_UNIMPLEMENTED();
    return false;
}
void GxRenderContext::Slot190() {
    FROST_UNIMPLEMENTED();
}
bool GxRenderContext::CopyTex(gxDx12Backend::Tex*, gxDx12Backend::Tex*, int32_t*, int32_t*, uint32_t*, uint32_t, int32_t) {
    FROST_UNIMPLEMENTED();
    return false;
}
void GxRenderContext::Slot1a0() {
    FROST_UNIMPLEMENTED();
}
bool GxRenderContext::ResolveTex(gxDx12Backend::Tex*, gxDx12Backend::Tex*) {
    FROST_UNIMPLEMENTED();
    return false;
}
bool GxRenderContext::UpdateTex(gxDx12Backend::Tex*, const int32_t*, bool, GXTEXUPDATEPROC, void*) {
    FROST_UNIMPLEMENTED();
    return false;
}
void GxRenderContext::Slot1b8() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot1c0() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot1c8() {
}
void GxRenderContext::Slot1d0() {
}
void GxRenderContext::Slot1d8() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot1e0() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot1e8() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot1f0() {
    FROST_UNIMPLEMENTED();
}
void GxRenderContext::Slot1f8() {
    FROST_UNIMPLEMENTED();
}
GxBuf* GxRenderContext::GetUploadBuf(uint8_t) {
    FROST_UNIMPLEMENTED();
    return nullptr;
}
