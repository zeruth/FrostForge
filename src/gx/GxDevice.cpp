#include "gx/GxDevice.hpp"
#include "gx/GxVertexFormat.hpp"
#include "gx/GxPlatform.hpp"
#include "prism/Instance.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Unimplemented.hpp"
#include <windows.h>
#include <cstring>

namespace {

const char s_vertexFormatMapTag[] =
    ".?AU?$pair@$$CB_KV?$unique_ptr@UpmVertexBufferFormat@@Uvbf_deleter@GxDevice@@@blz@@@blz@@";

}

// ref: FUN_142290650
CGxDevice::CGxDevice(uint8_t api) {
    this->m_api = api;
    this->m_gpu = nullptr;
    this->m_data = nullptr;
    this->m_ready = 0;
    this->m_0a = 0;
    this->m_params.m_vsync = 1;
    this->m_params.m_cachelessShaders = 0;
    this->m_params.m_monitor = 0;
    this->m_params.m_maxFrameLatency = 3;
    this->m_params.m_aftermath = 0;
    std::memset(this->m_80, 0, 0xb0);
    Field<uint64_t>(0x130) = 0;
    Field<uint64_t>(0x138) = 1;
    Field<uint32_t>(0x140) = 0;
    Field<uint32_t>(0x144) = 1;
    Field<uint32_t>(0x148) = 0x1000000;
    Field<uint16_t>(0x14c) = 0;
    Field<uint8_t>(0x14e) = 0;
    Field<uint64_t>(0x150) = 0;
    Field<uint64_t>(0x158) = 0;
    Field<uint32_t>(0x160) = 0;
    std::memset(&Field<uint8_t>(0x168), 0, 0x2b0);
    // The vertex buffer formats made for this device (blz::unordered_map<uint64_t,
    // unique_ptr<pmVertexBufferFormat>>, 8 buckets, load 0.5).
    Field<uint64_t>(0x418) = 8;
    Field<uint64_t>(0x428) = 0;
    Field<uint64_t>(0x430) = 0;
    auto buckets = static_cast<uint8_t*>(SMemAlloc(0x80, 0xb, s_vertexFormatMapTag, SStrHashFnv1a(s_vertexFormatMapTag), -6, 0));
    Field<uint8_t*>(0x420) = buckets;
    for (uint64_t i = 0; i < Field<uint64_t>(0x418); i++) {
        *reinterpret_cast<uint64_t*>(buckets + i * 0x10) = 0;
    }
    Field<float>(0x438) = 0.75f;
    Field<uint64_t>(0x440) = 0;
    Field<uint64_t>(0x448) = 0;
    Field<uint64_t>(0x450) = 0;
    Field<uint64_t>(0x458) = 0;
    InitializeSRWLock(&Field<SRWLOCK>(0x460));
    Field<float>(0x438) = 0.5f;
    Field<uint32_t>(0x468) = 1;
    // The global vertex format table (0x2b formats at DAT_142d41d78): each format.s attributes
    // by semantic and its masks.
    GxVertexFormatsInitialize();
}

// ref: FUN_142290b40
CGxDevice::~CGxDevice() {
    FROST_UNIMPLEMENTED();  // FUN_142290980 / FUN_142290a50: the format list and map teardown
    // m_params.m_adapterName's own destructor frees its buffer.
}

// Keeps the console's device settings; an adapter name that does not contain the chosen GPU's
// name is dropped.
// ref: FUN_1422910d0
bool CGxDevice::SetParams(const GXDEVICEPARAMS* params) {
    this->m_params.m_vsync = params->m_vsync;
    this->m_params.m_cachelessShaders = params->m_cachelessShaders;
    this->m_params.m_monitor = params->m_monitor;
    this->m_params.m_maxFrameLatency = params->m_maxFrameLatency;
    this->m_params.m_aftermath = params->m_aftermath;
    if (&this->m_params.m_adapterName != &params->m_adapterName) {
        blz::string_assign(this->m_params.m_adapterName.header(), params->m_adapterName.m_data, params->m_adapterName.m_size);
    }
    const char* gpuName = Field<const char*>(0x90);
    if (std::strstr(this->m_params.m_adapterName.m_data, gpuName) == nullptr) {
        this->m_params.m_adapterName.m_data[0] = '\0';
        this->m_params.m_adapterName.m_size = 0;
    }
    return true;
}

