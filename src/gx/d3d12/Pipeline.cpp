#include "gx/d3d12/Pipeline.hpp"

#include <cstring>
#include <new>

#include "blz/Function.hpp"
#include "gx/d3d12/DeviceD3d12.hpp"
#include "gx/Gx.hpp"
#include "gx/GxPlatform.hpp"
#include "gx/GxShader.hpp"
#include "gx/GxVertexFormat.hpp"
#include "storm/String.hpp"
#include "threadpool/ThreadPool.hpp"

using namespace gxDx12Backend;

const D3D12_PRIMITIVE_TOPOLOGY s_gxTopologies[6] = {
    D3D_PRIMITIVE_TOPOLOGY_POINTLIST, D3D_PRIMITIVE_TOPOLOGY_LINELIST, D3D_PRIMITIVE_TOPOLOGY_LINESTRIP,
    D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, D3D_PRIMITIVE_TOPOLOGY_UNDEFINED,
};

namespace {

// The blend factors and operations of each Gx blend mode (DAT_14291bff0, DAT_14291c090,
// DAT_14291c040, DAT_14291c0e0) and the operations (DAT_14291c128).
const uint32_t s_srcBlend[20] = { 2, 2, 5, 5, 9, 9, 9, 6, 6, 5, 2, 14, 10, 2, 2, 7, 1, 0, 0, 0 };
const uint32_t s_destBlend[20] = { 1, 1, 6, 2, 1, 3, 2, 2, 1, 1, 2, 15, 2, 6, 2, 6, 3, 0, 0, 0 };
const uint32_t s_srcBlendAlpha[20] = { 2, 2, 2, 1, 7, 7, 7, 6, 6, 5, 1, 14, 2, 2, 2, 2, 1, 0, 0, 0 };
const uint32_t s_destBlendAlpha[20] = { 1, 1, 6, 2, 1, 5, 2, 2, 1, 1, 2, 15, 1, 6, 2, 2, 5, 0, 1, 2 };
const uint32_t s_blendOps[6] = { 1, 2, 3, 4, 5, 0 };
// Cull modes (DAT_14291c140), comparisons (DAT_14291c150), stencil operations (DAT_14291c170)
// and topology types (DAT_14291c258).
const uint32_t s_cullModes[4] = { 1, 3, 2, 0 };
const uint32_t s_comparisons[8] = { 4, 3, 7, 2, 1, 5, 6, 8 };
const uint32_t s_stencilOps[8] = { 1, 2, 3, 7, 8, 4, 5, 6 };
const uint32_t s_topologyTypes[6] = { 1, 2, 2, 3, 3, 0 };
// The depth bias of each Gx bias level (DAT_142915cd0, with the last overridden to 0.01) and
// the scale to D3D12's integer bias (DAT_142915cbc).
const float s_depthBiases[5] = { 0.0f, 3.051804378628731e-05f, 3.814697265625e-06f, 0.0005000000237487257f, 0.01f };
const float s_depthBiasScale = 16777216.0f;
// A pm vertex format as DXGI (DAT_142919ce0).
const DXGI_FORMAT s_vertexFormats[64] = {
    static_cast<DXGI_FORMAT>(0), static_cast<DXGI_FORMAT>(61), static_cast<DXGI_FORMAT>(0), static_cast<DXGI_FORMAT>(63),
    static_cast<DXGI_FORMAT>(62), static_cast<DXGI_FORMAT>(64), static_cast<DXGI_FORMAT>(65), static_cast<DXGI_FORMAT>(56),
    static_cast<DXGI_FORMAT>(58), static_cast<DXGI_FORMAT>(57), static_cast<DXGI_FORMAT>(59), static_cast<DXGI_FORMAT>(54),
    static_cast<DXGI_FORMAT>(49), static_cast<DXGI_FORMAT>(0), static_cast<DXGI_FORMAT>(51), static_cast<DXGI_FORMAT>(50),
    static_cast<DXGI_FORMAT>(52), static_cast<DXGI_FORMAT>(0), static_cast<DXGI_FORMAT>(115), static_cast<DXGI_FORMAT>(0),
    static_cast<DXGI_FORMAT>(85), static_cast<DXGI_FORMAT>(0), static_cast<DXGI_FORMAT>(86), static_cast<DXGI_FORMAT>(42),
    static_cast<DXGI_FORMAT>(43), static_cast<DXGI_FORMAT>(41), static_cast<DXGI_FORMAT>(35), static_cast<DXGI_FORMAT>(37),
    static_cast<DXGI_FORMAT>(36), static_cast<DXGI_FORMAT>(38), static_cast<DXGI_FORMAT>(34), static_cast<DXGI_FORMAT>(28),
    static_cast<DXGI_FORMAT>(29), static_cast<DXGI_FORMAT>(31), static_cast<DXGI_FORMAT>(30), static_cast<DXGI_FORMAT>(32),
    static_cast<DXGI_FORMAT>(87), static_cast<DXGI_FORMAT>(91), static_cast<DXGI_FORMAT>(24), static_cast<DXGI_FORMAT>(25),
    static_cast<DXGI_FORMAT>(0), static_cast<DXGI_FORMAT>(26), static_cast<DXGI_FORMAT>(67), static_cast<DXGI_FORMAT>(17),
    static_cast<DXGI_FORMAT>(18), static_cast<DXGI_FORMAT>(16), static_cast<DXGI_FORMAT>(11), static_cast<DXGI_FORMAT>(13),
    static_cast<DXGI_FORMAT>(12), static_cast<DXGI_FORMAT>(14), static_cast<DXGI_FORMAT>(10), static_cast<DXGI_FORMAT>(7),
    static_cast<DXGI_FORMAT>(8), static_cast<DXGI_FORMAT>(6), static_cast<DXGI_FORMAT>(3), static_cast<DXGI_FORMAT>(4),
    static_cast<DXGI_FORMAT>(2), static_cast<DXGI_FORMAT>(71), static_cast<DXGI_FORMAT>(72), static_cast<DXGI_FORMAT>(74),
    static_cast<DXGI_FORMAT>(75), static_cast<DXGI_FORMAT>(77), static_cast<DXGI_FORMAT>(78), static_cast<DXGI_FORMAT>(80),
};

// The bytecode a permutation carries, and the stage's binding tables from it.
D3D12_SHADER_BYTECODE Bytecode(const GXSHADERPERMUTE* permute, PIPELINESTAGE* stage) {
    D3D12_SHADER_BYTECODE code = {};
    if (permute == nullptr) {
        std::memset(stage, 0, sizeof(PIPELINESTAGE));
        return code;
    }
    code.pShaderBytecode = reinterpret_cast<const uint8_t*>(&permute->m_bytecodeOffset) + permute->m_bytecodeOffset;
    code.BytecodeLength = permute->m_bytecodeSize;
    stage->m_srvs = permute->m_20;
    stage->m_uavs = permute->m_18;
    stage->m_constants = permute->m_38;
    stage->m_samplers = permute->m_30;
    stage->m_flags = permute->m_08;
    return code;
}

// The pipeline is freed and its PSO released once the frame is done (FUN_1422d7bc0).
// ref: FUN_1422c9290
void PipelineRelease(CGxDeviceD3d12* device, GxPipeline* pipeline) {
    IUnknown* pso = pipeline->m_pso;
    g_gxDeviceLock.Enter();
    device->m_frame->m_releases.push_back(pso);
    g_gxDeviceLock.Leave();
    pipeline->m_pso = nullptr;
    GxGetAllocator()->deallocate(pipeline);
}

struct PIPELINEJOB {
    CGxDeviceD3d12* m_device;
    GxPipeline* m_pipeline;
};

// LAB_1422e7e80
void PipelineJob(void* storage) {
    auto job = static_cast<PIPELINEJOB*>(storage);
    job->m_device->BuildPipeline(&job->m_pipeline->m_desc, job->m_pipeline);
}

}

