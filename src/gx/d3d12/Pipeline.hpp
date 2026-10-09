#ifndef GX_D3D12_PIPELINE_HPP
#define GX_D3D12_PIPELINE_HPP

// The D3D12 device's pipelines: the PSO and what each shader stage binds.

#include <cstddef>
#include <cstdint>
#include <d3d12.h>

#include "gx/GxPipeline.hpp"

// What a pipeline's shaders of one stage read (0x20 bytes), from their permutation's tables.
struct PIPELINESTAGE {
    uint64_t m_srvs;            // 0x00: shader resource slots
    uint64_t m_uavs;            // 0x08
    uint64_t m_constants;       // 0x10: constant buffer slots
    uint16_t m_samplers;        // 0x18
    uint16_t m_pad1a;
    uint32_t m_flags;           // 0x1c
};

// The device's pipeline object (0xc8 bytes, then the layouts).
struct GxPipeline {
    uint64_t m_hash;                    // 0x00
    volatile LONG m_state;              // 0x08: 0 building, 1 ready, 2 failed, 3 destroyed
    uint32_t m_pad0c;
    ID3D12PipelineState* m_pso;         // 0x10
    PIPELINESTAGE m_stages[3];          // 0x18: vertex, geometry, pixel
    GXPIPELINEDESC m_desc;              // 0x78
    // GXVERTEXBUFFERFORMAT* m_layouts[m_desc.m_layoutCount] at 0xc8
};
static_assert(offsetof(GxPipeline, m_desc) == 0x78, "GxPipeline layout");
static_assert(sizeof(GxPipeline) == 0xc8, "GxPipeline is 0xc8 bytes");

// The primitive topology each Gx topology draws as (DAT_14291c240).
extern const D3D12_PRIMITIVE_TOPOLOGY s_gxTopologies[6];

#endif
