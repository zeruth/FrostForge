#include "tact/AsyncState.hpp"
#include "tact/Codec.hpp"
#include "tact/Common.hpp"
#include "tact/Container.hpp"
#include "tact/FileHandle.hpp"
#include "tact/HeaderCache.hpp"
#include "tact/Instance.hpp"
#include "tact/ReadQueue.hpp"
#include "tact/TaskScheduler.hpp"
#include "blz/Log.hpp"
#include <cstddef>
#include <new>

// pkg/tact/3.6.39/src/async_state/async_file_span_resident_state.cpp: whether a span of a
// file is in local storage. A whole file asks the container for its key; a span of an
// unencoded file asks for the span; a span of a block-encoded file has to map the span onto
// the blocks it falls in, through the file's encoding header.

namespace tact {

namespace {

const char s_file[] =
    "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\async_state\\async_file_span_resident_state.cpp";
const char s_instanceFile[] = "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\instance.cpp";
const char s_noTag[] = "";  // DAT_1426178ba
// The eSpec a raw read of the header asks for (DAT_1426cc7c8).
const char s_eSpecNone[] = "n";

enum : uint8_t {
    STAGE_CHECK = 1,
    STAGE_HEADER = 2,   // reading the encoding header
    STAGE_SPAN = 3,     // mapping the span through the header
    STAGE_DONE = 4,
};

// 0xa8 bytes, vtable 0x1428e4cd8.
class SpanResidentState : public AsyncState {
    public:
    FILEKEY m_key;                  // 0x40
    uint32_t m_offset;              // 0x80
    uint32_t m_size;                // 0x84
    HEADERCACHEVALUE* m_header;     // 0x88: the cached encoding header (reference counted)
    uint32_t m_headerSize;          // 0x90
    void* m_scratch;                // 0x98
    uint8_t m_stage;                // 0xa0

    SpanResidentState(Instance* instance, uint64_t opId, OPCALLBACK callback, void* context);
    ~SpanResidentState() override;

    void Destroy() override;
    uint8_t GetType() override;