// The PSO from the desc's shaders and state; the pipeline is ready (1) or failed (2) after,
// and freed here when it was destroyed (3) while building.
// ref: FUN_1422d86b0
bool CGxDeviceD3d12::BuildPipeline(const GXPIPELINEDESC* desc, GxPipeline* pipeline) {
    if (this->m_ready == 0) {
        if (InterlockedExchange(&pipeline->m_state, 2) == 3) {
            if (InterlockedExchange(&pipeline->m_state, 3) != 0) {
                PipelineRelease(this, pipeline);
            }
        }
        return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC state = {};
    state.pRootSignature = this->m_rootSignatures[0];
    GXSHADERBUFFER vertexBuffer;
    GXSHADERBUFFER geometryBuffer;
    GXSHADERBUFFER pixelBuffer;
    const GXSHADERPERMUTE* vertex = GxShaderGetPermute(desc->m_vertexShader, desc->m_vsPermute, &vertexBuffer);
    const GXSHADERPERMUTE* geometry = nullptr;
    if (desc->m_geometryShader != 0) {
        geometry = GxShaderGetPermute(desc->m_geometryShader, desc->m_gsPermute, &geometryBuffer);
    }
    const GXSHADERPERMUTE* pixel = GxShaderGetPermute(desc->m_pixelShader, desc->m_psPermute, &pixelBuffer);
    state.VS = Bytecode(vertex, &pipeline->m_stages[0]);
    state.GS = Bytecode(geometry, &pipeline->m_stages[1]);
    state.PS = Bytecode(pixel, &pipeline->m_stages[2]);

    state.BlendState.AlphaToCoverageEnable = desc->m_alphaToCoverage;
    state.BlendState.IndependentBlendEnable = desc->m_independentBlend;
    uint32_t described = desc->m_independentBlend != 0 ? 4 : 1;
    for (uint32_t i = 0; i < described; i++) {
        uint8_t mode = desc->m_blend[i][0];
        uint8_t bits = desc->m_blend[i][1];
        D3D12_RENDER_TARGET_BLEND_DESC& target = state.BlendState.RenderTarget[i];
        target.BlendEnable = mode > 1;
        target.LogicOpEnable = FALSE;
        target.SrcBlend = static_cast<D3D12_BLEND>(s_srcBlend[mode]);
        target.DestBlend = static_cast<D3D12_BLEND>(s_destBlend[mode]);
        target.BlendOp = static_cast<D3D12_BLEND_OP>(s_blendOps[bits & 0xf]);
        target.SrcBlendAlpha = static_cast<D3D12_BLEND>(s_srcBlendAlpha[mode]);
        target.DestBlendAlpha = static_cast<D3D12_BLEND>(s_destBlendAlpha[mode]);
        target.BlendOpAlpha = target.BlendOp;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = static_cast<UINT8>(bits >> 4);
    }
    // The targets not described take the default (DAT_14291b6e0): no blending, all channels.
    for (uint32_t i = described; i < 8; i++) {
        D3D12_RENDER_TARGET_BLEND_DESC& target = state.BlendState.RenderTarget[i];
        target.BlendEnable = FALSE;
        target.LogicOpEnable = FALSE;
        target.SrcBlend = D3D12_BLEND_ONE;
        target.DestBlend = D3D12_BLEND_ZERO;
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlendAlpha = D3D12_BLEND_ZERO;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = 0xf;
    }
    state.SampleMask = 0xffffffff;
    state.RasterizerState.FillMode = static_cast<D3D12_FILL_MODE>((~(desc->m_rasterFlags >> 1) & 1) | 2);
    state.RasterizerState.CullMode = static_cast<D3D12_CULL_MODE>(s_cullModes[desc->m_cull]);
    state.RasterizerState.FrontCounterClockwise = TRUE;
    state.RasterizerState.DepthBias = static_cast<INT>(s_depthBiases[desc->m_depthBias] * s_depthBiasScale);
    state.RasterizerState.DepthBiasClamp = 0.0f;
    state.RasterizerState.SlopeScaledDepthBias = 0.0f;
    state.RasterizerState.DepthClipEnable = ~(desc->m_rasterFlags >> 2) & 1;
    state.RasterizerState.MultisampleEnable = TRUE;
    state.RasterizerState.AntialiasedLineEnable = FALSE;
    state.RasterizerState.ForcedSampleCount = 0;
    state.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    uint8_t depth = desc->m_depthFlags;
    state.DepthStencilState.DepthEnable = depth & 1;
    state.DepthStencilState.DepthWriteMask = static_cast<D3D12_DEPTH_WRITE_MASK>((depth >> 2) & 1);
    state.DepthStencilState.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(s_comparisons[depth >> 3]);
    state.DepthStencilState.StencilEnable = (depth >> 1) & 1;
    state.DepthStencilState.StencilReadMask = desc->m_stencilReadMask;
    state.DepthStencilState.StencilWriteMask = desc->m_stencilWriteMask;
    state.DepthStencilState.FrontFace.StencilFailOp = static_cast<D3D12_STENCIL_OP>(s_stencilOps[desc->m_stencilFail]);
    state.DepthStencilState.FrontFace.StencilDepthFailOp = static_cast<D3D12_STENCIL_OP>(s_stencilOps[desc->m_stencilDepthFail]);
    state.DepthStencilState.FrontFace.StencilPassOp = static_cast<D3D12_STENCIL_OP>(s_stencilOps[desc->m_stencilPass]);
    state.DepthStencilState.FrontFace.StencilFunc = static_cast<D3D12_COMPARISON_FUNC>(s_comparisons[desc->m_stencilFunc]);
    state.DepthStencilState.BackFace = state.DepthStencilState.FrontFace;

    D3D12_INPUT_ELEMENT_DESC elements[32];
    uint32_t elementCount = 0;
    for (uint32_t i = 0; i < desc->m_layoutCount; i++) {
        const GXVERTEXBUFFERFORMATDESC* layout = desc->m_layouts[i]->m_desc;
        for (uint32_t k = 0; k < layout->m_count && elementCount < 32; k++) {
            const GXVERTEXATTRIBDESC& attrib = layout->m_attribs[k];
            D3D12_INPUT_ELEMENT_DESC& element = elements[elementCount++];
            if (attrib.m_name == nullptr) {
                element.SemanticName = "Attr";
                element.SemanticIndex = attrib.m_semantic;
            } else {
                element.SemanticName = attrib.m_name;
                element.SemanticIndex = attrib.m_index;
            }
            element.Format = s_vertexFormats[attrib.m_format];
            element.InputSlot = layout->m_stream;
            element.AlignedByteOffset = attrib.m_offset;
            element.InputSlotClass = static_cast<D3D12_INPUT_CLASSIFICATION>(layout->m_instanced != 0);
            element.InstanceDataStepRate = layout->m_instanced != 0;
        }
    }
    state.InputLayout.pInputElementDescs = elementCount != 0 ? elements : nullptr;
    state.InputLayout.NumElements = elementCount;
    state.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    state.PrimitiveTopologyType = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(s_topologyTypes[desc->m_topology]);
    uint32_t targets = 0;
    for (uint32_t i = 0; i < 4 && desc->m_targetFormats[i] != 0; i++) {
        state.RTVFormats[i] = ResourceFormat(desc->m_targetFormats[i]);
        targets++;
    }
    state.NumRenderTargets = targets;
    state.DSVFormat = desc->m_depthFormat != 0 ? ResourceFormat(desc->m_depthFormat) : DXGI_FORMAT_UNKNOWN;
    state.SampleDesc.Count = 1u << (desc->m_samplesLog2 & 0x1f);
    state.SampleDesc.Quality = 0;

    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = this->m_device->CreateGraphicsPipelineState(&state, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&pso));
    if (FAILED(hr)) {
        char vertexName[0x100];
        char pixelName[0x100];
        char shaders[0x200];
        char message[0x300];
        SStrPrintf(shaders, sizeof(shaders), "%s VS: %s(%d) PS: %s(%d)", "", GxShaderName(desc->m_vertexShader, vertexName, sizeof(vertexName)),
            desc->m_vsPermute, GxShaderName(desc->m_pixelShader, pixelName, sizeof(pixelName)), desc->m_psPermute);
        SStrPrintf(message, sizeof(message), "CreateGraphicsPipelineState failed: %s", shaders);
        this->Fail(hr, message);
    }
    pipeline->m_pso = pso;
    if (InterlockedExchange(&pipeline->m_state, pso != nullptr ? 1 : 2) == 3) {
        if (InterlockedExchange(&pipeline->m_state, 3) != 0) {
            PipelineRelease(this, pipeline);
        }
    }
    return pso != nullptr;
}

