#ifndef WORLD_CULL_WORKER_HPP
#define WORLD_CULL_WORKER_HPP

// WoW/Source/WorldClient2/CullWorker.cpp: a view's culling. Each view owns a culling hub
// (CWorldCull, 0x16578 bytes, WorldView.cpp:0x83) holding 43 workers, each a CullWorker
// with three virtuals (Init with the hub and the worker's index, Begin with the frame, End).
// The hub's Init (FUN_141446830) calls every worker's Init with indices 0 to 42 in layout
// order and makes five thread pool groups.
//
// The workers' layouts past the base are known only from their constructors and are reached
// through Field<T>(offset), offsets from the worker itself. Their reference names are not
// known; they are named by the vtable they carry.

#include "base/RndSeed.hpp"
#include <cstddef>
#include <cstdint>

class CWorldView;
class CWorldScene;
class CWorldMap;
class CWorldCull;
struct THREADPOOLGROUP;

// The bounds and flags a cull pass produces (FUN_14144cda0 initialises it, 0x9c bytes):
// several boxes empty (FLT_MAX / -FLT_MAX) and a flag byte at +0.
struct CULLRESULT {
    uint8_t m_data[0x9c];

    // ref: FUN_14144cda0
    CULLRESULT();

    template <class T>
    T& Field(size_t offset) {
        return *reinterpret_cast<T*>(this->m_data + offset);
    }
};

// The per-worker frame state its Init hands the base (FUN_14141eef0 initialises it, 0x1dc).
struct CULLSTATE {
    uint8_t m_data[0x1dc];

    // ref: FUN_14141eef0
    CULLSTATE();

    template <class T>
    T& Field(size_t offset) {
        return *reinterpret_cast<T*>(this->m_data + offset);
    }
};

class CullWorker {
    public:
    // vtable 0x14273a078. The derived workers' slots call these first.
    // ref: FUN_1414534e0
    virtual void Init(CWorldCull* hub, uint32_t index);
    // ref: FUN_1414535f0
    virtual void Begin();
    // ref: FUN_1414534a0
    virtual void End();

    uint32_t m_index;                   // 0x08
    uint32_t m_pad0c;
    CWorldCull* m_hub;                  // 0x10
    CWorldScene* m_scene;               // 0x18
    void* m_20;                         // 0x20 the current map's 0xe8 object, per frame
    void* m_28;                         // 0x28 the view's +0x334
    void* m_30;                         // 0x30 the scene's 0x48 (particulates), per frame
    void* m_38;                         // 0x38 0x460 bytes (FUN_141269fd0)
    CULLSTATE* m_state;                 // 0x40 the derived worker's state block
    uint8_t m_48[0x98];                 // 0x48 (FUN_14144cfd0 ends a frame on it)
    CULLRESULT m_result;                // 0xe0

    // ref: FUN_1414533b0
    explicit CullWorker(CULLSTATE* state);

    template <class T>
    T& Field(size_t offset) {
        return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(this) + offset);
    }
};
static_assert(offsetof(CullWorker, m_state) == 0x40, "CullWorker layout");
static_assert(offsetof(CullWorker, m_result) == 0xe0, "CullWorker layout");
static_assert(sizeof(CullWorker) == 0x180, "CullWorker is 0x180 bytes");

// vtable 0x14273a108 (0x850): the hub's first worker and its 32 at +0x4d38. Its result at
// +0x640 lists detail doodad batches (TSFixedArray<CVisDetailDoodadBatch>).
class CullWorker108 : public CullWorker {
    public:
    uint8_t m_data[0x850 - sizeof(CullWorker)];

    // ref: FUN_1414583f0
    CullWorker108();
    // ref: FUN_14145d750
    void Init(CWorldCull* hub, uint32_t index) override;
    // ref: FUN_1414601a0
    void Begin() override;
    // ref: FUN_141459790
    void End() override;
};
static_assert(sizeof(CullWorker108) == 0x850, "CullWorker108 is 0x850 bytes");

// vtable 0x14273a0d8 (0xbf8): the hub's workers 1 to 5; its result at +0x378.
class CullWorker0d8 : public CullWorker {
    public:
    uint8_t m_data[0xbf8 - sizeof(CullWorker)];

    // ref: FUN_141455b00
    CullWorker0d8();
    // ref: FUN_141455e10
    void Init(CWorldCull* hub, uint32_t index) override;
    // ref: FUN_141455f70
    void Begin() override;
    // ref: FUN_141455b70
    void End() override;
};
static_assert(sizeof(CullWorker0d8) == 0xbf8, "CullWorker0d8 is 0xbf8 bytes");

// vtable 0x14273a0f0 (0x478): workers 6 and 7, with a random seed at +0x184 and their
// result at +0x3d8.
class CullWorker0f0 : public CullWorker {
    public:
    uint8_t m_data[0x478 - sizeof(CullWorker)];

    // ref: FUN_141456720
    CullWorker0f0();
    // ref: FUN_141457a90
    void Init(CWorldCull* hub, uint32_t index) override;
    // ref: FUN_141457d10
    void Begin() override;
    // ref: FUN_141456850
    void End() override;
};
static_assert(sizeof(CullWorker0f0) == 0x478, "CullWorker0f0 is 0x478 bytes");

// vtable 0x142739f40 (0x3c8): worker 8, result at +0x368.
class CullWorkerF40 : public CullWorker {
    public:
    uint8_t m_data[0x3c8 - sizeof(CullWorker)];