// ref: FUN_1402ca290
uint8_t CGxDevice::Slot000() {
    return 0;
}

// The base slots: _guard_check_icall (an empty body), _purecall (FROST_UNIMPLEMENTED: every
// device overrides them) and the few with a shared body.
void CGxDevice::Slot008() {}
void CGxDevice::Slot010() {}
void CGxDevice::Slot020() {}
bool CGxDevice::DeviceCreate(const GXDEVICEPARAMS* params) {
    (void)params;
    FROST_UNIMPLEMENTED();
    return false;
}
// ref: 0x14022c3f0 (returns 0)
void CGxDevice::Slot030() {}
// ref: 0x1422910c0 (jumps to FUN_14228e4d0)
void CGxDevice::DeviceShutdown() {
    GxNotifyOnDeviceDestroy();
}
bool CGxDevice::FormatSupportsMultisample(uint8_t, uint8_t, int32_t) {
    FROST_UNIMPLEMENTED();
    return false;
}
// Finds the device's GPU again in the rebuilt adapter list (by LUID, else by vendor and
// device id) and takes its handle, description and device data.
// ref: FUN_1422928d0
bool CGxDevice::RebindAdapter() {
    GXADAPTERLIST* list = GxGetAdapterList();
    for (uint64_t i = 0; i < list->m_size; i++) {
        GxAdapterInfo* info = &list->m_data[i];
        bool match;
        if (Field<uint64_t>(0xc0) == 0) {
            if (info->m_gpu->m_deviceId != Field<uint16_t>(0xa2)) {
                continue;
            }
            match = info->m_gpu->m_vendorId == Field<uint16_t>(0xa0);
        } else {
            match = info->m_gpu->m_luid == Field<uint64_t>(0xc0);
        }
        if (match) {
            this->m_gpu = info->m_handle;
            std::memcpy(&Field<uint8_t>(0x90), info->m_gpu, 0x50);
            static const uint8_t s_prismApis[GXAPI_COUNT] = { pm::API_D3D11, pm::API_D3D11, pm::API_D3D12, 0, 0 };  // DAT_142910f88
            this->m_data = g_gxPrismInstance->GetDeviceData(info->m_handle, s_prismApis[this->m_api]);
            return true;
        }
    }
    return false;
}
uint8_t CGxDevice::Slot050() {
    return 0;
}
void CGxDevice::DeferCallback(blz::function_storage* callback) {
    (void)callback;
    FROST_UNIMPLEMENTED();
}
void CGxDevice::Slot060() {
    FROST_UNIMPLEMENTED();
}
void CGxDevice::Slot068() {}
void CGxDevice::Slot070() {}
void CGxDevice::SetLowLatencyMode(uint8_t) {
    FROST_UNIMPLEMENTED();
}
void CGxDevice::FrameMarker() {
    FROST_UNIMPLEMENTED();
}
void CGxDevice::LatencyMarker(uint32_t, uint32_t) {}
void* CGxDevice::Slot090(void* out, uint64_t a2) {
    (void)out;
    (void)a2;
    FROST_UNIMPLEMENTED();
    return nullptr;
}
void CGxDevice::Slot098() {
    FROST_UNIMPLEMENTED();
}
void CGxDevice::Slot0a0() {
    FROST_UNIMPLEMENTED();
}
void CGxDevice::Slot0a8() {
    FROST_UNIMPLEMENTED();
}
void CGxDevice::Slot0b0() {
    FROST_UNIMPLEMENTED();
}
void CGxDevice::Slot0b8() {
    FROST_UNIMPLEMENTED();
}
void CGxDevice::Slot0c0() {
    FROST_UNIMPLEMENTED();
}
void CGxDevice::Slot0c8() {
    FROST_UNIMPLEMENTED();
}
void CGxDevice::Slot0d0() {
    FROST_UNIMPLEMENTED();
}
void* CGxDevice::Slot0d8(void* out, uint64_t a2) {
    (void)out;
    (void)a2;
    FROST_UNIMPLEMENTED();
    return nullptr;
}
void CGxDevice::Slot0e0() {
    FROST_UNIMPLEMENTED();
}
void CGxDevice::Slot0e8() {
    FROST_UNIMPLEMENTED();
}
void* CGxDevice::Slot0f0(void* out) {
    (void)out;
    FROST_UNIMPLEMENTED();
    return nullptr;
}
void CGxDevice::Slot0f8() {
    FROST_UNIMPLEMENTED();
}
void* CGxDevice::SwapChainCreate(const GXSWAPCHAINPARAMS* params) {
    (void)params;
    FROST_UNIMPLEMENTED();
    return nullptr;
}
void CGxDevice::SwapChainDestroy(void* chain) {
    (void)chain;
    FROST_UNIMPLEMENTED();
}
bool CGxDevice::SwapChainBegin(void* chain) {
    (void)chain;
    FROST_UNIMPLEMENTED();
    return false;
}
void* CGxDevice::SwapChainGetBackBuffer(void* chain) {
    (void)chain;
    FROST_UNIMPLEMENTED();
    return nullptr;
}
void CGxDevice::SwapChainPresent(void* chain) {
    (void)chain;
    FROST_UNIMPLEMENTED();
}
void CGxDevice::Slot128(void* chain) {
    (void)chain;
}
void CGxDevice::SwapChainEnd(void* chain) {
    (void)chain;
    FROST_UNIMPLEMENTED();
}
GxPipeline* CGxDevice::PipelineCreate(const GXPIPELINEDESC* desc, uint64_t hash, const char* name) {
    (void)desc;
    (void)hash;
    (void)name;
    FROST_UNIMPLEMENTED();
    return nullptr;
}
void CGxDevice::PipelineDestroy(GxPipeline* pipeline) {
    (void)pipeline;
    FROST_UNIMPLEMENTED();
}
uint16_t* CGxDevice::CreateVertexBufferFormat(uint16_t* result, const GXVERTEXBUFFERFORMATDESC* desc,
    GXVERTEXBUFFERFORMAT** format) {
    (void)desc;
    FROST_UNIMPLEMENTED();
    *format = nullptr;
    *result = 1;
    return result;
}
// ref: FUN_142291090 (slot 0x150 of every device through 0x14229b750)
void CGxDevice::ObjectFree(void* object) {
    if (object != nullptr) {
        GxGetAllocator()->deallocate(object);
    }
}
void CGxDevice::Slot158() {}
// (a device with no swap chains of its own has none current)
void* CGxDevice::CurrentSwapChain() {
    return nullptr;
}
void CGxDevice::Slot168() {}
// ref: FUN_1422922c0
bool CGxDevice::VariableRateShadingSupported(uint8_t tier) {
    return tier == 0;
}
// ref: FUN_1422922b0
bool CGxDevice::ShadingRateSupported(uint32_t, int32_t, uint8_t a4) {
    return a4 == 0;
}
bool CGxDevice::FrameBegin(void*) {
    FROST_UNIMPLEMENTED();  // FUN_141370aa0
    return false;
}
void CGxDevice::Slot188() {}
void* CGxDevice::FrameEnd(void* out, void*) {
    FROST_UNIMPLEMENTED();
    return out;
}
uint32_t CGxDevice::FrameCount() {
    FROST_UNIMPLEMENTED();
    return 0;
}
bool CGxDevice::BufCreate(GxBuf* buf) {
    (void)buf;
    FROST_UNIMPLEMENTED();
    return false;
}
void CGxDevice::BufDestroy(GxBuf* buf) {
    (void)buf;
    FROST_UNIMPLEMENTED();
}
void* CGxDevice::Slot1b0(const void* desc) {
    (void)desc;
    FROST_UNIMPLEMENTED();
    return nullptr;
}
void CGxDevice::Slot1b8(void* object) {
    (void)object;
    FROST_UNIMPLEMENTED();
}
void* CGxDevice::Slot1c0(const void* desc) {
    (void)desc;
    FROST_UNIMPLEMENTED();
    return nullptr;
}
void CGxDevice::Slot1c8() {
    FROST_UNIMPLEMENTED();
}
void CGxDevice::Slot1d0() {
    FROST_UNIMPLEMENTED();
}
void CGxDevice::Slot1d8() {
    FROST_UNIMPLEMENTED();
}
void CGxDevice::Slot1e0() {
    FROST_UNIMPLEMENTED();
}
bool CGxDevice::TexHeapCreate(const void*, void** heap) {
    *heap = nullptr;
    return false;
}
void CGxDevice::TexHeapDestroy(void*) {}
// ref: FUN_142290cf0
GXTEXALLOCINFO* CGxDevice::TexAllocationInfo(GXTEXALLOCINFO* out, const void*) {
    out->m_size = 0;
    out->m_alignment = 0;
    return out;
}
bool CGxDevice::TexCreate(const void*, void** tex) {
    FROST_UNIMPLEMENTED();
    *tex = nullptr;
    return false;
}
void CGxDevice::TexDestroy(void*) {
    FROST_UNIMPLEMENTED();
}
void* CGxDevice::DefaultTex() {
    FROST_UNIMPLEMENTED();
    return nullptr;
}
void CGxDevice::Slot218() {}
void CGxDevice::Slot220() {
    FROST_UNIMPLEMENTED();
}
void CGxDevice::Slot228() {
    FROST_UNIMPLEMENTED();
}
void CGxDevice::Slot230() {
    FROST_UNIMPLEMENTED();
}
void CGxDevice::Slot238() {
    FROST_UNIMPLEMENTED();
}
void CGxDevice::Slot240() {
    FROST_UNIMPLEMENTED();
}
void CGxDevice::Slot248() {
    FROST_UNIMPLEMENTED();
}
void* CGxDevice::TexShaderView(void*, uint32_t) {
    FROST_UNIMPLEMENTED();
    return nullptr;
}
void* CGxDevice::TexUnorderedView(void*) {
    FROST_UNIMPLEMENTED();
    return nullptr;
}
void* CGxDevice::TexTargetView(void*) {
    FROST_UNIMPLEMENTED();
    return nullptr;
}
void CGxDevice::Slot268() {}
void CGxDevice::Slot270() {}
void* CGxDevice::QueryCreate(uint8_t) {
    FROST_UNIMPLEMENTED();
    return nullptr;
}
void CGxDevice::QueryDestroy(void*) {
    FROST_UNIMPLEMENTED();
}
bool CGxDevice::QueryInfo(void*, uint8_t, uint32_t*) {
    FROST_UNIMPLEMENTED();
    return false;
}
bool CGxDevice::QueryGetResult(void*, void*, uint32_t) {
    FROST_UNIMPLEMENTED();
    return false;
}
void* CGxDevice::CreateComputeShader(uint32_t kind, uint16_t permutation) {
    (void)kind;
    (void)permutation;
    FROST_UNIMPLEMENTED();
    return nullptr;
}
void CGxDevice::DestroyComputeShader(void* shader) {
    (void)shader;
    FROST_UNIMPLEMENTED();
}