    void Finish(uint8_t result);
    uint8_t Check(uint8_t* waiting);
    uint8_t CheckSpan();
    void OnHeader(uint8_t result);
    void Start(const FILEKEY* key, uint32_t offset, uint32_t size);
    void StartByName(const char* path, uint32_t offset, uint32_t size);
};
static_assert(offsetof(SpanResidentState, m_key) == 0x40, "SpanResidentState layout");
static_assert(offsetof(SpanResidentState, m_offset) == 0x80, "SpanResidentState layout");
static_assert(offsetof(SpanResidentState, m_scratch) == 0x98, "SpanResidentState layout");
static_assert(sizeof(SpanResidentState) == 0xa8, "SpanResidentState is 0xa8 bytes");

void StepTask(void* argument);
void HeaderRead(void* context, uint8_t result);

SpanResidentState::SpanResidentState(Instance* instance, uint64_t opId, OPCALLBACK callback, void* context)
    : AsyncState(instance, opId, callback, context) {
    this->m_header = nullptr;
    this->m_headerSize = 0;
    this->m_scratch = nullptr;
    this->m_stage = 0;
}

// ref: FUN_1420d4100
SpanResidentState::~SpanResidentState() {
    if (this->m_header != nullptr) {
        this->m_header->Release();
    }
}

// ref: FUN_1420d4200
void SpanResidentState::Destroy() {
    this->~SpanResidentState();
    PackageFree(GetBufferAllocator(), this);
}

// ref: FUN_1406a4ce0
uint8_t SpanResidentState::GetType() {
    return 8;
}

// The common ending: done, the scratch block back, and the result.
// ref: FUN_142107f60
void SpanResidentState::Finish(uint8_t result) {
    this->m_stage = STAGE_DONE;
    if (this->m_scratch != nullptr) {
        this->m_instance->m_scratch.Free(this->m_scratch);
        this->m_scratch = nullptr;
    }
    this->Complete(result);
}

// The span through the encoding header onto the stored blocks, then whether those are local.
// ref: FUN_1420f1390
uint8_t SpanResidentState::CheckSpan() {
    uint32_t cSpan[2] = { this->m_offset, this->m_size };
    uint32_t eSpan[2] = { 0, 0 };
    if (EncodingHeaderMapSpan(this->m_header->m_header, cSpan, eSpan, nullptr) != 0) {
        char text[0x30];
        KeyToString(text, 0x21, this->m_key.m_ekey);
        blz::log(g_logCategory, 4, s_file, 0x100, "Failed to translate requested span to on-disk file span. Key: %s",
            text);
        return ERR_INVALID_PARAMETER;
    }
    bool resident = this->m_instance->m_container->GetSpan(this->m_key.m_ekey, eSpan);
    this->m_stage = STAGE_DONE;
    return resident ? ERR_SUCCESS : ERR_NOT_AVAILABLE;
}

// ref: FUN_1420f1470
uint8_t SpanResidentState::Check(uint8_t* waiting) {
    *waiting = 0;
    Instance* instance = this->m_instance;
    if (this->m_offset == 0 && this->m_size == this->m_key.m_csize) {
        this->m_stage = STAGE_DONE;
        return instance->m_container->Contains(this->m_key.m_ekey) ? ERR_SUCCESS : ERR_NOT_AVAILABLE;
    }
    const char* spec = this->m_key.m_eSpec;
    if (spec != nullptr && *spec != '\0') {
        uint8_t type = ESpecType(spec, nullptr);
        if (type != ESPEC_BLOCKS && type != ESPEC_NONE) {
            char text[0x30];
            KeyToString(text, 0x21, this->m_key.m_ekey);
            blz::log(g_logCategory, 4, s_file, 0x98, "Attempted span residency check on non-block encoded file. Key: %s",
                text);
            return ERR_INVALID_PARAMETER;
        }
        if (type == ESPEC_NONE) {
            this->m_stage = STAGE_DONE;
            uint32_t span[2] = { this->m_offset, this->m_size };
            return instance->m_container->GetSpan(this->m_key.m_ekey, span) ? ERR_SUCCESS : ERR_NOT_AVAILABLE;
        }
    }
    // A block-encoded file: its encoding header from the instance's cache, or read raw from the
    // container into a scratch block and parsed when it lands (HeaderRead).
    HEADERCACHE* cache = reinterpret_cast<HEADERCACHE*>(instance->m_a28);
    HEADERCACHEVALUE* cached = cache->Find(this->m_key.m_ekey);
    if (this->m_header != nullptr) {
        this->m_header->Release();
    }
    this->m_header = cached;
    if (this->m_header != nullptr) {
        this->m_stage = STAGE_SPAN;
        return this->CheckSpan();
    }
    uint8_t result = ESpecEncodingHeaderSize(this->m_key.m_eSpec, this->m_key.m_csize, &this->m_headerSize);
    if (result != ERR_SUCCESS) {
        return result;
    }
    this->m_scratch = instance->m_scratch.Allocate(this->m_headerSize);
    this->m_stage = STAGE_HEADER;
    uint64_t cSpan = static_cast<uint64_t>(this->m_headerSize) << 32;
    uint64_t eSpan = static_cast<uint64_t>(this->m_headerSize) << 32;
    instance->m_readQueue.Submit(this->m_key.m_ekey, this->m_key.m_eSpecFlags, s_eSpecNone, 1, nullptr, &eSpan, &cSpan,
        static_cast<uint8_t*>(this->m_scratch), 1, 2, 2, &HeaderRead, this, nullptr, nullptr);
    *waiting = 1;
    return ERR_SUCCESS;
}

// The header's bytes are in: parsed and cached, then the span mapped on the scheduler; or the
// check ends with the read's error.
// ref: FUN_1420f11a0
void SpanResidentState::OnHeader(uint8_t result) {
    if (result == ERR_SUCCESS) {
        uint32_t headerSize = 0;
        ENCODINGHEADER* header = EncodingHeaderCreate(this->m_key.m_ekey, this->m_key.m_eSpecFlags,
            static_cast<const uint8_t*>(this->m_scratch), this->m_headerSize, &headerSize, &result);
        if (header != nullptr) {
            HEADERCACHE* cache = reinterpret_cast<HEADERCACHE*>(this->m_instance->m_a28);
            HEADERCACHEVALUE* value = cache->Insert(this->m_key.m_ekey, header);
            if (this->m_header != nullptr) {
                this->m_header->Release();
            }
            this->m_header = value;
            this->m_stage = STAGE_SPAN;
            this->m_instance->m_scheduler->Post(&StepTask, this, 0);
            return;
        }
    }
    this->m_stage = STAGE_DONE;
    if (this->m_scratch != nullptr) {
        this->m_instance->m_scratch.Free(this->m_scratch);
        this->m_scratch = nullptr;
    }
    this->Complete(result);
}

// The read queue's callback.
// ref: FUN_1420f1310
void HeaderRead(void* context, uint8_t result) {
    auto state = static_cast<SpanResidentState*>(context);
    AsyncStateScope scope(state);
    state->OnHeader(result);
}

// ref: FUN_1420f1000
void StepTask(void* argument) {
    auto state = static_cast<SpanResidentState*>(argument);
    AsyncStateScope scope(state);
    if (state->m_cancelled) {
        state->Finish(ERR_CANCELLED);
        return;
    }
    uint8_t result = ERR_UNKNOWN;
    uint8_t waiting = 0;
    if (state->m_stage == STAGE_CHECK) {
        result = state->Check(&waiting);
    } else if (state->m_stage == STAGE_SPAN) {
        result = state->CheckSpan();
    } else {
        state->Finish(result);
        return;
    }
    if (result != ERR_SUCCESS) {
        state->Finish(result);
        return;
    }
    if (waiting) {
        return;
    }
    if (state->m_stage == STAGE_DONE) {
        state->Finish(ERR_SUCCESS);
    } else {
        state->m_instance->m_scheduler->Post(&StepTask, state, 0);
    }
}

// ref: FUN_1420f1780
void SpanResidentState::Start(const FILEKEY* key, uint32_t offset, uint32_t size) {
    uint32_t total = key->m_csize;
    if (total <= offset) {
        char text[0x30];
        KeyToString(text, 0x21, key->m_ekey);
        blz::log(g_logCategory, 4, s_file, 0x34, "Invalid offset specified for file. Key: %s, Offset: %u, FileSize: %u",
            text, offset, key->m_csize);
        this->Finish(ERR_INVALID_PARAMETER);
        return;
    }
    if (size == 0 || total < offset + size) {
        size = total - offset;
    }
    this->m_key = *key;
    this->m_size = size;
    this->m_offset = offset;
    this->m_stage = STAGE_CHECK;
    this->m_instance->m_scheduler->Post(&StepTask, this, 0);
}

// ref: FUN_1420f18c0
void SpanResidentState::StartByName(const char* path, uint32_t offset, uint32_t size) {
    if (path != nullptr) {
        FILEKEY key;
        if (this->m_instance->m_fileManifest.Lookup(path, &key, 1) == ERR_SUCCESS) {
            this->Start(&key, offset, size);
            return;
        }
        blz::log(g_logCategory, 4, s_file, 0x15, "Invalid path specified for file. Path: %s", path);
    }
    this->Finish(ERR_INVALID_PARAMETER);
}

// Registers a new span residency check with the instance; the handle keeps a reference and
// the lock.
// ref: FUN_1420d3a80
OPHANDLE* CreateSpanResident(Instance* instance, OPHANDLE* out, OPCALLBACK callback, void* context) {
    OPREGISTRY& ops = instance->m_ops;
    AcquireSRWLockExclusive(&ops.m_lock);
    uint64_t id = ops.m_nextId++;
    Instance* owner = ops.m_instance;
    AsyncState* state = nullptr;
    void* memory = PackageAlloc(GetBufferAllocator(), sizeof(SpanResidentState), 8, false, s_noTag, 0);
    if (memory != nullptr) {
        state = new (memory) SpanResidentState(owner, id, callback, context);
        state->AddRef();
    }
    ops.Register(state);
    int64_t* lock = SpinLockExclusive(&state->m_lock);
    out->m_state = state;
    out->m_lock = lock;
    if (state != nullptr) {
        state->AddRef();
        state->Release();
    }
    ReleaseSRWLockExclusive(&ops.m_lock);
    return out;
}

}

// ref: FUN_1420d4df0
uint64_t InstanceCheckResidency(Instance* instance, const char* path, uint32_t offset, uint32_t size) {
    if (instance == nullptr || !instance->m_initialized) {
        blz::log(g_logCategory, 4, s_instanceFile, 0x4e1, "Unable to check file residency. Invalid tact instance.");
        return 0;  // DAT_1428e4ca0
    }
    OPHANDLE handle;
    CreateSpanResident(instance, &handle, nullptr, nullptr);
    auto state = static_cast<SpanResidentState*>(handle.m_state);
    state->StartByName(path, offset, size);
    uint64_t id = state->m_opId;
    if (handle.m_lock != nullptr) {
        *handle.m_lock = 0;
    }
    state->Release();
    return id;
}

}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_1420f1310 = (void*)&tact::HeaderRead;
// ---- end of generated globals ----
