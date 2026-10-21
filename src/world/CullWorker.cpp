#include "world/CullWorker.hpp"

#include "model2/M2Shared.hpp"
#include "threadpool/ThreadPool.hpp"
#include "util/Unimplemented.hpp"
#include "world/WorldMap.hpp"
#include "world/WorldScene.hpp"
#include "world/WorldView.hpp"
#include <cfloat>
#include <cstring>
#include <new>

namespace {

const char s_cullFile[] = "D:\\BuildServer\\A\\work-git\\wow\\WoW\\Source\\WorldClient2\\CullWorker.cpp";

template <class T>
void Store(void* base, size_t offset, T value) {
    std::memcpy(static_cast<uint8_t*>(base) + offset, &value, sizeof(T));
}

void StoreIdentity(void* base, size_t offset) {
    for (int32_t i = 0; i < 16; i++) {
        Store<float>(base, offset + i * 4, (i % 5) == 0 ? 1.0f : 0.0f);
    }
}

// The cull pass's scratch block (0x460, FUN_141269fd0).
// ref: FUN_141269fd0
void* CullScratchInit(void* scratch) {
    Store<uint32_t>(scratch, 0x380, 0x1f);
    Store<uint64_t>(scratch, 0x448, 0);
    Store<uint64_t>(scratch, 0x450, 0);
    Store<uint32_t>(scratch, 0x444, 0);
    Store<uint64_t>(scratch, 0x458, 0);
    return scratch;
}

// The detail doodad result's two words (FUN_14144d510).
// ref: FUN_14144d510
void CullResult108Clear(uint8_t* result) {
    Store<uint64_t>(result, 0x200, 0);
    Store<uint32_t>(result, 0x208, 0);
}

}  // namespace

// ---- the result and state blocks -----------------------------------------------------------

// ref: FUN_14144cda0
CULLRESULT::CULLRESULT() {
    std::memset(this->m_data, 0, sizeof(this->m_data));
    Store<uint32_t>(this->m_data, 0x04, 3);
    for (size_t offset = 0x08; offset <= 0x1c; offset += 4) {
        Store<float>(this->m_data, offset, FLT_MAX);
    }
    Store<float>(this->m_data, 0x24, FLT_MAX);
    Store<float>(this->m_data, 0x28, FLT_MAX);
    Store<float>(this->m_data, 0x2c, -FLT_MAX);
    Store<float>(this->m_data, 0x30, -FLT_MAX);
    Store<float>(this->m_data, 0x50, FLT_MAX);
    Store<float>(this->m_data, 0x54, -FLT_MAX);
    Store<float>(this->m_data, 0x60, FLT_MAX);
    Store<float>(this->m_data, 0x64, FLT_MAX);
    Store<float>(this->m_data, 0x68, -FLT_MAX);
    Store<float>(this->m_data, 0x6c, -FLT_MAX);
    Store<float>(this->m_data, 0x8c, FLT_MAX);
    Store<float>(this->m_data, 0x90, -FLT_MAX);
}

// ref: FUN_14141eef0
CULLSTATE::CULLSTATE() {
    std::memset(this->m_data, 0, sizeof(this->m_data));
    // Six (1, 0, 0, 0) rows from +0x18 and one more at +0xd8.
    for (size_t offset = 0x18; offset <= 0x68; offset += 0x10) {
        Store<float>(this->m_data, offset, 1.0f);
    }
    Store<float>(this->m_data, 0xd8, 1.0f);
    // Three 4x4 identities at +0x11c, +0x15c and +0x19c.
    Store<float>(this->m_data, 0x11c, 1.0f);
    Store<float>(this->m_data, 0x130, 1.0f);
    Store<float>(this->m_data, 0x144, 1.0f);
    Store<float>(this->m_data, 0x158, 1.0f);
    Store<float>(this->m_data, 0x15c, 1.0f);
    Store<float>(this->m_data, 0x170, 1.0f);
    Store<float>(this->m_data, 0x184, 1.0f);
    Store<float>(this->m_data, 0x198, 1.0f);
    Store<float>(this->m_data, 0x19c, 1.0f);
    Store<float>(this->m_data, 0x1b0, 1.0f);
    Store<float>(this->m_data, 0x1c4, 1.0f);
    Store<float>(this->m_data, 0x1d8, 1.0f);
}

// ---- the base worker -----------------------------------------------------------------------

// ref: FUN_1414533b0
CullWorker::CullWorker(CULLSTATE* state) {
    this->m_state = state;
    this->m_index = 0;
    this->m_hub = nullptr;
    this->m_scene = nullptr;
    this->m_20 = nullptr;
    this->m_28 = nullptr;
    this->m_30 = nullptr;
    this->m_38 = nullptr;
    std::memset(this->m_48, 0, sizeof(this->m_48));
}