GXREGISTRYLINK g_gxRegisteredBufs = { &g_gxRegisteredBufs, &g_gxRegisteredBufs };
SRWLOCK g_gxRegisteredBufsLock = SRWLOCK_INIT;

// ref: FUN_142290d00
void CGxDevice::ReregisterObjects() {
    this->CreateVertexFormats();
    AcquireSRWLockExclusive(&g_gxRegisteredBufsLock);
    for (GXREGISTRYLINK* link = g_gxRegisteredBufs.m_next; link != &g_gxRegisteredBufs; link = link->m_next) {
        this->BufCreate(reinterpret_cast<GxBuf*>(reinterpret_cast<uint8_t*>(link) - 0x10));
    }
    ReleaseSRWLockExclusive(&g_gxRegisteredBufsLock);
}

// ref: FUN_142290f60
void CGxDevice::ReleaseRegisteredObjects() {
    AcquireSRWLockExclusive(&g_gxRegisteredBufsLock);
    for (GXREGISTRYLINK* link = g_gxRegisteredBufs.m_next; link != &g_gxRegisteredBufs; link = link->m_next) {
        this->BufDestroy(reinterpret_cast<GxBuf*>(reinterpret_cast<uint8_t*>(link) - 0x10));
    }
    ReleaseSRWLockExclusive(&g_gxRegisteredBufsLock);
    // The 0x2b vertex formats' two objects each (+0x168).
    auto formats = &Field<void*>(0x168);
    for (uint32_t i = 0; i < 0x2b * 2; i++) {
        this->ObjectFree(formats[i]);
    }
    // FUN_142292c90: the deque of {key, object} at +0x440 (blocks of eight).
    auto map = Field<uint8_t**>(0x440);
    uint64_t mapSize = Field<uint64_t>(0x448);
    uint64_t offset = Field<uint64_t>(0x450);
    uint64_t end = offset + Field<uint64_t>(0x458);
    for (uint64_t index = offset; index != end; index++) {
        uint8_t* block = map[(index >> 3) & (mapSize - 1)];
        void* object = *reinterpret_cast<void**>(block + (index & 7) * 0x10 + 8);
        if (object != nullptr) {
            static_cast<CGxDevice*>(g_gxDevice)->ObjectFree(object);
        }
    }
    Field<uint64_t>(0x450) = 0;
    Field<uint64_t>(0x458) = 0;
    // The map of {key, object} at +0x418 (0 empty, -1 removed).
    uint64_t capacity = Field<uint64_t>(0x418);
    auto entries = Field<uint8_t*>(0x420);
    for (uint64_t i = 0; i < capacity; i++) {
        auto entry = reinterpret_cast<uint64_t*>(entries + i * 0x10);
        if (entry[0] - 1 < 0xfffffffffffffffeull && entry[1] != 0) {
            static_cast<CGxDevice*>(g_gxDevice)->ObjectFree(reinterpret_cast<void*>(entry[1]));
        }
        entry[0] = 0;
    }
    Field<uint64_t>(0x430) = 0;
    Field<uint64_t>(0x428) = 0;
}

