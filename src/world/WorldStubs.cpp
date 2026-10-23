#include "world/WorldStubs.hpp"

#include <cstring>

#include "storm/Memory.hpp"
#include "storm/list/TSList.hpp"
#include "util/Unimplemented.hpp"
#include "world/WorldQueries.hpp"
#include <windows.h>

extern "C" int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);

// ---- the type 6 scene's objects ------------------------------------------------------------

extern "C" unsigned long long FUN_1412a5ce0(unsigned long long);
// translated: FUN_1412a5ce0
void* WorldScene1950Create(void* memory) {
    __try {
        return reinterpret_cast<void*>(FUN_1412a5ce0((unsigned long long)memory));
    } __except (GenFault(0x1412a5ce0ull, GetExceptionInformation())) {
        return nullptr;
    }
}

// ref: FUN_1412a9ff0
void WorldScene1950Release() {
    FROST_UNIMPLEMENTED();
}

extern "C" unsigned long long FUN_1412a5dc0(unsigned long long);
// translated: FUN_1412a5dc0
void WorldScene1950Destroy(void* object) {
    __try {
        FUN_1412a5dc0((unsigned long long)object);
    } __except (GenFault(0x1412a5dc0ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141439100(unsigned long long);
// translated: FUN_141439100
void* WorldScene6d8Create(void* memory) {
    __try {
        return reinterpret_cast<void*>(FUN_141439100((unsigned long long)memory));
    } __except (GenFault(0x141439100ull, GetExceptionInformation())) {
        return nullptr;
    }
}

extern "C" unsigned long long FUN_141439440(unsigned long long);
// translated: FUN_141439440
void WorldScene6d8Destroy(void* object) {
    __try {
        FUN_141439440((unsigned long long)object);
    } __except (GenFault(0x141439440ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141439ee0(unsigned long long);
// translated: FUN_141439ee0
void WorldScene6d8Unload(void* object) {
    __try {
        FUN_141439ee0((unsigned long long)object);
    } __except (GenFault(0x141439ee0ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_14143a9e0(unsigned long long);
// translated: FUN_14143a9e0
void* WorldScene6e0Create(void* memory) {
    __try {
        return reinterpret_cast<void*>(FUN_14143a9e0((unsigned long long)memory));
    } __except (GenFault(0x14143a9e0ull, GetExceptionInformation())) {
        return nullptr;
    }
}

extern "C" unsigned long long FUN_14143ab50(unsigned long long);
// translated: FUN_14143ab50
void WorldScene6e0Destroy(void* object) {
    __try {
        FUN_14143ab50((unsigned long long)object);
    } __except (GenFault(0x14143ab50ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_14143adf0(unsigned long long);
// translated: FUN_14143adf0
void WorldScene6e0Unload(void* object) {
    __try {
        FUN_14143adf0((unsigned long long)object);
    } __except (GenFault(0x14143adf0ull, GetExceptionInformation())) {
    }
}

// ---- the sky scene -------------------------------------------------------------------------

// ref: FUN_14143bcc0
void SkySceneDestroy(void* sky) {
    (void)sky;
    FROST_UNIMPLEMENTED();
}

extern "C" unsigned long long FUN_14143cad0(unsigned long long);
// translated: FUN_14143cad0
void SkySceneUnload(void* sky) {
    __try {
        FUN_14143cad0((unsigned long long)sky);
    } __except (GenFault(0x14143cad0ull, GetExceptionInformation())) {
    }
}

// ---- the scene's 0xfa50 object -------------------------------------------------------------

// Its 0x3e8 (count, a quad) cells, its texture (FUN_14127e630 on `a4`) and the cell setup
// (FUN_1414242d0, FUN_1414240f0): all the reference's, so its teardown is too.
extern "C" unsigned long long FUN_141423ae0(unsigned long long, double, double, unsigned long long);
// translated: FUN_141423ae0
void* WorldScene030Create(void* memory, float a2, float a3, uint32_t a4) {
    __try {
        return reinterpret_cast<void*>(FUN_141423ae0((unsigned long long)memory, (double)a2, (double)a3, (unsigned long long)a4));
    } __except (GenFault(0x141423ae0ull, GetExceptionInformation())) {
        return nullptr;
    }
}

extern "C" unsigned long long FUN_141423c00(unsigned long long);
// translated: FUN_141423c00
void WorldScene030Destroy(void* object) {
    __try {
        FUN_141423c00((unsigned long long)object);
    } __except (GenFault(0x141423c00ull, GetExceptionInformation())) {
    }
}

// ---- MapWeather ----------------------------------------------------------------------------

// The weather (0x1378 bytes): its own fields, its particle base (FUN_14138a320), its sound block
// (+0x1230, FUN_141389fc0) and its 5x5 precipitation grid (+0x1200, 0xe0 bytes from
// FUN_141389d40), all the reference's. MapWeatherDensity and the rest below read it at the
// reference's offsets.
extern "C" unsigned long long FUN_14138a820(unsigned long long, unsigned long long);
// translated: FUN_14138a820
void* MapWeatherCreate(void* memory, CWorldScene* scene) {
    __try {
        return reinterpret_cast<void*>(FUN_14138a820((unsigned long long)memory, (unsigned long long)scene));
    } __except (GenFault(0x14138a820ull, GetExceptionInformation())) {
        return nullptr;
    }
}

extern "C" unsigned long long FUN_14138b150(unsigned long long);
// translated: FUN_14138b150
void MapWeatherDestroy(void* weather) {
    __try {
        FUN_14138b150((unsigned long long)weather);
    } __except (GenFault(0x14138b150ull, GetExceptionInformation())) {
    }
}

// ---- fadeouts and capture ------------------------------------------------------------------

// The object fadeouts (TSList<SObjectFadeout>): the active list (0x142d011b8) and the free list
// its nodes go back to (0x142d011d0), both the reference's own, filled by the translated fade
// code. Each node keeps its object at +0 and a second link (the scene's) at +0x68.
namespace {

using FADEOUTLIST = TSList<uint8_t, TSGetLink<uint8_t>, 8>;

FADEOUTLIST* ActiveFadeouts() {
    return reinterpret_cast<FADEOUTLIST*>(0x142d011b8ull);
}

FADEOUTLIST* FreeFadeouts() {
    return reinterpret_cast<FADEOUTLIST*>(0x142d011d0ull);
}

extern "C" unsigned long long FUN_14134fd60(unsigned long long);

// An object's fade let go (FUN_14134fd60, translated: a model's fade count, or the object).
void ReleaseFadeObject(uint64_t object) {
    __try {
        FUN_14134fd60(object);
    } __except (GenFault(0x14134fd60ull, GetExceptionInformation())) {
    }
}

}  // namespace

// The scene's unload: every active fadeout's object let go and the node moved to the free list
// (part of FUN_1413c3a40).
void WorldSceneFreeObjectFadeouts() {
    FADEOUTLIST* active = ActiveFadeouts();
    FADEOUTLIST* spare = FreeFadeouts();
    uint8_t* node = active->Head();
    while (node != nullptr) {
        uint8_t* next = active->Next(node);
        ReleaseFadeObject(*reinterpret_cast<uint64_t*>(node));
        *reinterpret_cast<uint64_t*>(node) = 0;
        reinterpret_cast<TSLink<uint8_t>*>(node + 0x68)->Unlink();
        spare->Link(node)->Unlink();
        spare->LinkToTail(node);
        node = next;
    }
}

// The scene's destruction: the free list's nodes unlinked and freed (part of FUN_1413c24e0).
void WorldSceneFreeFadeouts() {
    FADEOUTLIST* spare = FreeFadeouts();
    while (uint8_t* node = spare->Head()) {
        reinterpret_cast<TSLink<uint8_t>*>(node + 0x68)->Unlink();
        spare->Link(node)->Unlink();
        SMemFree(node, ".?AV?$TSList@USObjectFadeout@@", -2, 0);
    }
}

void WorldSceneDeleteA80(void* object) {
    (void)object;
    FROST_UNIMPLEMENTED();
}

// ---- map queries ---------------------------------------------------------------------------

// ref: FUN_1413bc550
bool WorldMapIntersectSegment(CWorldMap* map, WORLDSEGMENT* segment, WORLDSEGMENTHIT* hit) {
    (void)map;
    (void)segment;
    (void)hit;
    FROST_UNIMPLEMENTED();
    return false;
}

// ref: FUN_1413bbf60
void WorldMapIntersectSegmentMulti() {
    FROST_UNIMPLEMENTED();
}

extern "C" unsigned long long FUN_1413b5b40(unsigned long long, unsigned long long, unsigned long long);
// translated: FUN_1413b5b40
bool WorldMapGroundHeight(CWorldMap* map, const float* position, float* height) {
    __try {
        return (FUN_1413b5b40((unsigned long long)map, (unsigned long long)position, (unsigned long long)height) & 0xff) != 0;
    } __except (GenFault(0x1413b5b40ull, GetExceptionInformation())) {
        return false;
    }
}

// ---- the view's lists ----------------------------------------------------------------------

extern "C" unsigned long long FUN_1413a2970(unsigned long long);
// translated: FUN_1413a2970
void* WorldProjectedDecalCreate(void* query) {
    __try {
        return reinterpret_cast<void*>(FUN_1413a2970((unsigned long long)query));
    } __except (GenFault(0x1413a2970ull, GetExceptionInformation())) {
        return nullptr;
    }
}

extern "C" unsigned long long FUN_141364cc0(unsigned long long, unsigned long long);
// translated: FUN_141364cc0
void WorldViewListAdd(WORLDVIEWLIST* list, void* node) {
    __try {
        FUN_141364cc0((unsigned long long)list, (unsigned long long)node);
    } __except (GenFault(0x141364cc0ull, GetExceptionInformation())) {
    }
}

// ref: FUN_1413e0270
void WorldViewListRelease(void* node) {
    (void)node;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1413a7a20
void WorldViewReflectionDestroy(void* reflection) {
    (void)reflection;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1413a1e20
void WorldViewDrawProjected(CWorldView* view, GxRenderContext* context, ShaderEffectContext* shader,
    M2Element* element, uint8_t* box, uint32_t* color, uint8_t* object) {
    (void)view;
    (void)context;
    (void)shader;
    (void)element;
    (void)box;
    (void)color;
    (void)object;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14138c730
float MapWeatherDensity(void* weather) {
    return *reinterpret_cast<float*>(static_cast<uint8_t*>(weather) + 0x1224);
}

// ref: FUN_14138c740
const float* MapWeatherColor(void* weather) {
    return reinterpret_cast<const float*>(static_cast<uint8_t*>(weather) + 0x1218);
}

// ref: FUN_14138c760
const float* MapWeatherDirection(void* weather) {
    return reinterpret_cast<const float*>(static_cast<uint8_t*>(weather) + 0x120c);
}

// ref: FUN_14138c750
float MapWeatherIntensity(void* weather) {
    return *reinterpret_cast<float*>(static_cast<uint8_t*>(weather) + 0x1228);
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:GenFault=GenFaultMissing")
#pragma comment(linker, "/alternatename:FUN_1412a5ce0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1412a5dc0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14134fd60=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141364cc0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14138a820=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14138b150=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413a2970=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413b5b40=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141423ae0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141423c00=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141439100=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141439440=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141439ee0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14143a9e0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14143ab50=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14143adf0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14143cad0=GenForwardMissing")
// ---- end of link fallbacks ----