// ref: FUN_1414534e0
void CullWorker::Init(CWorldCull* hub, uint32_t index) {
    this->m_hub = hub;
    this->m_scene = hub->m_scene;
    void* scratch = M2AlignedAlloc(0x460, s_cullFile, 0x25, 0x27, 0);
    this->m_38 = CullScratchInit(scratch);
    CWorldView* view = this->m_hub->m_view;
    this->m_index = index;
    this->m_28 = reinterpret_cast<uint8_t*>(view) + 0x334;
}

// ref: FUN_1414535f0
void CullWorker::Begin() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1414534a0
void CullWorker::End() {
    FROST_UNIMPLEMENTED();
}

// ---- the workers ---------------------------------------------------------------------------

// ref: FUN_1414583f0
CullWorker108::CullWorker108() : CullWorker(reinterpret_cast<CULLSTATE*>(reinterpret_cast<uint8_t*>(this) + 0x388)) {
    std::memset(this->m_data, 0, sizeof(this->m_data));
    StoreIdentity(this, 0x1c0);
    StoreIdentity(this, 0x200);
    for (size_t offset = 0x248; offset <= 0x298; offset += 0x10) {
        Store<float>(this, offset, 1.0f);
    }
    Store<float>(this, 0x320, 1.0f);
    new (reinterpret_cast<uint8_t*>(this) + 0x388) CULLSTATE();
    Store<float>(this, 0x5d8, 1.0f);
    Store<float>(this, 0x5fc, 1.0f);
    Store<float>(this, 0x604, 1.0f);
    Store<uint32_t>(this, 0x340, 8);
    Store<uint32_t>(this, 0x360, 8);
    Store<uint32_t>(this, 0x380, 0x10);
}

// ref: FUN_14145d750
void CullWorker108::Init(CWorldCull* hub, uint32_t index) {
    CullWorker::Init(hub, index);
    CullResult108Clear(reinterpret_cast<uint8_t*>(this) + 0x640);
}

// ref: FUN_1414601a0
void CullWorker108::Begin() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141459790
void CullWorker108::End() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141455b00
CullWorker0d8::CullWorker0d8() : CullWorker(reinterpret_cast<CULLSTATE*>(reinterpret_cast<uint8_t*>(this) + 0x180)) {
    std::memset(this->m_data, 0, sizeof(this->m_data));
    new (reinterpret_cast<uint8_t*>(this) + 0x180) CULLSTATE();
    // The result at +0x378 (FUN_141455900): all zero but a 1 at its +0x7d0.
    Store<float>(this, 0x378 + 0x7d0, 1.0f);
}

// ref: FUN_141455e10
void CullWorker0d8::Init(CWorldCull* hub, uint32_t index) {
    CullWorker::Init(hub, index);
    FROST_UNIMPLEMENTED();  // FUN_14144d2f0: the result's arrays emptied (they start empty)
}

// ref: FUN_141455f70
void CullWorker0d8::Begin() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141455b70
void CullWorker0d8::End() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141456720
CullWorker0f0::CullWorker0f0() : CullWorker(reinterpret_cast<CULLSTATE*>(reinterpret_cast<uint8_t*>(this) + 0x190)) {
    std::memset(this->m_data, 0, sizeof(this->m_data));
    RndSeedInit(reinterpret_cast<CRndSeed*>(reinterpret_cast<uint8_t*>(this) + 0x184), 0);
    new (reinterpret_cast<uint8_t*>(this) + 0x190) CULLSTATE();
    Store<float>(this, 0x390, 1.0f);
    Store<float>(this, 0x398, 1.0f);
}

// ref: FUN_141457a90
void CullWorker0f0::Init(CWorldCull* hub, uint32_t index) {
    CullWorker::Init(hub, index);
    RndSeedInit(reinterpret_cast<CRndSeed*>(reinterpret_cast<uint8_t*>(this) + 0x184), 0x9a5ef37d);
}

// ref: FUN_141457d10
void CullWorker0f0::Begin() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141456850
void CullWorker0f0::End() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141441870
CullWorkerF40::CullWorkerF40() : CullWorker(reinterpret_cast<CULLSTATE*>(reinterpret_cast<uint8_t*>(this) + 0x180)) {
    std::memset(this->m_data, 0, sizeof(this->m_data));
    new (reinterpret_cast<uint8_t*>(this) + 0x180) CULLSTATE();
}

// ref: FUN_141441b60
void CullWorkerF40::Init(CWorldCull* hub, uint32_t index) {
    CullWorker::Init(hub, index);
}

// ref: FUN_141441d40
void CullWorkerF40::Begin() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141441900
void CullWorkerF40::End() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141460a40
CullWorker168::CullWorker168() : CullWorker(reinterpret_cast<CULLSTATE*>(reinterpret_cast<uint8_t*>(this) + 0x180)) {
    std::memset(this->m_data, 0, sizeof(this->m_data));
    new (reinterpret_cast<uint8_t*>(this) + 0x180) CULLSTATE();
}

void CullWorker168::Init(CWorldCull* hub, uint32_t index) {
    CullWorker::Init(hub, index);
}