// ref: FUN_14228c0b0
void* GxSwapChainCreate(const GXSWAPCHAINPARAMS* params) {
    return static_cast<CGxDevice*>(g_gxDevice)->SwapChainCreate(params);
}

// ref: FUN_14228c0d0
void GxSwapChainDestroy(void** chain) {
    if (*chain != nullptr) {
        static_cast<CGxDevice*>(g_gxDevice)->SwapChainDestroy(*chain);
        *chain = nullptr;
    }
}

// ref: FUN_14228c090
bool GxSwapChainBegin(void* chain) {
    return static_cast<CGxDevice*>(g_gxDevice)->SwapChainBegin(chain);
}

// ref: FUN_1422885f0
void* GxSwapChainGetBackBuffer(void* chain) {
    return static_cast<CGxDevice*>(g_gxDevice)->SwapChainGetBackBuffer(chain);
}

// ref: FUN_14228c140
void GxSwapChainPresent(void* chain) {
    static_cast<CGxDevice*>(g_gxDevice)->SwapChainPresent(chain);
}

// ref: FUN_14228c100
void GxSwapChainSkip(void* chain) {
    static_cast<CGxDevice*>(g_gxDevice)->Slot128(chain);
}

// ref: FUN_14228c120
void GxSwapChainEnd(void* chain) {
    static_cast<CGxDevice*>(g_gxDevice)->SwapChainEnd(chain);
}

