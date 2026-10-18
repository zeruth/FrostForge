#include "ui/ModelSceneFrame.hpp"

#include <cstring>
#include <new>

#include <windows.h>

#include "base/DataAllocator.hpp"
#include "framescript/FrameScript.hpp"
#include "model2/CM2Lighting.hpp"
#include "storm/Memory.hpp"
#include "ui/BinaryXML.hpp"
#include "world/ModelFog.hpp"
#include "world/WorldScene.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

// The reference's own functions this class calls, translated (src/gen); each line below makes
// tools/translate.py take it as a root.
extern "C" {
int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);
// (noexcept, as the runtime declares them; C5043 otherwise)
void __cdecl _Init_thread_header(int* guard) noexcept;
void __cdecl _Init_thread_footer(int* guard) noexcept;
// translated: FUN_141a93190
unsigned long long FUN_141a93190(unsigned long long actor);
// translated: FUN_141a95670
unsigned long long FUN_141a95670(unsigned long long block, unsigned long long frame);
// translated: FUN_141a97cd0
unsigned long long FUN_141a97cd0(unsigned long long array, unsigned long long count);
// translated: FUN_141a935a0
unsigned long long FUN_141a935a0(unsigned long long actor, unsigned long long node, unsigned long long status);
// translated: FUN_14060f390
unsigned long long FUN_14060f390(unsigned long long object, unsigned long long node, unsigned long long status);
// translated: FUN_141a96fe0
unsigned long long FUN_141a96fe0(unsigned long long frame, unsigned long long elapsedBits);
// translated: FUN_141a96ee0
unsigned long long FUN_141a96ee0(unsigned long long frame, unsigned long long batch);
// translated: FUN_141a96e70
unsigned long long FUN_141a96e70(unsigned long long layout, unsigned long long scale);
// translated: FUN_141a96f70
unsigned long long FUN_141a96f70(unsigned long long layout, unsigned long long rect, unsigned long long previous);
// translated: FUN_141a970a0
unsigned long long FUN_141a970a0(unsigned long long layout);
}
// the runtime's thread-safe-statics epoch (src/gen/RefImage.cpp; the compiler crashes on the
// thread-local itself in this file)
extern "C" int GenInitThreadEpoch();

int32_t ModelSceneFrame::s_metaTable = -2;           // DAT_142d2f568
int32_t ModelSceneFrame::s_restrictedMetaTable = -2; // DAT_142d2f56c

namespace {

// The model scenes' pool (DAT_143208508).
CDataAllocator s_modelSceneHeap(0x4d0, 10);  // DAT_143208508

// The actors' pool (0xe8 bytes, 20 a chunk): the reference's own, in the image, since the
// translated actor destructor returns blocks to it.
CDataAllocator& ActorHeap() {
    return *reinterpret_cast<CDataAllocator*>(0x1432084e0ull);
}

// The one script past the Frame ones a model scene has (OnModelLoaded's index), no arguments.
constexpr uint8_t SCRIPT_MODELSCENE = 0x30;

template <class F>
void Guarded(uint64_t address, F&& body) {
    __try {
        body();
    } __except (GenFault(address, GetExceptionInformation())) {
    }
}

unsigned long long Raw(const void* p) {
    return reinterpret_cast<unsigned long long>(p);
}

}  // namespace

// ref: FUN_14167aee0 (the pool half)
void* ModelSceneFrame::operator new(size_t size) {
    (void)size;
    return s_modelSceneHeap.GetData(false, ".?AVModelSceneFrame@@", -2, 0x1f);
}

void ModelSceneFrame::operator delete(void* frame) {
    if (frame != nullptr) {
        s_modelSceneHeap.PutData(frame);
    }
}

// ref: FUN_141a960c0
ModelSceneFrame::ModelSceneFrame(CSimpleFrame* parent) : CSimpleFrame(parent) {
    // The reference sets each field; what it leaves unset is the pool's leftovers, zero here.
    this->m_scene = nullptr;
    std::memset(this->m_fields, 0, sizeof(this->m_fields));
    std::memset(this->m_tail, 0, sizeof(this->m_tail));
    this->At<uint64_t>(0x308) = 0x3f800000;
    this->At<uint64_t>(0x31c) = 0x3f800000;
    this->At<uint64_t>(0x330) = 0x3f800000;
    this->At<uint32_t>(0x344) = 0x3f800000;    // 1.0
    this->At<uint32_t>(0x348) = 0x3f71463b;    // 0.943
    this->At<uint32_t>(0x34c) = 0x3e4ccccd;    // 0.2
    this->At<uint32_t>(0x350) = 0x42c80000;    // 100.0
    this->At<uint32_t>(0x354) = 0x3f800000;
    new (this->m_fog) MODELFOG();
    new (this->m_lighting) CM2Lighting();
    this->At<uint8_t>(0x4ac) = 0;
    this->At<uint64_t>(0x4b0) = 0;
    this->At<uint32_t>(0x4a8) = (this->At<uint32_t>(0x4a8) & 0xfffffff2) | 2;
    // the fog's distances (1e7 and 1e8) and its flags
    this->At<uint32_t>(0x374) = 0x4b189680;
    this->At<uint32_t>(0x378) = 0x4cbebc20;
    this->At<uint32_t>(0x380) = 0;
    this->Lighting()->SetLightType(0);
    // the light's colours, at the reference's offsets in its CM2Lighting
    this->At<uint32_t>(0x468) = 0x3f4ccccd;    // 0.8
    this->At<uint32_t>(0x46c) = 0x3f4ccccd;
    this->At<uint32_t>(0x470) = 0x3f23d70a;    // 0.64
    for (uint32_t offset = 0x444; offset <= 0x464; offset += 4) {
        this->At<uint32_t>(offset) = 0x3f333333;    // 0.7
    }
    const float direction[3] = { 0.0f, 1.0f, 0.0f };
    this->Lighting()->SetDirection(direction);
    this->Lighting()->SetEnabled(1);
}