// ref: FUN_141461800
void CullWorker168::Begin() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141460aa0
void CullWorker168::End() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1414609d0
CullWorker150::CullWorker150() : CullWorker(reinterpret_cast<CULLSTATE*>(reinterpret_cast<uint8_t*>(this) + 0x180)) {
    std::memset(this->m_data, 0, sizeof(this->m_data));
    new (reinterpret_cast<uint8_t*>(this) + 0x180) CULLSTATE();
}

// ---- the hub -------------------------------------------------------------------------------

// ref: FUN_141443a20
CWorldCull::CWorldCull(CWorldView* view) {
    this->m_flags = 0;
    this->m_flags2 = 0;
    this->m_16288 = 0;
    this->m_1628c = 0;
    this->m_16290 = 0;
    this->m_16294 = 0;
    this->m_plane[0] = 0.0f;
    this->m_plane[1] = 0.0f;
    this->m_plane[2] = 1.0f;
    this->m_plane[3] = 0.0f;
    for (int32_t i = 0; i < 16; i++) {
        this->m_matrix[i] = (i % 5) == 0 ? 1.0f : 0.0f;
    }
    std::memset(this->m_162f4, 0, sizeof(this->m_162f4));
    RndSeedInit(&this->m_seed, 0);
    // FUN_141443880: the block at +0x16318, zeroed.
    std::memset(this->m_16318, 0, sizeof(this->m_16318));
    this->Reset();
    this->Init(view);
}

// ref: FUN_141446a80
void CWorldCull::Reset() {
    this->m_flags2 = static_cast<uint8_t>(this->m_flags2 & 0xc0);
    this->m_view = nullptr;
    this->m_map = nullptr;
    this->m_mapCull = nullptr;
    this->m_scene = nullptr;
    this->m_1628c = 0;
    this->m_16290 = 0;
    this->m_16288 = 0;
    this->m_16294 = 0;
    this->m_16298 = 1.0f;
    this->m_1629c = 700.0f;
    this->m_162a0 = 2000.0f;
    for (THREADPOOLGROUP*& group : this->m_groups) {
        group = nullptr;
    }
    this->m_flags = 0;
    RndSeedInit(&this->m_seed, 0x9a5ef37d);
}

// ref: FUN_141446830
void CWorldCull::Init(CWorldView* view) {
    CWorldScene* scene = view->m_scene;
    this->m_scene = scene;
    this->m_view = view;
    CWorldMap* map = scene->m_maps[scene->m_mapIndex];
    this->m_map = map;
    this->m_mapCull = map != nullptr ? map->Field<void*>(0xe8) : nullptr;
    this->m_worker0.Init(this, 0);
    for (uint32_t i = 0; i < 5; i++) {
        this->m_workers1[i].Init(this, 1 + i);
    }
    this->m_workers6[0].Init(this, 6);
    this->m_workers6[1].Init(this, 7);
    this->m_worker8.Init(this, 8);
    this->m_worker9.Init(this, 9);
    this->m_worker10.Init(this, 10);
    for (uint32_t i = 0; i < AREA_WORKERS; i++) {
        this->m_areaWorkers[i].Init(this, 0xb + i);
    }
    // Workers 6 and 7 cull different sets (FUN_141457cf0: bits 0 and 1 of their +0x180).
    uint8_t& flags6 = this->m_workers6[0].Field<uint8_t>(0x180);
    flags6 = static_cast<uint8_t>(((flags6 & 0xfe) | 1) & 0xfd);
    uint8_t& flags7 = this->m_workers6[1].Field<uint8_t>(0x180);
    flags7 = static_cast<uint8_t>(((flags7 & 0xfe) | 0) & 0xfd | 2);
    // Workers 1 to 5 each take a pair of the tables at 0x142739f90 and 0x142739fc0
    // (FUN_141455f50: bit 3 of +0x360, the pair at +0x364).
    static const uint32_t s_first[5] = {0, 0, 1, 0, 1};
    static const uint32_t s_second[5] = {0, 0, 0, 1, 1};
    for (uint32_t i = 0; i < 5; i++) {
        CullWorker0d8& worker = this->m_workers1[i];
        worker.Field<uint8_t>(0x360) |= 8;
        worker.Field<uint32_t>(0x364) = s_first[i];
        worker.Field<uint32_t>(0x368) = s_second[i];
    }
    for (THREADPOOLGROUP*& group : this->m_groups) {
        group = ThreadPoolGroupCreate(2);
    }
}

// A physics bone's state for the culling.
// ref: FUN_141444930
void CWorldCull::AddPhysics(uint8_t* state) {
    (void)state;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1414440f0
CWorldCull::~CWorldCull() {
    for (THREADPOOLGROUP*& group : this->m_groups) {
        if (group != nullptr) {
            ThreadPoolGroupRelease(group);
            group = nullptr;
        }
    }
    FROST_UNIMPLEMENTED();  // the workers' result arrays (FUN_141444000, FUN_141453460, ...)
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141444000=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14144d2f0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141453460=GenForwardMissing")
// ---- end of link fallbacks ----

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_141269fd0 = (void*)&CullScratchInit;
// ---- end of generated globals ----
