#ifndef TACT_DYNAMIC_INIT_HPP
#define TACT_DYNAMIC_INIT_HPP

// pkg/tact/3.6.39/src/async_state/async_initialize_dynamic_state.cpp: the operation that
// initializes an instance over local ("dynamic") storage. It runs as a state machine on the
// instance's scheduler: download queue, build/CDN configuration, container, sizes, CDN
// indices, patch index, patch manifest, then the root manifest and the encoding table.

#include "blz/AllocString.hpp"
#include "tact/AsyncState.hpp"
#include "tact/Common.hpp"
#include "tact/Instance.hpp"
#include <cstddef>
#include <cstdint>

namespace tact {

// (context, instance, stage, done, total)
typedef void (*PROGRESSFUNCTION)(void* context, Instance* instance, int32_t stage, uint64_t done, uint64_t total);

enum DYNAMICINITSTAGE : uint8_t {
    DI_START = 0,
    DI_DOWNLOAD_QUEUE = 1,
    DI_DOWNLOAD_QUEUE_WAIT = 2,
    DI_CONFIG = 3,
    DI_CONTAINER = 4,
    DI_SIZES = 5,
    DI_INDICES = 6,
    DI_PATCH_INDEX = 7,
    DI_PATCH_MANIFEST = 8,
    DI_FETCH = 9,
    DI_MANIFESTS = 10,
    DI_DONE = 11,
};

// 0x398 bytes, vtable 0x1428e8860.
class DynamicInitState : public AsyncState {
    public:
    uint8_t m_stage;                        // 0x40
    PROGRESSFUNCTION m_progress;            // 0x48
    void* m_progressContext;                // 0x50
    int64_t m_progressTime;                 // 0x58: ns of the last report
    int32_t m_progressStage;                // 0x60
    uint64_t m_patchManifestSize;           // 0x68
    uint64_t m_indexTotal;                  // 0x70: the index set's last reported total
    uint64_t m_done;                        // 0x78
    uint64_t m_total;                       // 0x80
    uint64_t m_fetchDone;                   // 0x88
    uint64_t m_fetchTotal;                  // 0x90
    INSTANCEPARAMS m_params;                // 0x98
    blz::alloc_string m_buildConfigPath;    // 0x278
    blz::alloc_string m_cdnPath;            // 0x298
    Array<blz::alloc_string> m_hostStrings; // 0x2b8
    Array<const char*> m_hosts;             // 0x2d0
    blz::alloc_string m_armadilloKey;       // 0x2e8
    blz::alloc_string m_1c0;                // 0x308
    int32_t m_pendingConfigs;               // 0x328
    uint64_t m_configOp;                    // 0x330
    uint64_t m_readOp;                      // 0x338: patch index / patch manifest read
    uint64_t m_rootOp;                      // 0x340
    uint64_t m_eSpecOp;                     // 0x348
    uint64_t m_350;                         // 0x350
    uint64_t m_fetchOp;                     // 0x358
    uint8_t* m_patchManifestData;           // 0x360
    uint32_t m_retries;                     // 0x368
    RootReader* m_rootReader;               // 0x370
    uint8_t* m_rootData;                    // 0x378
    uint8_t* m_eSpecData;                   // 0x380
    uint8_t* m_patchIndexData;              // 0x388
    uint8_t m_error;                        // 0x390: the first failure while others are pending

    DynamicInitState(Instance* instance, uint64_t opId, OPCALLBACK callback, void* context);
    ~DynamicInitState() override;

    void Destroy() override;
    uint8_t GetType() override;
    void OnCancel() override;

    void Start(uint8_t error, INSTANCEPARAMS* params);
    void ApplyParams(INSTANCEPARAMS* params);
    void ReportProgress(int32_t stage);
    void Step();
    void Fail(uint8_t error);
    void Schedule(uint8_t stage);

    void ReadConfigs();
    void MeasureSizes();
    void FetchIndices();
    void ReadPatchIndex();
    uint8_t DecodePatchIndex();
    void ReadPatchManifest();
    void ReadManifests();
    void ReadRoot();

    void OnPatchIndexRead(uint64_t opId);
    void OnPatchManifestRead(uint64_t opId);
    void OnRootRead(uint64_t opId);
};
static_assert(offsetof(DynamicInitState, m_stage) == 0x40, "DynamicInitState layout");
static_assert(offsetof(DynamicInitState, m_params) == 0x98, "DynamicInitState layout");
static_assert(offsetof(DynamicInitState, m_pendingConfigs) == 0x328, "DynamicInitState layout");
static_assert(sizeof(DynamicInitState) == 0x398, "DynamicInitState is 0x398 bytes");

AsyncState* DynamicInitCreate(Instance* instance, uint64_t opId, OPCALLBACK callback, void* context);
void DynamicInitStart(AsyncState* state, uint8_t error, INSTANCEPARAMS* params);

}

#endif