    // ref: FUN_141441870
    CullWorkerF40();
    // ref: FUN_141441b60
    void Init(CWorldCull* hub, uint32_t index) override;
    // ref: FUN_141441d40
    void Begin() override;
    // ref: FUN_141441900
    void End() override;
};
static_assert(sizeof(CullWorkerF40) == 0x3c8, "CullWorkerF40 is 0x3c8 bytes");

// vtable 0x14273a168 (0x388): worker 9, result at +0x368.
class CullWorker168 : public CullWorker {
    public:
    uint8_t m_data[0x388 - sizeof(CullWorker)];

    // ref: FUN_141460a40
    CullWorker168();
    // FUN_141441b60, as CullWorkerF40's.
    void Init(CWorldCull* hub, uint32_t index) override;
    // ref: FUN_141461800
    void Begin() override;
    // ref: FUN_141460aa0
    void End() override;
};
static_assert(sizeof(CullWorker168) == 0x388, "CullWorker168 is 0x388 bytes");

// vtable 0x14273a150 (0x360): worker 10. Its three slots are jumps to the base's
// (0x141460a20 Init, 0x141460a30 Begin, 0x141460a10 End).
class CullWorker150 : public CullWorker {
    public:
    uint8_t m_data[0x360 - sizeof(CullWorker)];

    // ref: FUN_1414609d0
    CullWorker150();
};
static_assert(sizeof(CullWorker150) == 0x360, "CullWorker150 is 0x360 bytes");

// The hub (0x16578).
class CWorldCull {
    public:
    enum : uint32_t {
        AREA_WORKERS = 32,
        GROUP_COUNT = 5,
    };

    CWorldView* m_view;                         // 0x00000
    CWorldScene* m_scene;                       // 0x00008
    void* m_mapCull;                            // 0x00010 the current map's 0xe8 object
    CWorldMap* m_map;                           // 0x00018
    CullWorker108 m_worker0;                    // 0x00020
    CullWorker0d8 m_workers1[5];                // 0x00870
    CullWorker0f0 m_workers6[2];                // 0x04448
    CullWorker108 m_areaWorkers[AREA_WORKERS];  // 0x04d38
    CullWorkerF40 m_worker8;                    // 0x15738
    CullWorker168 m_worker9;                    // 0x15b00
    CullWorker150 m_worker10;                   // 0x15e88
    CULLRESULT m_result;                        // 0x161e8 (its +0 bits 3 and 4 asked by the view)
    uint8_t m_flags;                            // 0x16284
    uint8_t m_flags2;                           // 0x16285 bits 6-7 kept by Reset
    uint8_t m_pad16286[2];
    uint32_t m_16288;                           // 0x16288
    uint32_t m_1628c;                           // 0x1628c
    uint32_t m_16290;                           // 0x16290
    uint32_t m_16294;                           // 0x16294
    float m_16298;                              // 0x16298 1
    float m_1629c;                              // 0x1629c 700
    float m_162a0;                              // 0x162a0 2000
    float m_plane[4];                           // 0x162a4 (0, 0, 1, 0)
    float m_matrix[16];                         // 0x162b4 identity
    uint8_t m_162f4[0x18];                      // 0x162f4
    CRndSeed m_seed;                            // 0x1630c
    uint32_t m_pad16314;
    uint8_t m_16318[0x238];                     // 0x16318 (FUN_141443880)
    THREADPOOLGROUP* m_groups[GROUP_COUNT];     // 0x16550

    // ref: FUN_141443a20
    explicit CWorldCull(CWorldView* view);
    // ref: FUN_1414440f0
    ~CWorldCull();

    template <class T>
    T& Field(size_t offset) {
        return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(this) + offset);
    }

    // ref: FUN_141446a80
    void Reset();
    // ref: FUN_141446830
    void Init(CWorldView* view);
    // ref: FUN_141444930
    void AddPhysics(uint8_t* state);
};
static_assert(offsetof(CWorldCull, m_worker0) == 0x20, "CWorldCull layout");
static_assert(offsetof(CWorldCull, m_workers1) == 0x870, "CWorldCull layout");
static_assert(offsetof(CWorldCull, m_workers6) == 0x4448, "CWorldCull layout");
static_assert(offsetof(CWorldCull, m_areaWorkers) == 0x4d38, "CWorldCull layout");
static_assert(offsetof(CWorldCull, m_worker8) == 0x15738, "CWorldCull layout");
static_assert(offsetof(CWorldCull, m_worker9) == 0x15b00, "CWorldCull layout");
static_assert(offsetof(CWorldCull, m_worker10) == 0x15e88, "CWorldCull layout");
static_assert(offsetof(CWorldCull, m_result) == 0x161e8, "CWorldCull layout");
static_assert(offsetof(CWorldCull, m_flags) == 0x16284, "CWorldCull layout");
static_assert(offsetof(CWorldCull, m_seed) == 0x1630c, "CWorldCull layout");
static_assert(offsetof(CWorldCull, m_groups) == 0x16550, "CWorldCull layout");
static_assert(sizeof(CWorldCull) == 0x16578, "CWorldCull is 0x16578 bytes");

// The cull's occluder list (FUN_141446590), its frame finished (FUN_141445630) and the water
// height it found (FUN_141446290).
void* CWorldCullOccluders(CWorldCull* cull);
void CWorldCullUpdate(CWorldCull* cull);
float CWorldCullWaterHeight(CWorldCull* cull);

#endif