// Device slot 0x138: the pipeline object with its desc and layouts, built now or by a job.
// ref: FUN_1422dc720
GxPipeline* CGxDeviceD3d12::PipelineCreate(const GXPIPELINEDESC* desc, uint64_t hash, const char* name) {
    (void)name;
    void* memory = GxGetAllocator()->allocate(static_cast<uint64_t>(desc->m_layoutCount) * 8 + sizeof(GxPipeline), 0x10);
    GxPipeline* pipeline = static_cast<GxPipeline*>(memory);
    std::memset(pipeline, 0, sizeof(GxPipeline));
    pipeline->m_hash = hash;
    pipeline->m_desc = *desc;
    auto layouts = reinterpret_cast<GXVERTEXBUFFERFORMAT**>(pipeline + 1);
    for (uint32_t i = 0; i < desc->m_layoutCount; i++) {
        layouts[i] = desc->m_layouts[i];
    }
    pipeline->m_desc.m_layouts = layouts;
    if (this->m_ready == 0) {
        InterlockedExchange(&pipeline->m_state, 2);
        return pipeline;
    }
    if (this->Field<uint8_t>(0x14a) == 0 || (reinterpret_cast<const uint8_t*>(desc)[0x20] & 1) != 0) {
        this->BuildPipeline(desc, pipeline);
        return pipeline;
    }
    blz::function_storage job = {};
    job.m_invoke = reinterpret_cast<void*>(PipelineJob);
    job.m_manager = blz::function_trivial_manager;
    auto data = reinterpret_cast<PIPELINEJOB*>(job.m_storage);
    data->m_device = this;
    data->m_pipeline = pipeline;
    ThreadPoolSubmit(1, &job, 1, nullptr);
    if (job.m_manager != blz::function_empty_manager) {
        job.m_manager(job.m_storage, nullptr, 0);
    }
    return pipeline;
}

// Device slot 0x140: a built pipeline goes now (its PSO when the frame is done); one still
// building is freed by its job.
// ref: FUN_1422dc920
void CGxDeviceD3d12::PipelineDestroy(GxPipeline* pipeline) {
    if (InterlockedExchange(&pipeline->m_state, 3) == 0) {
        return;
    }
    PipelineRelease(this, pipeline);
}
