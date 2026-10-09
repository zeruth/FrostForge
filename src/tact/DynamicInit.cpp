#include "tact/DynamicInit.hpp"
#include "tact/Codec.hpp"
#include "tact/ConfigRead.hpp"
#include "tact/Container.hpp"
#include "tact/DownloadQueue.hpp"
#include "tact/FileRead.hpp"
#include "tact/Manifest.hpp"
#include "blz/Log.hpp"
#include "os/Os.hpp"
#include "util/Unimplemented.hpp"
#include <cstring>
#include <new>
#include <windows.h>

namespace tact {

namespace {

const char s_file[] =
    "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\async_state\\async_initialize_dynamic_state.cpp";
const char s_noTag[] = "";
// The eSpec a raw (unencoded) read asks for (DAT_1426cc7c8).
const char s_eSpecNone[] = "n";

const uint8_t s_zeroKey[16] = {};

bool IsZeroKey(const uint8_t key[16]) {
    return std::memcmp(key, s_zeroKey, 16) == 0;
}

// A record names a file only with a key and both sizes.
bool IsDefined(const CONFIGRECORD& record) {
    return !IsZeroKey(record.m_ekey) && record.m_csize != 0 && record.m_esize != 0;
}

uint8_t* IndexSetOf(Instance* instance) {
    return instance->m_downloadQueue + 0x1f0;
}

// The reference returns the instance's memory limit from the shared `return 0`
// (0x14022c3f0) with the limit parameter as its argument.
void* MemoryLimitCreate(uint32_t limit) {
    (void)limit;
    return nullptr;
}

// ref: FUN_1420f3220
void StepTask(void* argument) {
    auto state = static_cast<DynamicInitState*>(argument);
    AsyncStateScope scope(state);
    state->Step();
}

// ref: FUN_1420f2c00
void OnDownloadQueueReady(void* context, uint8_t error) {
    auto state = static_cast<DynamicInitState*>(context);
    AsyncStateScope scope(state);
    if (error == ERR_SUCCESS) {
        state->Schedule(DI_CONFIG);
    } else {
        state->Fail(error);
    }
}

// The build configuration read finished; the container comes once every configuration read
// has.
// ref: FUN_1420f1fe0
void OnBuildConfig(void* context, Instance* instance, uint64_t opId) {
    (void)instance;
    auto state = static_cast<DynamicInitState*>(context);
    AsyncStateScope scope(state);
    state->m_pendingConfigs--;
    state->m_configOp = 0;
    uint8_t error = InstanceGetOpResult(state->m_instance, opId, nullptr);
    if (error == ERR_SUCCESS) {
        if (state->m_pendingConfigs != 0) {
            return;
        }
        error = state->m_error;
        if (error == ERR_SUCCESS) {
            state->Schedule(DI_CONTAINER);
            return;
        }
    } else {
        blz::log(g_logCategory, 4, s_file, 0x16d, "Failed to load build config. Error(%u) : %s", error, ErrorString(error));
        if (state->m_pendingConfigs != 0) {
            state->m_error = error;
            return;
        }
    }
    state->Fail(error);
}

// ref: FUN_1420f21f0
void OnCdnConfig(void* context, uint8_t error) {
    auto state = static_cast<DynamicInitState*>(context);
    AsyncStateScope scope(state);
    int32_t pending = --state->m_pendingConfigs;
    if (error == ERR_SUCCESS) {
        if (pending != 0) {
            return;
        }
        error = state->m_error;
        if (error == ERR_SUCCESS) {
            state->Schedule(DI_CONTAINER);
            return;
        }
    } else {
        blz::log(g_logCategory, 4, s_file, 0x1a5, "Failed to load cdn config. Error(%u) : %s", error, ErrorString(error));
        if (state->m_pendingConfigs != 0) {
            state->m_error = error;
            return;
        }
    }
    state->Fail(error);
}

// ref: FUN_1420f2330
void OnIndexProgress(void* context, uint64_t done, uint64_t total) {
    auto state = static_cast<DynamicInitState*>(context);
    AsyncStateScope scope(state);
    uint64_t previous = state->m_indexTotal;
    state->m_done = done;
    if (total != previous) {
        state->m_indexTotal = total;
        state->m_total += total - previous;
    }
    state->ReportProgress(3);
}

// ref: FUN_1420f23e0
void OnIndicesFetched(void* context, uint8_t error) {
    auto state = static_cast<DynamicInitState*>(context);
    AsyncStateScope scope(state);
    if (error == ERR_SUCCESS) {
        state->Schedule(DI_FETCH);
    } else {
        state->Fail(error);
    }
}

// ref: FUN_1420f2a80
void OnFetched(void* context, Instance* instance, uint64_t opId) {
    (void)instance;
    auto state = static_cast<DynamicInitState*>(context);
    AsyncStateScope scope(state);
    state->m_fetchOp = 0;
    uint8_t error = InstanceGetOpResult(state->m_instance, opId, nullptr);
    if (error == ERR_SUCCESS) {
        state->m_done += state->m_fetchTotal;
        state->Schedule(DI_PATCH_INDEX);
    } else {
        state->Fail(error);
    }
}

// ref: FUN_1420f2b60
void OnFetchProgress(void* context, uint64_t done, uint64_t total) {
    auto state = static_cast<DynamicInitState*>(context);
    AsyncStateScope scope(state);
    state->m_fetchDone = done;
    state->m_fetchTotal = total;
    state->ReportProgress(3);
}

// The encoding table finished reading; a table that does not deserialize only costs the
// eSpec look-up fallback.
// ref: FUN_1420f2cc0
void OnESpecRead(void* context, Instance* instance, uint64_t opId) {
    (void)instance;
    auto state = static_cast<DynamicInitState*>(context);
    AsyncStateScope scope(state);
    state->m_eSpecOp = 0;
    uint8_t error = InstanceGetOpResult(state->m_instance, opId, nullptr);
    if (error == ERR_SUCCESS) {
        if (state->m_instance->m_encodingTable.Parse(0) != ERR_SUCCESS) {
            blz::log(g_logCategory, 3, s_file, 0x405,
                "Failed to deserialize encoding table; eSpec look-up fallback will be disabled.");
            state->m_instance->m_encodingTable.Clear();
        }
    } else {
        blz::log(g_logCategory, 3, s_file, 0x3fe,
            "Failed to read encoding table; eSpec look-up fallback will be disabled. Error(%u) : %s", error,
            ErrorString(error));
    }
    if (state->m_eSpecOp == 0 && state->m_rootOp == 0 && state->m_350 == 0) {
        uint8_t deferred = state->m_error;
        state->m_stage = DI_DONE;
        if (deferred == ERR_SUCCESS) {
            state->m_instance->m_scheduler->Post(&StepTask, state, 0);
        } else {
            IndexSetReset(IndexSetOf(state->m_instance));
            state->Complete(deferred);
        }
    }
    state->m_eSpecData = nullptr;
}

// ref: FUN_1420f3290
void OnRootReadTask(void* context, Instance* instance, uint64_t opId) {
    (void)instance;
    auto state = static_cast<DynamicInitState*>(context);
    AsyncStateScope scope(state);
    state->OnRootRead(opId);
}

// ref: FUN_1420f3f30
void OnPatchIndexReadTask(void* context, Instance* instance, uint64_t opId) {
    (void)instance;
    auto state = static_cast<DynamicInitState*>(context);
    AsyncStateScope scope(state);
    state->OnPatchIndexRead(opId);
}

// ref: FUN_1420f3fb0
void OnPatchManifestReadTask(void* context, Instance* instance, uint64_t opId) {
    (void)instance;
    auto state = static_cast<DynamicInitState*>(context);
    AsyncStateScope scope(state);
    state->OnPatchManifestRead(opId);
}

// The download queue's cancel task (0x1420ecff0); streaming only.
// ref: 0x1420ecff0
void DownloadQueueCancelTask(void* argument) {
    (void)argument;
    FROST_UNIMPLEMENTED();
}

// What a missing file costs to fetch: its encoded size, unless the container has it.
uint64_t MissingSize(Instance* instance, const CONFIGRECORD& record) {
    if (!IsDefined(record)) {
        return 0;
    }
    FILEKEY key;
    record.CopyKeyTo(&key);
    if (instance->m_container->Contains(key.m_ekey)) {
        return 0;
    }
    return key.m_esize;
}

}

// --- construction ---------------------------------------------------------------------------

// ref: FUN_1420f1ad0
DynamicInitState::DynamicInitState(Instance* instance, uint64_t opId, OPCALLBACK callback, void* context)
    : AsyncState(instance, opId, callback, context) {
    this->m_stage = DI_START;
    this->m_progress = nullptr;
    this->m_progressContext = nullptr;
    this->m_progressTime = 0;
    this->m_progressStage = 6;
    this->m_patchManifestSize = 0;
    this->m_indexTotal = 0;
    this->m_done = 0;
    this->m_total = 0;
    this->m_fetchDone = 0;
    this->m_fetchTotal = 0;

    INSTANCEPARAMS& params = this->m_params;
    std::memset(&params, 0, sizeof(params));
    params.m_08 = ~uint64_t(0);
    params.m_armadilloKey = s_noTag;
    params.m_60 = 200;
    params.m_64 = 1;
    params.m_65 = 1;
    params.m_68 = 0x10;
    params.m_70 = 0x10;
    params.m_74 = 0x20;
    params.m_78 = 0x20000;
    params.m_affinity = ~uint64_t(0);
    params.m_priority = 4;
    params.m_threads = 1;
    params.m_a0 = ~uint64_t(0);
    params.m_a8 = 4;
    params.m_b0 = 0.75;
    params.m_c8 = 1;

    this->m_buildConfigPath.Init(GetBufferAllocator());
    this->m_cdnPath.Init(GetBufferAllocator());
    this->m_hostStrings.Init(GetBufferAllocator());
    this->m_hosts.Init(GetBufferAllocator());
    this->m_armadilloKey.Init(GetBufferAllocator());
    this->m_1c0.Init(GetBufferAllocator());
    this->m_pendingConfigs = 0;
    this->m_configOp = 0;
    this->m_readOp = 0;
    this->m_rootOp = 0;
    this->m_eSpecOp = 0;
    this->m_350 = 0;
    this->m_fetchOp = 0;
    this->m_patchManifestData = nullptr;
    this->m_retries = 0;
    this->m_rootReader = nullptr;
    this->m_rootData = nullptr;
    this->m_eSpecData = nullptr;
    this->m_patchIndexData = nullptr;
    this->m_error = ERR_SUCCESS;
}

// ref: FUN_1420f1eb0
DynamicInitState::~DynamicInitState() {
    GetAllocator()->deallocate(this->m_patchManifestData);
    if (this->m_rootReader) {
        this->m_rootReader->Release();
    }
    this->m_1c0.Destroy();
    this->m_armadilloKey.Destroy();
    if (this->m_hosts.m_data) {
        this->m_hosts.m_allocator->deallocate(this->m_hosts.m_data);
    }
    // FUN_1420f1e40
    if (this->m_hostStrings.m_data) {
        for (uint32_t i = 0; i < this->m_hostStrings.m_count; i++) {
            this->m_hostStrings.m_data[i].Destroy();
        }
        this->m_hostStrings.m_allocator->deallocate(this->m_hostStrings.m_data);
    }
    this->m_cdnPath.Destroy();
    this->m_buildConfigPath.Destroy();
}

// ref: FUN_1420f3310
void DynamicInitState::Destroy() {
    this->~DynamicInitState();
    PackageFree(GetBufferAllocator(), this);
}

// ref: FUN_1420f3440
uint8_t DynamicInitState::GetType() {
    return 0x19;
}

// ref: FUN_1420f3740
void DynamicInitState::OnCancel() {
    blz::log(g_logCategory, 2, s_file, 0xb5, "OnCancel for DynamicInit|Op: %llu|Stage: %u", this->m_opId,
        this->m_stage);
    switch (this->m_stage) {
        case DI_DOWNLOAD_QUEUE_WAIT:
            this->m_instance->m_scheduler->Post(&DownloadQueueCancelTask, this->m_instance->m_downloadQueue, 0);
            return;
        case DI_CONFIG:
            CancelOp(this->m_instance, this->m_configOp);
            IndexSetCancel(IndexSetOf(this->m_instance));
            return;
        case DI_INDICES:
            IndexSetCancel(IndexSetOf(this->m_instance));
            return;
        case DI_PATCH_INDEX:
        case DI_PATCH_MANIFEST:
            CancelOp(this->m_instance, this->m_readOp);
            return;
        case DI_FETCH:
            CancelOp(this->m_instance, this->m_fetchOp);
            return;
        case DI_MANIFESTS:
            CancelOp(this->m_instance, this->m_rootOp);
            CancelOp(this->m_instance, this->m_eSpecOp);
            CancelOp(this->m_instance, this->m_350);
            return;
        default:
            return;
    }
}

AsyncState* DynamicInitCreate(Instance* instance, uint64_t opId, OPCALLBACK callback, void* context) {
    void* memory = PackageAlloc(GetBufferAllocator(), sizeof(DynamicInitState), 8, false, s_noTag, 0);
    if (!memory) {
        return nullptr;
    }
    return new (memory) DynamicInitState(instance, opId, callback, context);
}

void DynamicInitStart(AsyncState* state, uint8_t error, INSTANCEPARAMS* params) {
    static_cast<DynamicInitState*>(state)->Start(error, params);
}

// --- the state machine ---------------------------------------------------------------------

void DynamicInitState::Fail(uint8_t error) {
    this->m_stage = DI_DONE;
    IndexSetReset(IndexSetOf(this->m_instance));
    this->Complete(error);
}

void DynamicInitState::Schedule(uint8_t stage) {
    this->m_stage = stage;
    this->m_instance->m_scheduler->Post(&StepTask, this, 0);
}

// Keeps copies of the caller's strings, so the parameters point into this state.
// ref: FUN_1420f24a0
void DynamicInitState::ApplyParams(INSTANCEPARAMS* params) {
    std::memcpy(&this->m_params, params, sizeof(INSTANCEPARAMS));
    if (params->m_storagePath) {
        this->m_params.m_storagePath = this->m_instance->m_storagePath.c_str();
    }
    if (params->m_buildConfigPath) {
        this->m_buildConfigPath.Assign(params->m_buildConfigPath, static_cast<uint32_t>(std::strlen(params->m_buildConfigPath)));
        this->m_params.m_buildConfigPath = this->m_buildConfigPath.c_str();
    }
    if (params->m_cdnPath) {
        this->m_cdnPath.Assign(params->m_cdnPath, static_cast<uint32_t>(std::strlen(params->m_cdnPath)));
        this->m_params.m_cdnPath = this->m_cdnPath.c_str();
    }
    if (params->m_armadilloKey) {
        this->m_armadilloKey.Assign(params->m_armadilloKey, static_cast<uint32_t>(std::strlen(params->m_armadilloKey)));
        this->m_params.m_armadilloKey = this->m_armadilloKey.c_str();
    }
    if (params->m_1c0) {
        this->m_1c0.Assign(params->m_1c0, static_cast<uint32_t>(std::strlen(params->m_1c0)));
        this->m_params.m_1c0 = this->m_1c0.c_str();
    }
    for (uint32_t i = 0; i < params->m_cdnHostCount; i++) {
        const char* host = params->m_cdnHosts[i];
        blz::allocator* allocator = GetBufferAllocator();
        // FUN_1420f50f0 moves the strings into the new block: for strings that share the
        // allocator that is the memcpy Array::Reserve does.
        this->m_hostStrings.Reserve(this->m_hostStrings.m_count + 1);
        blz::alloc_string* copy = &this->m_hostStrings.m_data[this->m_hostStrings.m_count];
        copy->Init(allocator);
        copy->Insert(0, host, static_cast<uint32_t>(std::strlen(host)));
        this->m_hostStrings.m_count++;
        // The pointer is taken into the element: an inline host name moves if the array
        // grows again, as in the reference.
        const char* text = copy->c_str();
        this->m_hosts.Reserve(this->m_hosts.m_count + 1);
        this->m_hosts.m_data[this->m_hosts.m_count++] = text;
    }
    if (this->m_hosts.m_count != 0) {
        this->m_params.m_cdnHosts = this->m_hosts.m_data;
    }
}

// ref: FUN_1420f4ab0
void DynamicInitState::Start(uint8_t error, INSTANCEPARAMS* params) {
    if (error != ERR_SUCCESS) {
        this->Fail(error);
        return;
    }
    this->ApplyParams(params);
    this->m_progress = reinterpret_cast<PROGRESSFUNCTION>(params->m_progress);
    this->m_progressContext = params->m_progressContext;
    this->m_instance->m_cdnCallbacks.m_08 = this->m_params.m_150;
    this->m_instance->m_cdnCallbacks.m_10 = this->m_params.m_158;
    this->m_instance->m_cdnCallbacks.m_18 = this->m_params.m_198;
    this->m_instance->m_cdnCallbacks.m_20 = this->m_params.m_1a0;
    this->m_instance->m_f58 = this->m_params.m_140;
    this->m_instance->m_f60 = this->m_params.m_148;
    this->Schedule(params->m_streaming == 1 ? DI_CONFIG : DI_DOWNLOAD_QUEUE);
}

// Reports at a stage change, when everything is done, or every 600 ms.
// ref: FUN_1420f2130
void DynamicInitState::ReportProgress(int32_t stage) {
    if (!this->m_progress) {
        return;
    }
    if (this->m_progressTime == 0) {
        this->m_progressTime = OsGetNanoseconds();
    }
    uint64_t extra = this->m_stage == DI_FETCH ? this->m_fetchDone : 0;
    int32_t previous = this->m_progressStage;
    this->m_progressStage = stage;
    int64_t now = OsGetNanoseconds();
    if (previous != stage || this->m_done + extra == this->m_total || now - this->m_progressTime > 599999999) {
        this->m_progressTime = now;
        this->m_progress(this->m_progressContext, this->m_instance, stage, this->m_done + extra, this->m_total);
    }
}

// ref: FUN_1420f2e40
void DynamicInitState::Step() {
    if (this->m_cancelled) {
        IndexSetReset(IndexSetOf(this->m_instance));
        this->m_stage = DI_DONE;
        this->Complete(ERR_CANCELLED);
        return;
    }
    int32_t progress;
    switch (this->m_stage) {
        case DI_START:
        case DI_DOWNLOAD_QUEUE:
        case DI_DOWNLOAD_QUEUE_WAIT:
            progress = 0;
            break;
        case DI_CONFIG:
            progress = 1;
            break;
        case DI_CONTAINER:
        case DI_SIZES:
            progress = 2;
            break;
        case DI_INDICES:
        case DI_PATCH_INDEX:
        case DI_PATCH_MANIFEST:
        case DI_FETCH:
            progress = 3;
            break;
        case DI_MANIFESTS:
            progress = 4;
            break;
        default:
            progress = 5;
            break;
    }
    this->ReportProgress(progress);

    Instance* instance = this->m_instance;
    switch (this->m_stage) {
        case DI_DOWNLOAD_QUEUE:
            this->m_stage = DI_DOWNLOAD_QUEUE_WAIT;
            DownloadQueueStart(instance->m_downloadQueue, &this->m_params, &OnDownloadQueueReady, this);
            return;
        case DI_CONFIG:
            this->m_error = ERR_SUCCESS;
            this->m_pendingConfigs = DownloadQueueStreaming(instance) ? 2 : 1;
            this->ReadConfigs();
            if (DownloadQueueStreaming(this->m_instance)) {
                IndexSetFetch(IndexSetOf(this->m_instance), this->m_instance, this->m_params.m_cdnKey, &OnCdnConfig, this, 2);
            }
            return;
        case DI_CONTAINER: {
            uint8_t error = instance->InitializeContainer(this->m_params.m_storagePath, this->m_params.m_streaming,
                this->m_params.m_60, this->m_params.m_68, this->m_params.m_6c, this->m_params.m_01,
                this->m_params.m_08, this->m_params.m_1d9);
            if (error == ERR_SUCCESS) {
                this->Schedule(DI_SIZES);
                return;
            }
            this->Fail(error);
            return;
        }
        case DI_SIZES:
            this->MeasureSizes();
            return;
        case DI_INDICES:
            if (DownloadQueueStreaming(instance)) {
                IndexSetFetch(IndexSetOf(instance), instance, this->m_params.m_cdnKey, &OnIndicesFetched, this, 0xb);
                return;
            }
            this->Schedule(DI_FETCH);
            return;
        case DI_PATCH_INDEX:
            instance->m_readQueue.SetDepth(this->m_params.m_70);
            this->m_instance->m_memoryLimit = MemoryLimitCreate(this->m_params.m_78);
            if (this->m_instance->m_buildConfig.m_records[CONFIG_PATCH_INDEX].m_esize != 0) {
                this->ReadPatchIndex();
                return;
            }
            if (this->m_instance->m_buildConfig.m_patchSize != 0) {
                this->m_patchManifestData = static_cast<uint8_t*>(
                    PackageAlloc(GetAllocator(), this->m_instance->m_buildConfig.m_patchSize, 0x10, true, s_noTag, 0));
                this->Schedule(DI_PATCH_MANIFEST);
                return;
            }
            this->Schedule(DI_MANIFESTS);
            return;
        case DI_PATCH_MANIFEST:
            this->ReadPatchManifest();
            return;
        case DI_FETCH:
            this->FetchIndices();
            return;
        case DI_MANIFESTS:
            this->ReadManifests();
            return;
        case DI_DONE:
            instance->m_initialized = 1;
            this->Fail(ERR_SUCCESS);
            return;
        default:
            return;
    }
}

// Starts the build configuration read (and with streaming, the CDN configuration fetch).
// ref: FUN_1420f4030
void DynamicInitState::ReadConfigs() {
    Instance* instance = this->m_instance;
    AcquireSRWLockExclusive(&instance->m_ops.m_lock);
    uint64_t id = instance->m_ops.m_nextId++;
    Instance* owner = instance->m_ops.m_instance;
    AsyncState* state = nullptr;
    void* memory = PackageAlloc(GetBufferAllocator(), 0xD0, 8, false, s_noTag, 0);
    if (memory) {
        state = ConfigReadCreate(memory, owner, id, &OnBuildConfig, this);
        if (state) {
            state->AddRef();
        }
    }
    instance->m_ops.Register(state);
    int64_t* lock = SpinLockExclusive(&state->m_lock);
    state->AddRef();
    state->Release();
    ReleaseSRWLockExclusive(&instance->m_ops.m_lock);
    this->m_configOp = state->m_opId;
    ConfigReadStart(state, 0, this->m_params.m_buildConfigPath, this->m_params.m_storagePath,
        &this->m_instance->m_buildConfig, this->m_params.m_buildKey, 1, 0);
    if (lock) {
        *lock = 0;
    }
    state->Release();
}

// Totals what initialization will have to download, for progress; only with streaming and a
// progress callback.
// ref: FUN_1420f3450
void DynamicInitState::MeasureSizes() {
    Instance* instance = this->m_instance;
    if (this->m_progress && DownloadQueueStreaming(instance)) {
        IndexSetMeasure(IndexSetOf(instance), &OnIndexProgress, this);
        BUILDCONFIG& config = this->m_instance->m_buildConfig;
        CONFIGRECORD* record = config.m_vfs.m_data;
        CONFIGRECORD* end = record + config.m_vfs.m_count;
        for (; record != end; record++) {
            if (!this->m_instance->m_container->Contains(record->m_ekey)) {
                this->m_total += record->m_esize;
            }
        }
        if (!IsDefined(config.m_records[CONFIG_PATCH_INDEX])) {
            if (!this->m_instance->m_container->Contains(config.m_patchKey)) {
                this->m_patchManifestSize = this->m_instance->m_buildConfig.m_patchSize;
            }
        } else {
            this->m_patchManifestSize = MissingSize(this->m_instance, config.m_records[CONFIG_ENCODING]);
        }
        this->m_total += this->m_patchManifestSize;
        if (this->m_params.m_1b8 || this->m_params.m_loadEncodingTable) {
            this->m_total += MissingSize(this->m_instance, this->m_instance->m_buildConfig.m_records[CONFIG_ENCODING]);
        }
        if (this->m_params.m_1b8) {
            this->m_total += MissingSize(this->m_instance, this->m_instance->m_buildConfig.m_records[1]);
            this->m_total += MissingSize(this->m_instance, this->m_instance->m_buildConfig.m_records[0]);
        }
    }
    this->Schedule(DI_INDICES);
}

// Fetches the files initialization needs from the CDN; only with streaming.
// ref: FUN_1420f41d0
void DynamicInitState::FetchIndices() {
    Instance* instance = this->m_instance;
    if (!DownloadQueueStreaming(instance) || !this->m_params.m_1b8) {
        this->Schedule(DI_PATCH_INDEX);
        return;
    }
    // The keys: every record of the configuration's list, then the encoding table and the two
    // first records when the container lacks them. The reference keeps them in a vector with
    // 0x80 inline entries; this one is heap-backed from the same allocator.
    Array<uint8_t> keys;
    keys.Init(GetBufferAllocator());
    uint32_t count = 0;
    auto push = [&](const uint8_t key[16]) {
        keys.Reserve((count + 1) * 16);
        std::memcpy(keys.m_data + count * 16, key, 16);
        count++;
        keys.m_count = count * 16;
    };
    BUILDCONFIG& config = this->m_instance->m_buildConfig;
    for (uint32_t i = 0; i < config.m_vfs.m_count; i++) {
        push(config.m_vfs.m_data[i].m_ekey);
    }
    const int32_t extra[] = { CONFIG_ENCODING, 1, 0 };
    for (int32_t index : extra) {
        CONFIGRECORD& record = this->m_instance->m_buildConfig.m_records[index];
        if (IsDefined(record) && !this->m_instance->m_container->Contains(record.m_ekey)) {
            push(record.m_ekey);
        }
    }

    instance = this->m_instance;
    AcquireSRWLockExclusive(&instance->m_ops.m_lock);
    uint64_t id = instance->m_ops.m_nextId++;
    Instance* owner = instance->m_ops.m_instance;
    AsyncState* state = nullptr;
    void* memory = PackageAlloc(GetBufferAllocator(), 0x500, 8, false, s_noTag, 0);
    if (memory) {
        state = FetchStateCreate(memory, owner, id, &OnFetched, this);
        if (state) {
            state->AddRef();
        }
    }
    instance->m_ops.Register(state);
    int64_t* lock = SpinLockExclusive(&state->m_lock);
    state->AddRef();
    state->Release();
    ReleaseSRWLockExclusive(&instance->m_ops.m_lock);
    this->m_fetchOp = state->m_opId;
    FetchStateStart(state, keys.m_data, count, 0x10, 5, 0, 0x80000000, &OnFetchProgress, this, 0, 0);
    if (lock) {
        *lock = 0;
    }
    state->Release();
    keys.Free();
}

// ref: FUN_1420f4bc0
void DynamicInitState::ReadPatchIndex() {
    FILEKEY key;
    this->m_instance->m_buildConfig.m_records[CONFIG_PATCH_INDEX].CopyKeyTo(&key);
    uint32_t size = key.m_esize < key.m_csize ? key.m_csize : key.m_esize;
    this->m_patchIndexData = static_cast<uint8_t*>(this->m_instance->m_scratch.Allocate(size));
    key.m_eSpec = s_eSpecNone;
    key.m_csize = key.m_esize;
    OPHANDLE handle;
    CreateFileRead(this->m_instance, &handle, &OnPatchIndexReadTask, this);
    this->m_readOp = handle.m_state->m_opId;
    FileReadSetFlag10(handle.m_state);
    FileReadStartWhole(handle.m_state, &key, this->m_patchIndexData, key.m_csize, 2, 2);
    if (handle.m_lock) {
        *handle.m_lock = 0;
    }
    handle.m_state->Release();
}

// The patch index arrives raw ("n"): decode it in place, through a buffer the size of its
// content.
// ref: FUN_1420f27f0
uint8_t DynamicInitState::DecodePatchIndex() {
    FILEKEY key;
    this->m_instance->m_buildConfig.m_records[CONFIG_PATCH_INDEX].CopyKeyTo(&key);
    auto buffer = static_cast<uint8_t*>(PackageAlloc(GetBufferAllocator(), key.m_csize, 0x10, false, s_noTag, 0));
    uint8_t error = ERR_SUCCESS;
    if (!CodecIsEncoded(this->m_patchIndexData, key.m_esize, &error)) {
        error = CodecDecodeRaw(buffer, key.m_csize, this->m_patchIndexData, key.m_esize, key.m_ekey, s_noTag, 0x10, 0, 0,
            0, 1);
    } else {
        Instance* instance = this->m_instance;
        error = CodecDecodeEncoded(buffer, key.m_csize, this->m_patchIndexData + 8, key.m_esize - 8, 0, key.m_ekey,
            key.m_eSpecFlags, 0, reinterpret_cast<KEYGETTER>(instance->m_f58), instance->m_f60, 0, &instance->m_scratch,
            static_cast<DECODERHOOK*>(instance->m_memoryLimit));
    }
    if (error != ERR_SUCCESS) {
        char text[0x21];
        KeyToString(text, sizeof(text), this->m_instance->m_buildConfig.m_records[CONFIG_PATCH_INDEX].m_ekey);
        blz::log(g_logCategory, 3, s_file, 0x2fa,
            "Failed to decode the patch index. Patching will be disabled. Error (%u): %s", error, ErrorString(error));
        error = this->m_instance->m_container->Remove(this->m_instance->m_buildConfig.m_records[CONFIG_PATCH_INDEX].m_ekey);
        if (error != ERR_SUCCESS) {
            blz::log(g_logCategory, 4, s_file, 0x300,
                "Failed to remove patch index from container, using key: %s. Error (%u): %s", text, error,
                ErrorString(error));
        }
    }
    std::memcpy(this->m_patchIndexData, buffer, key.m_csize);
    if (buffer) {
        PackageFree(GetBufferAllocator(), buffer);
    }
    return error;
}

// ref: FUN_1420f3af0
void DynamicInitState::OnPatchIndexRead(uint64_t opId) {
    Instance* instance = this->m_instance;
    this->m_readOp = 0;
    uint8_t error = InstanceGetOpResult(instance, opId, nullptr);
    if (error == ERR_SUCCESS) {
        if (this->DecodePatchIndex() != ERR_SUCCESS) {
            this->Schedule(DI_MANIFESTS);
            this->m_instance->m_scratch.Free(this->m_patchIndexData);
            return;
        }
        PATCHINDEXSOURCE source;
        source.m_instance = this->m_instance;
        source.m_data = this->m_patchIndexData;
        source.m_size = this->m_instance->m_buildConfig.m_records[CONFIG_PATCH_INDEX].m_csize;
        source.m_258 = this->m_params.m_1c0;
        source.m_260 = this->m_params.m_1c8;
        source.m_268 = this->m_params.m_1d0;
        error = this->m_instance->m_patchIndex.Load(&source);
        if (error == ERR_SUCCESS) {
            this->m_done += this->m_patchManifestSize;
            this->ReportProgress(3);
            this->Schedule(DI_MANIFESTS);
            this->m_instance->m_scratch.Free(this->m_patchIndexData);
            return;
        }
    }
    const uint8_t* ekey = this->m_instance->m_buildConfig.m_records[CONFIG_PATCH_INDEX].m_ekey;
    char text[0x21];
    KeyToString(text, sizeof(text), ekey);
    blz::log(g_logCategory, 3, s_file, 0x2ba, "Failed to read patch index using key: %s. Error (%u): %s", text, error,
        ErrorString(error));
    uint32_t removeError = 0;
    uint8_t removed = this->m_instance->m_container->Remove(ekey);
    if (removed != ERR_SUCCESS) {
        removeError = removed;
        blz::log(g_logCategory, 4, s_file, 0x2c1,
            "Failed to remove patch index from container, using key: %s. Error (%u): %s", text, removeError,
            ErrorString(removed));
    }
    if (++this->m_retries > 2) {
        blz::log(g_logCategory, 3, s_file, 0x2c8,
            "Failed to initialize Patch Index, patching will be disabled. Error(%u) : %s", removeError,
            ErrorString(removed));
        this->m_stage = DI_MANIFESTS;
    }
    this->m_instance->m_scheduler->Post(&StepTask, this, 0);
    this->m_instance->m_scratch.Free(this->m_patchIndexData);
}

// ref: FUN_1420f4d50
void DynamicInitState::ReadPatchManifest() {
    BUILDCONFIG& config = this->m_instance->m_buildConfig;
    FILEKEY key;
    std::memcpy(key.m_ckey, config.m_patchKey, 16);
    key.m_csize = config.m_patchSize;
    std::memcpy(key.m_ekey, config.m_patchKey, 16);
    key.m_esize = config.m_patchSize;
    key.m_eSpec = s_eSpecNone;
    key.m_eSpecFlags = 0x10;
    key.m_31 = 0;
    key.m_38 = 0;
    OPHANDLE handle;
    CreateFileRead(this->m_instance, &handle, &OnPatchManifestReadTask, this);
    FileReadSetFlag11(handle.m_state);
    this->m_readOp = handle.m_state->m_opId;
    FileReadStart(handle.m_state, &key, this->m_patchManifestData, 0, this->m_instance->m_buildConfig.m_patchSize, 2, 2);
    if (handle.m_lock) {
        *handle.m_lock = 0;
    }
    handle.m_state->Release();
}

// ref: FUN_1420f3d70
void DynamicInitState::OnPatchManifestRead(uint64_t opId) {
    this->m_readOp = 0;
    uint8_t error = InstanceGetOpResult(this->m_instance, opId, nullptr);
    if (error == ERR_SUCCESS) {
        Instance* instance = this->m_instance;
        error = PatchManifestParse(instance->m_patchManifest, this->m_patchManifestData, instance->m_buildConfig.m_patchKey,
            instance->m_buildConfig.m_patchSize);
        if (error == ERR_SUCCESS) {
            this->m_done += this->m_patchManifestSize;
            this->ReportProgress(3);
            this->m_patchManifestData = nullptr;
            this->Schedule(DI_MANIFESTS);
            return;
        }
    }
    char text[0x21];
    KeyToString(text, sizeof(text), this->m_instance->m_buildConfig.m_patchKey);
    blz::log(g_logCategory, 3, s_file, 0x34b, "Failed to read patch manifest using key: %s. Error (%u): %s", text, error,
        ErrorString(error));
    uint32_t removeError = 0;
    uint8_t removed = this->m_instance->m_container->Remove(this->m_instance->m_buildConfig.m_patchKey);
    if (removed != ERR_SUCCESS) {
        removeError = removed;
        blz::log(g_logCategory, 4, s_file, 0x352,
            "Failed to remove patch manifest from container, using key: %s. Error (%u): %s", text, removeError,
            ErrorString(removed));
    }
    if (++this->m_retries < 3) {
        this->m_instance->m_scheduler->Post(&StepTask, this, 0);
        return;
    }
    blz::log(g_logCategory, 3, s_file, 0x359,
        "Failed to initialize Patch Manifest, patching will be disabled. Error(%u) : %s", removeError,
        ErrorString(removed));
    this->Schedule(DI_MANIFESTS);
}

// The root manifest, then the encoding table (for eSpec look-ups) when asked for.
// ref: FUN_1420f4ed0
void DynamicInitState::ReadManifests() {
    this->m_error = ERR_SUCCESS;
    if (!IsDefined(this->m_instance->m_buildConfig.m_records[CONFIG_ROOT])) {
        blz::log(g_logCategory, 2, s_file, 0x36e, "Root manifest was not defined. No root manifest will be loaded.");
    } else {
        this->ReadRoot();
    }
    CONFIGRECORD& encoding = this->m_instance->m_buildConfig.m_records[CONFIG_ENCODING];
    if (IsDefined(encoding)) {
        if (this->m_params.m_loadEncodingTable && encoding.m_csize != 0 && encoding.m_esize != 0) {
            FILEKEY key;
            encoding.CopyKeyTo(&key);
            this->m_eSpecData = this->m_instance->m_encodingTable.Allocate(key.m_csize);
            OPHANDLE handle;
            CreateFileRead(this->m_instance, &handle, &OnESpecRead, this);
            this->m_eSpecOp = handle.m_state->m_opId;
            FileReadStartWhole(handle.m_state, &key, this->m_eSpecData, key.m_csize, 2, 0);
            if (handle.m_lock) {
                *handle.m_lock = 0;
            }
            handle.m_state->Release();
        }
    } else {
        blz::log(g_logCategory, 2, s_file, 0x377, "Encoding table was not defined.");
    }
    if (this->m_eSpecOp == 0 && this->m_rootOp == 0 && this->m_350 == 0) {
        this->Schedule(DI_DONE);
    }
}

// ref: FUN_1420f48f0
void DynamicInitState::ReadRoot() {
    FILEKEY key;
    this->m_instance->m_buildConfig.m_records[CONFIG_ROOT].CopyKeyTo(&key);
    RootReader* reader = nullptr;
    RootReader** created = RootReaderCreate(&reader, key.m_csize, &this->m_rootData);
    if (this->m_rootReader) {
        this->m_rootReader->Release();
    }
    this->m_rootReader = *created;
    *created = nullptr;
    if (reader) {
        reader->Release();
    }
    OPHANDLE handle;
    CreateFileRead(this->m_instance, &handle, &OnRootReadTask, this);
    this->m_rootOp = handle.m_state->m_opId;
    FileReadStart(handle.m_state, &key, this->m_rootData, 0, key.m_csize, 2, 0);
    if (handle.m_lock) {
        *handle.m_lock = 0;
    }
    handle.m_state->Release();
}

// ref: FUN_1420f3870
void DynamicInitState::OnRootRead(uint64_t opId) {
    this->m_rootOp = 0;
    uint8_t error = InstanceGetOpResult(this->m_instance, opId, nullptr);
    Instance* instance = this->m_instance;
    char text[0x21];
    if (error != ERR_SUCCESS) {
        KeyToString(text, sizeof(text), instance->m_buildConfig.m_records[CONFIG_ROOT].m_ekey);
        blz::log(g_logCategory, 4, s_file, 0x3a3, "Failed to read file manifest at initialization: %s. Error (%u): %s",
            text, error, ErrorString(error));
        if (this->m_eSpecOp != 0 || this->m_rootOp != 0 || this->m_350 != 0) {
            this->m_error = error;
            return;
        }
        this->Fail(error);
        return;
    }
    RECORDLIST records;
    records.m_records = instance->m_buildConfig.m_vfs.m_data;
    records.m_count = instance->m_buildConfig.m_vfs.m_count;
    error = instance->m_fileManifest.Load(&instance->m_buildConfig.m_records[CONFIG_ROOT], records.m_records,
        records.m_count, this->m_rootData, &this->m_rootReader);
    if (error != ERR_SUCCESS) {
        KeyToString(text, sizeof(text), this->m_instance->m_buildConfig.m_records[CONFIG_ROOT].m_ekey);
        blz::log(g_logCategory, 4, s_file, 0x3b9,
            "Failed to initialize file manifest at initialization: %s. Error (%u): %s", text, error, ErrorString(error));
        if (this->m_eSpecOp == 0 && this->m_rootOp == 0 && this->m_350 == 0) {
            this->Fail(ERR_DATA);
            return;
        }
        this->m_error = ERR_DATA;
        return;
    }
    if (this->m_eSpecOp == 0 && this->m_rootOp == 0 && this->m_350 == 0) {
        uint8_t deferred = this->m_error;
        this->m_stage = DI_DONE;
        if (deferred != ERR_SUCCESS) {
            this->Fail(deferred);
            return;
        }
        this->m_instance->m_scheduler->Post(&StepTask, this, 0);
    }
}

}