// ---- vertex formats ------------------------------------------------------------------------

namespace {

// An attribute type as a pm vertex format (DAT_142913520).
const uint8_t s_attribFormats[8] = { 0x22, 0x1f, 0x17, 0x2d, 0x35, 0x1b, 0x19, 0x38 };

// The HLSL semantic names (0x142913490) and indices (the constructor's table in FUN_142291c90).
const char* const s_semanticNames[GXVA_COUNT] = {
    "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "COLOR", "COLOR", "TEXCOORD", "TEXCOORD",
    "TEXCOORD", "TEXCOORD", "TEXCOORD", "TEXCOORD", "TEXCOORD", "TEXCOORD", "TANGENT", "BINORMAL",
};
const uint32_t s_semanticIndices[GXVA_COUNT] = { 0, 0, 0, 0, 0, 1, 0, 1, 2, 3, 4, 5, 6, 7, 0, 0 };

// The format whose stride each stream of a format takes (DAT_142912380); 0x2b is none.
const uint8_t s_streamStrideFormats[GX_VERTEX_FORMAT_COUNT][2] = {
    { 43, 43 }, { 1, 43 }, { 2, 43 }, { 3, 43 }, { 4, 43 }, { 5, 43 }, { 6, 43 }, { 7, 43 },
    { 8, 43 }, { 9, 43 }, { 10, 43 }, { 11, 43 }, { 12, 43 }, { 13, 43 }, { 14, 43 }, { 15, 43 },
    { 16, 43 }, { 17, 43 }, { 18, 43 }, { 19, 43 }, { 20, 43 }, { 21, 43 }, { 22, 43 }, { 23, 43 },
    { 24, 43 }, { 25, 43 }, { 26, 43 }, { 27, 43 }, { 43, 28 }, { 43, 29 }, { 43, 30 }, { 18, 28 },
    { 19, 28 }, { 20, 29 }, { 5, 29 }, { 9, 29 }, { 10, 29 }, { 22, 29 }, { 23, 29 }, { 24, 29 },
    { 25, 29 }, { 26, 29 }, { 42, 43 },
};

}