// ref: FUN_141a962e0
ModelSceneFrame::~ModelSceneFrame() {
    // the actors, each released, then the scene and the light's link
    auto actors = reinterpret_cast<uint64_t*>(this->At<uint64_t>(0x358));
    uint64_t count = this->At<uint64_t>(0x360);
    for (uint64_t i = 0; i < count; i++) {
        uint64_t actor = actors[i];
        Guarded(0x141a93190, [&] { FUN_141a93190(actor); });
    }
    this->At<uint64_t>(0x360) = 0;
    if (this->m_scene != nullptr) {
        this->m_scene->Release();
    }
    this->Lighting()->Unlink();
    if (static_cast<int64_t>(this->At<uint64_t>(0x368)) >= 0) {
        SMemFree(reinterpret_cast<void*>(this->At<uint64_t>(0x358)), ".PEAVModelSceneFrameActor@@", -6, 0);
        this->At<uint64_t>(0x358) = 0;
    }
}

// The reference's type id and its magic-static guard, held here so translated code and the
// port number the class once, alike.
int32_t g_modelSceneFrameType;  // DAT_1430a0b0c
int32_t g_modelSceneFrameTypeGuard;  // DAT_1430a0b10

int32_t ModelSceneFrame::GetObjectType() {
    return FrameScript_TypeId(&g_modelSceneFrameType, &g_modelSceneFrameTypeGuard);
}

// The model scene a script method was called on (its table at `index`), or null with the
// reason in *error.
// ref: FUN_140d11f10
FrameScript_Object* ModelSceneFrame::GetObjectFromLua(const char** error, lua_State* L, int32_t index,
    bool allowForbidden, bool allowLockdown) {
    return FrameScript_Object::GetObjectFromLua(error, L, ModelSceneFrame::GetObjectType(), index, allowForbidden,
        allowLockdown);
}

// ref: FUN_141a96680
void ModelSceneFrame::CreateScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    s_metaTable = FrameScript_Object::CreateScriptMetaTable(L, ModelSceneFrame_FillScriptMethods, false);
    s_restrictedMetaTable = FrameScript_Object::CreateScriptMetaTable(L, ModelSceneFrame_FillScriptMethods, true);
}

// ref: FUN_141a96990
int32_t ModelSceneFrame::GetScriptMetaTable() {
    if (!(this->FrameScript_Object::m_flags & FSOBJ_FORBIDDEN)
        && !(this->FrameScript_Object::m_flags & FSOBJ_PROTECTED_METATABLE)) {
        return s_metaTable;
    }
    return s_restrictedMetaTable;
}

// ref: FUN_141a97bc0
bool ModelSceneFrame::GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) {
    if (index == SCRIPT_MODELSCENE) {
        return true;
    }
    return this->CSimpleFrame::GetScriptData(index, data);
}

// ref: FUN_141a96b20
bool ModelSceneFrame::IsObjectTypeName(const char* name) {
    return _stricmp(name, "ModelScene") == 0 || this->CSimpleFrame::IsObjectTypeName(name);
}

// ref: FUN_141a969c0
bool ModelSceneFrame::IsObjectType(int32_t type) {
    return type == ModelSceneFrame::GetObjectType() || this->CSimpleFrame::IsObjectType(type);
}

// ref: FUN_141a96900
const char* ModelSceneFrame::GetObjectTypeName() {
    return "ModelScene";
}

// ref: FUN_141a968e0
uint32_t ModelSceneFrame::GetObjectSize() {
    return 0x4d0;
}

