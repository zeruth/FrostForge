// The RTTI names gxDx12Backend's containers tag their Storm allocations with.

#include "gx/d3d12/DeviceD3d12.hpp"

namespace gxDx12Backend {

const char s_d3d12ObjectTag[] = ".?AUD3d12Object@gxDx12Backend@@";
const char s_d3d12ObjectPtrTag[] = ".PEBUD3d12Object@gxDx12Backend@@";
const char s_texPtrTag[] = ".PEAUTex@gxDx12Backend@@";
const char s_texTag[] = ".?AUTex@gxDx12Backend@@";
const char s_freeListMapTag[] = ".PEAI";
const char s_freeListBlockTag[] = ".I";
const char s_comPtrTag[] = ".?AV?$ComPtr@UIUnknown@@@WRL@Microsoft@@";
const char s_queryPtrTag[] = ".PEAUQuery@gxDx12Backend@@";
const char s_callbackPageTag[] = ".?AUCallbackPage@GxCallbackListBase@@";
const char s_smemPtrTag[] = ".?AV?$unique_ptr@XUsmem_free_deleter@@@blz@@";
const char s_queueFrameTag[] = ".?AUFrameData@Queue@Device@gxDx12Backend@@";
const char s_frameDataTag[] = ".?AUFrameData@Device@gxDx12Backend@@";
const char s_listTag[] = ".?AV?$ComPtr@UID3D12GraphicsCommandList@@@WRL@Microsoft@@";
const char s_allocatorTag[] = ".?AV?$ComPtr@UID3D12CommandAllocator@@@WRL@Microsoft@@";
const char s_residencySetPtrTag[] = ".PEAVResidencySet@D3DX12Residency@@";
const char s_barrierTag[] = ".?AUD3D12_RESOURCE_BARRIER@@";
const char s_stateTrackerTag[] = ".?AU?$pair@QEBUD3d12Object@gxDx12Backend@@UStateTracker@2@@blz@@";
const char s_stateTrackerMapTag[] =
    ".?AV?$probed_hash_map@PEBUD3d12Object@gxDx12Backend@@UStateTracker@2@U?$PointerBucketTraits@PEBUD3d12Object@"
    "gxDx12Backend@@@@U?$hash@PEBUD3d12Object@gxDx12Backend@@@blz@@U?$equal_to@PEBUD3d12Object@gxDx12Backend@@@6@V?$"
    "allocator@U?$pair@QEBUD3d12Object@gxDx12Backend@@UStateTracker@2@@blz@@@6@@blz@@";
const char s_perFrameTag[] = ".?AUPerFrameResources@CopyCommandList@gxDx12Backend@@";
const char s_uploadPageTag[] = ".?AUDescriptorUploadPage@gxDx12Backend@@";
const char s_dataUploadPageTag[] = ".?AUDataUploadPage@gxDx12Backend@@";
const char s_bufPairTag[] = ".?AU?$pair@PEAUGxBuf@@I@blz@@";
const char s_uintVectorTag[] = ".?AV?$embedded_vector@I$04V?$allocator@I@blz@@@blz@@";
const char s_uintTag[] = ".I";
const char s_arrayTag[] = ".?AU?$array@_K$04@blz@@";
const char s_swapChainPtrTag[] = ".PEAUSwapChain@gxDx12Backend@@";
const char s_enqueuePairTag[] = ".?AUEnqueuePair@Device@gxDx12Backend@@";
const char s_commandListPtrTag[] = ".PEAUID3D12CommandList@@";
const char s_copyListPtrTag[] = ".PEAUCopyCommandList@gxDx12Backend@@";
const char s_heapPageTag[] = ".?AV?$unique_ptr@UHeapPage@gxDx12Backend@@U?$smem_del_deleter@UHeapPage@gxDx12Backend@@@@@blz@@";
const char s_samplerMapTag[] =
    ".?AU?$pair@$$CB_KV?$unique_ptr@Usampler@Device@gxDx12Backend@@Usampler_delete@23@@blz@@@blz@@";
const char s_stateMapTag[] = ".?AU?$pair@QEBUD3d12Object@gxDx12Backend@@W4D3D12_RESOURCE_STATES@@@blz@@";

}