// ref: FUN_142291c90
void CGxDevice::CreateVertexFormats() {
    auto layouts = &Field<GXVERTEXBUFFERFORMAT*>(0x168);
    for (uint32_t i = 0; i < GX_VERTEX_FORMAT_COUNT; i++) {
        const GXVERTEXFORMATDESC& format = g_gxVertexFormats[i];
        if (format.m_attribs == nullptr) {
            continue;
        }
        GXVERTEXATTRIBDESC streams[2][16] = {};
        uint32_t counts[2] = {};
        for (uint32_t k = 0; k < format.m_count; k++) {
            const GXVERTEXATTRIB& attrib = format.m_attribs[k];
            uint8_t stream = static_cast<uint8_t>(attrib.m_stream);
            GXVERTEXATTRIBDESC& desc = streams[stream][counts[stream]++];
            desc.m_format = s_attribFormats[static_cast<uint8_t>(attrib.m_type)];
            desc.m_offset = attrib.m_offset;
            desc.m_semantic = attrib.m_semantic;
            desc.m_name = s_semanticNames[attrib.m_semantic];
            desc.m_index = s_semanticIndices[attrib.m_semantic];
        }
        for (uint32_t stream = 0; stream < 2; stream++) {
            if (counts[stream] == 0) {
                layouts[i * 2 + stream] = nullptr;
                continue;
            }
            GXVERTEXBUFFERFORMATDESC desc = {};
            desc.m_stream = stream;
            desc.m_count = counts[stream];
            desc.m_attribs = streams[stream];
            desc.m_instanced = stream != 0;
            desc.m_stride = g_gxVertexFormats[s_streamStrideFormats[i][stream]].m_stride;
            GXVERTEXBUFFERFORMAT* created = nullptr;
            uint16_t result;
            if (*this->CreateVertexBufferFormat(&result, &desc, &created) != 0) {
                GxLog("CreateVertexBufferFormat %zu failed", static_cast<size_t>(i));
                this->m_ready = 0;
            }
            layouts[i * 2 + stream] = created;
        }
    }
}

// ref: FUN_142291880
CGxDevice::VERTEXFORMATREF* CGxDevice::GetVertexFormat(VERTEXFORMATREF* out, uint8_t format) {
    auto layouts = &Field<GXVERTEXBUFFERFORMAT*>(0x168 + format * 0x10);
    out->m_layouts = layouts;
    if (layouts[1] != nullptr) {
        out->m_state = 2;
        return out;
    }
    out->m_state = layouts[0] != nullptr ? 1 : 0;
    return out;
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141370aa0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_142290980=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_142290a50=GenForwardMissing")
// ---- end of link fallbacks ----