// The fog distances, the fog colour, the draw layer, then one actor per <Actors> child and the
// view insets. The actors are the reference's own objects (translated), from its own pool.
// ref: FUN_141a96bb0
void ModelSceneFrame::LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) {
    this->CSimpleFrame::LoadXML(node, status, intrinsic);
    if (!(node->m_flags & 0x4000)) {
        return;
    }
    auto scene = static_cast<XMLMODELSCENE*>(node);
    if (scene->m_sceneFlags & 1) {
        this->At<float>(0x374) = scene->m_fogNear;
    }
    if (scene->m_sceneFlags & 2) {
        this->At<float>(0x378) = scene->m_fogFar;
    }
    if (auto color = static_cast<XMLCOLOR*>(XMLObjectFindChild(node, XMLTYPE_FOGCOLOR))) {
        uint32_t argb = XmlColorPack(color);
        this->At<uint32_t>(0x370) = argb;
        this->At<uint32_t>(0x3a0) = argb;
        this->At<uint32_t>(0x3a4) = argb;
        this->At<uint32_t>(0x3a8) = argb;
    }
    if (scene->m_frameFlags & 0x100000) {
        uint32_t flags = this->At<uint32_t>(0x4a8);
        if ((flags & 0xf) != scene->m_drawLayer) {
            this->At<uint32_t>(0x4a8) = ((flags ^ scene->m_drawLayer) & 0xf) ^ flags;
            this->MarkBatchDirty();
        }
    }
    for (XMLOBJECT* child = node->m_child; child; child = child->m_next) {
        if (child->m_type == XMLTYPE_ACTORS) {
            for (XMLOBJECT* entry = child->m_child; entry; entry = entry->m_next) {
                void* block = ActorHeap().GetData(false, ".?AVModelSceneFrameActor@@", -2, 0x1f);
                uint64_t actor = 0;
                Guarded(0x141a95670, [&] { actor = block ? FUN_141a95670(Raw(block), Raw(this)) : 0; });
                // m_actors: data, count, capacity (top bit: not owned)
                uint64_t capacity = this->At<uint64_t>(0x368) & 0x7fffffffffffffffull;
                if (this->At<uint64_t>(0x360) == capacity) {
                    Guarded(0x141a97cd0, [&] { FUN_141a97cd0(Raw(&this->At<uint64_t>(0x358)), capacity + 1); });
                }
                reinterpret_cast<uint64_t*>(this->At<uint64_t>(0x358))[this->At<uint64_t>(0x360)] = actor;
                this->At<uint64_t>(0x360) += 1;
                // FUN_140643a20, the port's own over the actor's script object
                auto object = reinterpret_cast<FrameScript_Object*>(actor);
                Guarded(0x140643a20, [&] { object->LoadXML(entry, status, nullptr); });
                Guarded(0x141a935a0, [&] { FUN_141a935a0(actor, Raw(entry), Raw(status)); });
                Guarded(0x14060f390, [&] { FUN_14060f390(actor, Raw(entry), Raw(status)); });
            }
        } else if (child->m_type == XMLTYPE_VIEWINSETS) {
            float left = 0.0f;
            float right;
            float top;
            float bottom;
            XMLInsetGet(static_cast<XMLINSET*>(child), &left, &right, &top, &bottom, status);
            this->At<float>(0x4bc) = top;
            this->At<float>(0x4c0) = left;
            this->At<float>(0x4c4) = bottom;
            this->At<float>(0x4c8) = right;
        }
    }
}

// ref: FUN_141a96fe0
void ModelSceneFrame::RunOnUpdate(float elapsed) {
    // Ghidra typed the elapsed time as a 4-byte integer: it is passed as its bits
    uint32_t bits;
    std::memcpy(&bits, &elapsed, sizeof(bits));
    Guarded(0x141a96fe0, [&] { FUN_141a96fe0(Raw(this), bits); });
}

// ref: FUN_14026bea0
bool ModelSceneFrame::WantsUpdate() {
    return true;
}

// ref: FUN_141a96ee0
void ModelSceneFrame::AddRegionsToBatch(CRenderBatch* batch) {
    Guarded(0x141a96ee0, [&] { FUN_141a96ee0(Raw(this), Raw(batch)); });
}

// ref: FUN_141a96e70
void ModelSceneFrame::ScaleBy(float* scale) {
    auto layout = Raw(static_cast<CLayoutFrame*>(this));
    Guarded(0x141a96e70, [&] { FUN_141a96e70(layout, Raw(scale)); });
}

// ref: FUN_141a96f70
void ModelSceneFrame::OnFrameSizeChanged(const CRect* rect, const CRect* previous) {
    auto layout = Raw(static_cast<CLayoutFrame*>(this));
    Guarded(0x141a96f70, [&] { FUN_141a96f70(layout, Raw(rect), Raw(previous)); });
}

// ref: FUN_141a970a0
void ModelSceneFrame::Unknown08() {
    auto layout = Raw(static_cast<CLayoutFrame*>(this));
    Guarded(0x141a970a0, [&] { FUN_141a970a0(layout); });
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:GenFault=GenFaultMissing")
#pragma comment(linker, "/alternatename:FUN_14060f390=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141a93190=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141a935a0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141a95670=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141a96e70=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141a96ee0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141a96f70=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141a96fe0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141a970a0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141a97cd0=GenForwardMissing")
// ---- end of link fallbacks ----

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142d2f568 = (void*)&ModelSceneFrame::s_metaTable;
extern "C" void* const gen_p_142d2f56c = (void*)&ModelSceneFrame::s_restrictedMetaTable;
extern "C" void* const gen_p_143208508 = (void*)&s_modelSceneHeap;
// ---- end of generated globals ----
