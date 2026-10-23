#include "world/CullWorker.hpp"

#include "model2/M2Shared.hpp"
#include "storm/Memory.hpp"
#include "threadpool/ThreadPool.hpp"
#include "util/Unimplemented.hpp"
#include "world/WorldMap.hpp"
#include "world/WorldScene.hpp"
#include "world/WorldView.hpp"
#include <cfloat>
#include <cstring>
#include <new>
#include <windows.h>

extern "C" int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);
extern "C" unsigned long long FUN_14144d2f0(unsigned long long);

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

extern "C" unsigned long long FUN_1414535f0(unsigned long long);
// translated: FUN_1414535f0
void CullWorker::Begin() {
    __try {
        FUN_1414535f0((unsigned long long)this);
    } __except (GenFault(0x1414535f0ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_1414534a0(unsigned long long);
// translated: FUN_1414534a0
void CullWorker::End() {
    __try {
        FUN_1414534a0((unsigned long long)this);
    } __except (GenFault(0x1414534a0ull, GetExceptionInformation())) {
    }
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

extern "C" unsigned long long FUN_1414601a0(unsigned long long);
// translated: FUN_1414601a0
void CullWorker108::Begin() {
    __try {
        FUN_1414601a0((unsigned long long)this);
    } __except (GenFault(0x1414601a0ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141459790(unsigned long long);
// translated: FUN_141459790
void CullWorker108::End() {
    __try {
        FUN_141459790((unsigned long long)this);
    } __except (GenFault(0x141459790ull, GetExceptionInformation())) {
    }
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
    // the result's arrays (+0x378) emptied
    __try {
        FUN_14144d2f0(reinterpret_cast<unsigned long long>(this) + 0x378);
    } __except (GenFault(0x14144d2f0ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141455f70(unsigned long long);
// translated: FUN_141455f70
void CullWorker0d8::Begin() {
    __try {
        FUN_141455f70((unsigned long long)this);
    } __except (GenFault(0x141455f70ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141455b70(unsigned long long);
// translated: FUN_141455b70
void CullWorker0d8::End() {
    __try {
        FUN_141455b70((unsigned long long)this);
    } __except (GenFault(0x141455b70ull, GetExceptionInformation())) {
    }
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

extern "C" unsigned long long FUN_141457d10(unsigned long long);
// translated: FUN_141457d10
void CullWorker0f0::Begin() {
    __try {
        FUN_141457d10((unsigned long long)this);
    } __except (GenFault(0x141457d10ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141456850(unsigned long long);
// translated: FUN_141456850
void CullWorker0f0::End() {
    __try {
        FUN_141456850((unsigned long long)this);
    } __except (GenFault(0x141456850ull, GetExceptionInformation())) {
    }
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

extern "C" unsigned long long FUN_141441d40(unsigned long long);
// translated: FUN_141441d40
void CullWorkerF40::Begin() {
    __try {
        FUN_141441d40((unsigned long long)this);
    } __except (GenFault(0x141441d40ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141441900(unsigned long long);
// translated: FUN_141441900
void CullWorkerF40::End() {
    __try {
        FUN_141441900((unsigned long long)this);
    } __except (GenFault(0x141441900ull, GetExceptionInformation())) {
    }
}

// ref: FUN_141460a40
CullWorker168::CullWorker168() : CullWorker(reinterpret_cast<CULLSTATE*>(reinterpret_cast<uint8_t*>(this) + 0x180)) {
    std::memset(this->m_data, 0, sizeof(this->m_data));
    new (reinterpret_cast<uint8_t*>(this) + 0x180) CULLSTATE();
}

void CullWorker168::Init(CWorldCull* hub, uint32_t index) {
    CullWorker::Init(hub, index);
}

extern "C" unsigned long long FUN_141461800(unsigned long long);
// translated: FUN_141461800
void CullWorker168::Begin() {
    __try {
        FUN_141461800((unsigned long long)this);
    } __except (GenFault(0x141461800ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141460aa0(unsigned long long);
// translated: FUN_141460aa0
void CullWorker168::End() {
    __try {
        FUN_141460aa0((unsigned long long)this);
    } __except (GenFault(0x141460aa0ull, GetExceptionInformation())) {
    }
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

extern "C" {
unsigned long long FUN_141444000(unsigned long long);
unsigned long long FUN_141453460(unsigned long long);
unsigned long long FUN_141443bb0(unsigned long long);
unsigned long long FUN_141443d40(unsigned long long);
unsigned long long FUN_141443f90(unsigned long long);
unsigned long long FUN_141443c30(unsigned long long);
unsigned long long FUN_14141f110(unsigned long long);
unsigned long long FUN_14141f0d0(unsigned long long);
unsigned long long FUN_14141f0f0(unsigned long long);
}

namespace {

const char s_fixedArrayTag[] = ".?AV?$TSFixedArray";

// A TSFixedArray's storage given back (its pointer is the array's +0x08).
void FreeFixedArray(uint8_t* base, intptr_t offset) {
    void*& data = *reinterpret_cast<void**>(base + offset);
    if (data != nullptr) {
        SMemFree(data, s_fixedArrayTag, -2, 0);
    }
}

// `count` 0x20-byte elements destroyed in place, last first (_vector_destructor_iterator_).
void DestroyElements(uint8_t* first, int32_t count, unsigned long long (*destroy)(unsigned long long)) {
    for (int32_t i = count; i-- > 0;) {
        destroy(reinterpret_cast<unsigned long long>(first + i * 0x20));
    }
}

}  // namespace

// The rest of the hub's teardown (the tail of FUN_1414440f0 after the thread pool groups): the
// workers' result arrays, which the translated cull fills, by the reference's offsets. Each
// worker's own (FUN_141453460) and its lists (FUN_141443bb0, FUN_141443d40, FUN_141443f90,
// FUN_141443c30, the element destructors) are translated.
static void CWorldCullFreeResults(CWorldCull* cull) {
    auto base = reinterpret_cast<uint8_t*>(cull);
    auto at = [base](intptr_t offset) {
        return reinterpret_cast<unsigned long long>(base + offset);
    };
    __try {
        FUN_141444000(at(0x16318));
        FUN_141453460(at(0x15e88));
        FreeFixedArray(base, 0x15e70);
        FUN_141453460(at(0x15b00));
        // the area workers, from the last
        FUN_141443bb0(at(0x15738));
        for (intptr_t worker = 0, end = 0x15a88; worker < 0x20; worker++, end -= 0x850) {
            FUN_141443d40(at(end - 0xac * 8));
            FreeFixedArray(base, end - 0x106 * 8);
            FreeFixedArray(base, end - 0x10a * 8);
            FreeFixedArray(base, end - 0x10e * 8);
            FUN_141453460(at(0x15738 - (worker + 1) * 0x850));
        }
        // the two map-object workers
        for (intptr_t worker = 0, end = 0x5150; worker < 2; worker++, end -= 0x478) {
            FUN_141443f90(at(end - 0x478));
            FreeFixedArray(base, end - 0x490);
            FreeFixedArray(base, end - 0x4b0);
            FUN_141453460(at(0x4d38 - (worker + 1) * 0x478));
        }
        // the five area-chunk workers
        for (intptr_t worker = 0, end = 0x4930; worker < 5; worker++, end -= 0x17f * 8) {
            FreeFixedArray(base, end - 0xa0 * 8);
            FUN_141443f90(at(end - 0xad * 8));
            FreeFixedArray(base, end - 0xb0 * 8);
            FreeFixedArray(base, end - 0xf9 * 8);
            FreeFixedArray(base, end - 0x17f * 8);
            FreeFixedArray(base, end - 0x183 * 8);
            DestroyElements(base + end - 400 * 8, 3, FUN_14141f110);
            FreeFixedArray(base, end - 0x194 * 8);
            DestroyElements(base + end - 0x19d * 8, 2, FUN_14141f0d0);
            FreeFixedArray(base, end - 0x1a0 * 8);
            DestroyElements(base + end - 0x1ad * 8, 3, FUN_14141f0f0);
            FUN_141453460(at(0x4448 - (worker + 1) * 0xbf8));
        }
        FUN_141443c30(at(0x20));
    } __except (GenFault(0x1414440f0ull, GetExceptionInformation())) {
    }
}

// ref: FUN_1414440f0
CWorldCull::~CWorldCull() {
    for (THREADPOOLGROUP*& group : this->m_groups) {
        if (group != nullptr) {
            ThreadPoolGroupRelease(group);
            group = nullptr;
        }
    }
    CWorldCullFreeResults(this);
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:GenFault=GenFaultMissing")
#pragma comment(linker, "/alternatename:FUN_14141f0d0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14141f0f0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14141f110=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141441900=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141441d40=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141443bb0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141443c30=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141443d40=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141443f90=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141444000=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14144d2f0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141453460=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1414534a0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1414535f0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141455b70=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141455f70=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141456850=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141457d10=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141459790=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1414601a0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141460aa0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141461800=GenForwardMissing")
// ---- end of link fallbacks ----

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_141269fd0 = (void*)&CullScratchInit;
// ---- end of generated globals ----
