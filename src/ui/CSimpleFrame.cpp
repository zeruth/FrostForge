#include "ui/CSimpleFrame.hpp"
#include "ui/CSimpleHTML.hpp"

#include <cmath>
#include <cstring>

#include "base/DataAllocator.hpp"
#include "base/Coordinate.hpp"
#include "base/Status.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/Taint.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "storm/Unicode.hpp"
#include "ui/BinaryXML.hpp"
#include "ui/CRenderBatch.hpp"
#include "ui/CScriptObjectScript.hpp"
#include "ui/CSimpleRegion.hpp"
#include "ui/CSimpleTop.hpp"
#include "ui/FrameXML.hpp"
#include "ui/UITypes.hpp"
#include "util/Unimplemented.hpp"

extern "C" {
#include "lauxlib.h"
#include "lblizzard.h"
#include "lua.h"
}

extern const char s_simpleFrameListTag[] = ".PEAVCSimpleFrame@@";
extern const char s_simpleRegionListTag[] = ".PEAVCSimpleRegion@@";

CSimpleFrame* CSimpleFrame::s_loading;
int64_t CSimpleFrame::s_count;
int32_t CSimpleFrame::s_metaTable = -2;
int32_t CSimpleFrame::s_restrictedMetaTable = -2;

namespace {

// The bounded copy the reference inlines: at most size - 1 characters, always terminated.
void CopyBounded(char* dest, const char* source, uint64_t size) {
    uint64_t i = 0;
    for (; i + 1 < size && source[i] != '\0'; i++) {
        dest[i] = source[i];
    }
    dest[i] = '\0';
}

const char s_attrNodeTag[] = ".?AV?$TSList@UFRAMEATTR@@V?$TSGetExplicitLink@UFRAMEATTR@@@@$07@@";
const char s_attrTableTag[] = ".?AV?$TSHashTable@UFRAMEATTR@@VHASHKEY_STRI@@$07@@";
const char s_childNodeTag[] = ".?AV?$TSList@USIMPLEFRAMENODE@@V?$TSGetLink@USIMPLEFRAMENODE@@@@$07@@";

// The frames' pool (0x142e97b38, made by the static initializer 0x1401df6b0).
CDataAllocator s_frameHeap(0x300, 100);

// The comparisons the reference makes through an abs mask (0x14261aa20) and these epsilons.
const float SCALE_EPSILON = 2.384185791015625e-07f;     // 0x14261a954
const float SIZE_EPSILON = 9.999999747378752e-06f;      // 0x1426c75c4
const float DRAG_THRESHOLD = 9.999999747378752e-05f;    // 0x1426c6f80, squared distance

// The hook RunOnLoad calls after the script (0x142cce848; a blz::function, never set here).
void (*s_loadHook)(CSimpleFrame* frame, void* context);
void* s_loadHookContext;

// Walks the regions. The next region is flagged first, so deleting it from inside the call
// waits until the walk is over (CSimpleRegion::FlushDeferredDeletes).
template <class Visit>
void ForEachRegion(CSimpleFrame* frame, Visit visit) {
    CSimpleRegion* region = frame->m_regions.Head();
    while (region != nullptr) {
        CSimpleRegion* next = frame->m_regions.Link(region)->Next();
        if (next != nullptr) {
            next->m_regionFlags |= CSimpleRegion::REGIONFLAG_ITERATING;
        }
        region->m_regionFlags &= ~CSimpleRegion::REGIONFLAG_ITERATING;
        visit(region);
        region = next;
    }
    CSimpleRegion::FlushDeferredDeletes();
}

template <class Visit>
void ForEachChild(CSimpleFrame* frame, Visit visit) {
    SIMPLEFRAMENODE* node = frame->m_children.Head();
    while (node != nullptr) {
        SIMPLEFRAMENODE* next = frame->m_children.Link(node)->Next();
        visit(node->m_frame);
        node = next;
    }
}

// CSimpleHTML's type id (0x142e97b68), made here by the reference's magic static.
int32_t CSimpleHTML_GetObjectType() {
    return CSimpleHTML::GetObjectType();
}

bool HasOwnBatch(CSimpleFrame* frame) {
    return (frame->m_frameFlags & CSimpleFrame::FRAME_OWN_BATCH) || (frame->m_flags2 & CSimpleFrame::FRAME2_FRAME_BUFFER);
}

const char* DisplayName(CSimpleFrame* frame) {
    const char* name = frame->GetName();
    return name != nullptr ? name : frame->GetObjectTypeName();
}

// The <KeyValues>-style typed value of an <Attribute>.
void PushAttributeValue(lua_State* L, const char* type, const char* value) {
    if (_stricmp(type, "nil") == 0) {
        lua_pushnil(L);
    } else if (_stricmp(type, "boolean") == 0) {
        lua_pushboolean(L, StringToBOOL(value));
    } else if (_stricmp(type, "number") == 0) {
        lua_pushnumber(L, static_cast<double>(SStrToFloat(value)));
    } else {
        lua_pushstring(L, value);
    }
}

}

// ---- FRAMEATTRTABLE --------------------------------------------------------------------

// The node out of the table, its name freed, both links undone (FUN_140613680), then freed.
// ref: FUN_140616d60
void FRAMEATTRTABLE::Delete(FRAMEATTR* node) {
    if (node->m_table != nullptr && node->m_linktoslot.m_next != nullptr) {
        node->m_linktoslot.Unlink();
        node->m_linktofull.Unlink();
        node->m_table = nullptr;
        static_cast<FRAMEATTRTABLE*>(this)->m_count--;
    }
    SMemFree(node->m_str, nullptr, 0, 0);
    node->m_str = nullptr;
    node->m_linktofull.Unlink();
    node->m_linktoslot.Unlink();
    SMemFree(node, s_attrTableTag, -2, 0);
}

// ref: FUN_140616d90
FRAMEATTR* FRAMEATTRTABLE::NewNode(TSExplicitList<FRAMEATTR>* slot, uint32_t extra, uint32_t flags) {
    auto node = static_cast<FRAMEATTR*>(SMemAlloc(uint64_t(extra) + sizeof(FRAMEATTR), 8, s_attrNodeTag, 0, -2, flags | SMEM_FLAG_ZEROMEMORY));
    node->m_linktoslot.m_prevlink = nullptr;
    node->m_linktoslot.m_next = nullptr;
    node->m_linktofull.m_prevlink = nullptr;
    node->m_linktofull.m_next = nullptr;
    node->m_table = nullptr;
    node->m_hashval = 0;
    node->m_str = nullptr;
    node->m_ref = 0;
    node->m_taint = nullptr;
    slot->LinkToHead(node);
    return node;
}

// ref: FUN_140619e80
FRAMEATTR* FRAMEATTRTABLE::Find(const char* name) {
    if (this->m_slotmod == 0xFFFFFFFF) {
        return nullptr;
    }
    uint32_t hash = SStrHashHT(name);
    TSExplicitList<FRAMEATTR>* slot = &this->m_slots[hash % this->m_slotmod];
    for (FRAMEATTR* node = slot->Head(); node != nullptr; node = slot->Link(node)->Next()) {
        if (node->m_hashval == hash && _stricmp(node->m_str, name) == 0) {
            return node;
        }
    }
    return nullptr;
}

// ref: FUN_140618820
FRAMEATTR* FRAMEATTRTABLE::Add(const char* name, uint32_t extra, uint32_t flags) {
    uint32_t hash = SStrHashHT(name);
    FRAMEATTR* node = this->New(hash, extra, flags);
    node->m_hashval = hash;
    if (node->m_str != name) {
        SMemFree(node->m_str, nullptr, 0, 0);
        node->m_str = SStrDupA(name, nullptr, 0);
    }
    return node;
}

// ---- construction ----------------------------------------------------------------------

// ref: FUN_140613290
CSimpleFrame::CSimpleFrame(CSimpleFrame* parent) {
    this->m_scale = 1.0f;
    this->m_depth = 0.0f;
    this->m_id = 0;
    this->m_frameFlags = 0;
    this->m_drawRegions.m_terminator.m_next = nullptr;
    this->m_drawRegions.InitEmpty(0x200);
    this->m_children.m_terminator.m_next = nullptr;
    this->m_children.InitEmpty(0);
    this->m_regions.m_terminator.m_next = nullptr;
    this->m_regions.InitEmpty(0x1f0);
    this->m_effectiveDepth = 1.0f;
    this->m_frameBuffer = nullptr;
    this->m_dragButtons = 0;
    this->m_dragStartX = 0.0f;
    this->m_dragStartY = 0.0f;
    this->m_buttonsDown = 0;
    this->m_hitRectInsets = {};
    this->m_clampRectInsets = {};
    this->m_alpha = 0xff;
    this->m_parentAlpha = 0xff;
    this->m_level = 0;
    this->m_raiseOrder = 0;
    this->m_flags2 = (this->m_flags2 & 0xffffc19f) | 0x180;
    s_count++;
    CSimpleTop::s_instance->AddRootFrame(this);
    PARENTARG arg = {parent, true};
    this->CSimpleFrame::SetParentArg(&arg);
    if (parent == nullptr) {
        uint32_t flags = this->m_frameFlags;
        if (!(flags & FRAME_OWN_BATCH) && !(this->m_flags2 & FRAME2_FRAME_BUFFER)) {
            this->MarkParentBatchDirty();
        } else if (!(flags & FRAME_BATCH_DIRTY)) {
            this->m_frameFlags = flags | FRAME_BATCH_DIRTY;
            if (this->m_layoutState & LAYOUT_VISIBLE) {
                CSimpleTop::s_instance->AddPendingBatchUpdate(this);
            }
        }
    } else {
        this->SetSourceLocation(parent->GetScriptLocation());
    }
    this->m_flags2 = (this->m_flags2 | 0xf) & ~0x10u;
    if (!this->CanQueryRect() && CSimpleTop::s_instance->m_actionBlockedHook != nullptr) {
        CSimpleTop::s_instance->m_actionBlockedHook(this);
    }
    this->m_layoutState |= LAYOUT_SHOWN;
    this->CSimpleFrame::ApplyShow();
}

// ref: FUN_140613850
CSimpleFrame::~CSimpleFrame() {
    this->m_regions.UnlinkAll();
    this->m_regions.m_terminator.Unlink();
    // FUN_14025cfe0
    this->m_children.UnlinkAll();
    this->m_children.m_terminator.Unlink();
    this->m_drawRegions.UnlinkAll();
    this->m_drawRegions.m_terminator.Unlink();
    this->m_rootLink.Unlink();
    this->m_updateLink.Unlink();
}

void* CSimpleFrame::operator new(size_t size) {
    (void)size;
    return s_frameHeap.GetData(false, ".?AVCSimpleFrame@@", -2, 0x1f);
}

// ref: FUN_140613aa0
void CSimpleFrame::operator delete(void* frame) {
    if (frame != nullptr) {
        s_frameHeap.PutData(frame);
    }
}

// ref: FUN_1405ee7c0
// The reference's type id and its magic-static guard (CSimpleFrame), held here so translated code
// and the port number the class once, alike.
int32_t g_cSimpleFrameType;  // DAT_142e979f4
int32_t g_cSimpleFrameTypeGuard;  // DAT_142e979f8

int32_t CSimpleFrame::GetObjectType() {
    return FrameScript_TypeId(&g_cSimpleFrameType, &g_cSimpleFrameTypeGuard);
}

// ref: FUN_140615200
void CSimpleFrame::CreateScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    s_metaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleFrame_FillScriptMethods, false);
    s_restrictedMetaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleFrame_FillScriptMethods, true);
}

// ref: FUN_140615510
void CSimpleFrame::DestroyScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    luaL_unref(L, LUA_REGISTRYINDEX, s_metaTable);
    luaL_unref(L, LUA_REGISTRYINDEX, s_restrictedMetaTable);
    s_metaTable = -2;
    s_restrictedMetaTable = -2;
}

// Input off first, then the regions, the children and the attributes; the frame goes last.
// ref: FUN_1406153d0
void CSimpleFrame::Destroy() {
    this->SetInputFlag(REGION_PROPAGATE_MOUSE, true);
    this->Unknown280();
    CSimpleTop* top = CSimpleTop::s_instance;
    top->RemoveFrameBatch(this);
    this->m_frameFlags &= ~FRAME_BATCH_DIRTY;
    top->RemoveFrame(this);
    while (CSimpleRegion* region = this->m_regions.Head()) {
        region->SetInputFlag(REGION_PROPAGATE_MOUSE, true);
        delete region;
    }
    this->m_drawRegions.UnlinkAll();
    while (SIMPLEFRAMENODE* node = this->m_children.Head()) {
        node->m_frame->Destroy();
    }
    if (this->m_parent != nullptr) {
        this->m_parent->RemoveChild(this);
    }
    this->m_attributes.m_fulllist.UnlinkAll();
    for (uint32_t i = 0; i < this->m_attributes.m_slotcount; i++) {
        while (FRAMEATTR* attr = this->m_attributes.m_slots[i].Head()) {
            this->m_attributes.Delete(attr);
        }
    }
    this->m_attributes.m_count = 0;
    s_count--;
    this->m_frameFlags |= FRAME_DESTROYED;
    delete this;
}

// ---- FrameScript_Object / CScriptObject -------------------------------------------------

// ref: FUN_1406165f0
int32_t CSimpleFrame::GetScriptMetaTable() {
    if (!(this->FrameScript_Object::m_flags & FSOBJ_FORBIDDEN) && !(this->FrameScript_Object::m_flags & FSOBJ_PROTECTED_METATABLE)) {
        return s_metaTable;
    }
    return s_restrictedMetaTable;
}

// An OnUpdate handler puts the frame on the update list.
// ref: FUN_14061a870
void CSimpleFrame::OnScriptsChanged(uint8_t index, bool hasScript) {
    if (index != 10) {
        return;
    }
    if (hasScript) {
        this->m_frameFlags |= FRAME_HAS_ONUPDATE;
    } else {
        this->m_frameFlags &= ~FRAME_HAS_ONUPDATE;
    }
    this->UpdateUpdateRegistration();
}

// The handlers a frame adds to a region's (and the arguments each gets).
// ref: FUN_14061bfd0
bool CSimpleFrame::GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) {
    switch (index) {
        case 9:
            data->m_wrapper = "return function(self,w,h) %s end";
            return true;
        case 10:
            data->m_wrapper = "return function(self,elapsed) %s end";
            return true;
        case 0x13:
            data->m_wrapper = "return function(self,button) %s end";
            return true;
        case 0x14:
        case 0x15:
        case 0x19:
        case 0x21:
        case 0x22:
            return true;
        case 0x16:
            data->m_update = 0;
            data->m_wrapper = "return function(self,text) %s end";
            return true;
        case 0x17:
            data->m_wrapper = "return function(self,link,text,button,region,left,bottom,width,height) %s end";
            return true;
        case 0x18:
            data->m_wrapper = "return function(self,link,text,region,left,bottom,width,height) %s end";
            return true;
        case 0x1a:
            data->m_wrapper = "return function(self,event,...) %s end";
            return true;
        case 0x1b:
        case 0x1c:
            data->m_update = 1;
            data->m_wrapper = "return function(self,key) %s end";
            return true;
        case 0x1d:
        case 0x1e:
            data->m_update = 5;
            data->m_wrapper = "return function(self,button) %s end";
            return true;
        case 0x1f:
            data->m_update = 6;
            data->m_wrapper = "return function(self,stick,x,y,len) %s end";
            return true;
        case 0x20:
            data->m_wrapper = "return function(self,name,value) %s end";
            return true;
        default:
            return this->CScriptRegion::GetScriptData(index, data);
    }
}

// ref: FUN_140616620
void CSimpleFrame::GetScriptCPUUsage(lua_State* L, double* time, int32_t* calls, bool includeChildren) {
    this->FrameScript_Object::GetScriptCPUUsage(L, time, calls, includeChildren);
    if (!includeChildren) {
        return;
    }
    ForEachChild(this, [&](CSimpleFrame* child) {
        if (!(static_cast<FrameScript_Object*>(child)->m_flags & FSOBJ_HIDDEN)) {
            child->GetScriptCPUUsage(L, time, calls, true);
        }
    });
}

// ref: FUN_140618f00
void CSimpleFrame::OnProtected() {
    this->CScriptRegion::OnProtected();
    ForEachChild(this, [](CSimpleFrame* child) {
        child->FrameScript_Object::SetProtected();
    });
    ForEachRegion(this, [](CSimpleRegion* region) {
        region->FrameScript_Object::SetProtected();
    });
}

// ref: FUN_140617110
bool CSimpleFrame::IsObjectTypeName(const char* name) {
    return _stricmp(name, "Frame") == 0 || _stricmp(name, "Region") == 0 || _stricmp(name, "Object") == 0
        || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_140617070
bool CSimpleFrame::IsObjectType(int32_t type) {
    return type == CSimpleFrame::GetObjectType() || this->CScriptRegion::IsObjectType(type);
}

// ref: FUN_140616490
const char* CSimpleFrame::GetObjectTypeName() {
    return "Frame";
}

// ref: FUN_140616340
uint32_t CSimpleFrame::GetObjectSize() {
    return 0x300;
}

// ref: FUN_1406149c0
CAnimatableObject* CSimpleFrame::GetAnimatableObject() {
    return this;
}

// ref: FUN_1406155d0
void CSimpleFrame::DumpHierarchy(uint32_t indent, HIERARCHYDUMPFUNC dump, void* context) {
    char line[0x80] = {};
    if (indent != 0) {
        memset(line, '.', indent);
    }
    alignas(blz::string) uint8_t storage[sizeof(blz::string)];
    blz::string* name = this->GetDebugName(reinterpret_cast<blz::string*>(storage), false);
    // FUN_140613260
    int32_t length = static_cast<int32_t>(SStrPrintf(line, 0x80, "Frame %s\n", name->m_data));
    name->~string();
    dump(line, length + static_cast<int32_t>(indent), context);
    ForEachRegion(this, [&](CSimpleRegion* region) {
        region->DumpHierarchy(indent + 1, dump, context);
    });
    ForEachChild(this, [&](CSimpleFrame* child) {
        child->DumpHierarchy(indent + 1, dump, context);
    });
}

// ref: FUN_140615760
void CSimpleFrame::Unknown98(void* argument) {
    ForEachRegion(this, [&](CSimpleRegion* region) {
        region->Unknown98(argument);
    });
}

// ---- XML -------------------------------------------------------------------------------

namespace {

// Loads an inherited template into the frame under the taint it was defined with; outside a
// secure load a tainted template's frame keeps loading plainly.
void LoadInherited(CSimpleFrame* frame, XMLOBJECT* node, const char* taint, CStatus* status) {
    lua_State* L = FrameScript_GetContext();
    if (CSimpleFrame::s_loading == nullptr || taint != nullptr || CSimpleTop::s_instance->IsSecureLoad()) {
        const char* previous = lua_taint;
        FrameScript_EnterTaint(L, taint);
        const char* objectTaint = lua_objectTaint;
        lua_objectTaint = taint;
        frame->LoadXML(node, status, nullptr);
        lua_objectTaint = objectTaint;
        FrameScript_LeaveTaint(L, previous);
        return;
    }
    frame->LoadXML(node, status, nullptr);
}

}

// The intrinsic type and inherited templates first, then the region settings and the frame's
// own: toplevel, movable, strata, level, alpha, scale, depth and the rest, then attributes,
// size bounds, hit insets, layers, scripts, child frames and animations.
// ref: FUN_140617390
void CSimpleFrame::LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) {
    if (!(node->m_flags & 0x100)) {
        return;
    }
    auto frame = static_cast<XMLFRAME*>(node);
    auto scriptStatus = static_cast<CScriptStatus*>(status);
    if (intrinsic != nullptr) {
        char name[0x80];
        CopyBounded(name, intrinsic, sizeof(name));
        const char* taint = nullptr;
        bool recursive = true;
        XMLOBJECT* found = FrameXML_AcquireIntrinsic(name, &taint, &recursive);
        if (found != nullptr) {
            if (!recursive) {
                LoadInherited(this, found, taint, status);
            } else {
                XMLStatusAddAt(scriptStatus, node, 1, "Multiply inherited intrinsic node: %s", node->m_tag.m_text);
            }
            if (!recursive) {
                FrameXML_ReleaseIntrinsic(name);
            }
        }
    }
    if (node->m_attributes & 0x100) {
        const char* cursor = node->m_inherits.m_text;
        char token[0x440];
        do {
            SStrTokenize(&cursor, token, 0x400, " ,", nullptr);
            if (token[0] == '\0') {
                break;
            }
            char name[0x80];
            CopyBounded(name, token, sizeof(name));
            const char* taint = nullptr;
            bool busy = true;
            XMLOBJECT* parent = FrameXML_AcquireVirtual(name, &taint, &busy);
            if (parent == nullptr) {
                XMLStatusAddAt(scriptStatus, node, 1, "Couldn't find inherited node: %s", name);
            } else if (busy) {
                XMLStatusAddAt(scriptStatus, node, 1, "Recursively inherited node: %s", name);
            } else {
                LoadInherited(this, parent, taint, status);
            }
            if (parent != nullptr && !busy) {
                FrameXML_ReleaseVirtual(name);
            }
        } while (token[0] != '\0');
    }
    this->CScriptRegion::LoadXML(node, status, intrinsic);
    if ((node->m_attributes & 0x80) && s_loading == nullptr) {
        FrameXML_SetSecureReferenceKey(node->m_secureReferenceKey.m_text, this);
    }
    uint32_t regionFlags = frame->m_regionFlags;
    if (regionFlags & 2) {
        if (regionFlags & 0x10) {
            this->Hide();
        } else {
            this->Show();
        }
    }
    uint32_t flags = frame->m_frameFlags;
    if (flags & 0x100) {
        bool toplevel = (flags >> 9) & 1;
        if ((this->m_frameFlags & FRAME_TOPLEVEL) != (toplevel ? FRAME_TOPLEVEL : 0u)) {
            this->m_frameFlags = toplevel ? (this->m_frameFlags | FRAME_TOPLEVEL) : (this->m_frameFlags & ~FRAME_TOPLEVEL);
            this->UpdateOwnBatch();
        }
    }
    if (flags & 0x400) {
        this->SetMovable((flags >> 11) & 1);
    }
    if (flags & 0x1000) {
        this->SetResizable((flags >> 13) & 1);
    }
    if (flags & 0x4000) {
        this->SetFrameStrata(frame->m_strata);
    }
    if (flags & 0x200000) {
        if (!(flags & 0x400000)) {
            this->m_frameFlags &= ~FRAME_USE_PARENT_LEVEL;
            if (this->m_parent != nullptr) {
                this->SetFrameLevel(static_cast<uint16_t>(this->m_parent->m_level + 1));
            }
        } else {
            if (this->m_parent != nullptr) {
                this->SetFrameLevel(this->m_parent->m_level);
            }
            this->m_frameFlags |= FRAME_USE_PARENT_LEVEL;
        }
    }
    if (flags & 1) {
        this->SetFrameLevel(static_cast<uint16_t>(frame->m_frameLevel));
    }
    uint32_t flags2 = frame->m_frameFlags2;
    if (flags2 & 0x80) {
        this->SetFrameFlag(FRAME_FIXED_STRATA, (flags2 >> 8) & 1);
    }
    if (flags2 & 0x200) {
        this->SetFrameFlag(FRAME_FIXED_LEVEL, (flags2 >> 10) & 1);
    }
    if (regionFlags & 1) {
        float alpha = frame->m_alpha;
        if (alpha >= 1.0f) {
            alpha = 1.0f;
        }
        if (alpha <= 0.0f) {
            alpha = 0.0f;
        }
        this->SetAlpha(static_cast<uint8_t>(static_cast<int32_t>(alpha * 255.0f)));
    }
    if ((flags & 0x18000) == 0x18000) {
        this->EnableInputEvent(1);
        this->EnableInputEvent(0);
    }
    if (flags & 2) {
        this->CLayoutFrame::SetClampedToScreen((flags >> 2) & 1);
    }
    if ((flags & 0x80) && SCALE_EPSILON <= fabsf(frame->m_depth - this->m_depth)) {
        float depth = this->m_effectiveDepth;
        this->m_depth = frame->m_depth;
        if (SCALE_EPSILON <= fabsf(depth - this->m_layoutDepth)) {
            this->CLayoutFrame::SetLayoutDepth(depth, false);
            ForEachRegion(this, [&](CSimpleRegion* region) {
                region->CLayoutFrame::SetLayoutDepth(depth, false);
            });
            ForEachChild(this, [&](CSimpleFrame* child) {
                if (SCALE_EPSILON <= fabsf(depth - child->m_effectiveDepth)) {
                    child->m_effectiveDepth = depth;
                    child->UpdateDepth(false);
                }
            });
        }
    }
    if (flags & 0x800000) {
        this->SetFrameFlag(FRAME_DONT_SAVE_POSITION, (flags >> 24) & 1);
    }
    if (flags & 0x2000000) {
        this->SetFrameFlag(FRAME_PROPAGATE_KEYBOARD, (flags >> 26) & 1);
    }
    if (flags & 0x8000000) {
        this->SetIgnoreParentAlpha((flags >> 28) & 1);
    }
    if (flags2 & 0x20) {
        if (flags2 & 0x40) {
            this->m_frameFlags |= FRAME_IGNORE_PARENT_SCALE;
        } else {
            this->m_frameFlags &= ~FRAME_IGNORE_PARENT_SCALE;
        }
        this->UpdateScale(true);
    }
    if (flags & 0x20000000) {
        this->SetFlattenRenderLayers((flags >> 30) & 1);
    }
    if (flags & 0x80000000) {
        bool clip = flags2 & 1;
        if (((this->m_frameFlags & FRAME_CLIP_CHILDREN) != 0) != clip) {
            this->m_frameFlags = clip ? (this->m_frameFlags | FRAME_CLIP_CHILDREN) : (this->m_frameFlags & ~FRAME_CLIP_CHILDREN);
            this->UpdateOwnBatch();
        }
    }
    if (flags2 & 2) {
        this->SetHyperlinksEnabled((flags2 >> 2) & 1);
    }
    if (flags2 & 8) {
        bool propagate = (flags2 >> 4) & 1;
        if (((this->m_frameFlags & FRAME_PROPAGATE_HYPERLINKS) != 0) != propagate) {
            this->m_frameFlags = propagate ? (this->m_frameFlags | FRAME_PROPAGATE_HYPERLINKS)
                                           : (this->m_frameFlags & ~FRAME_PROPAGATE_HYPERLINKS);
            this->UpdateClickRegistration();
            this->UpdateMotionRegistration();
        }
    }
    if (regionFlags & 0x200) {
        float scale = frame->m_scale;
        if (scale > 0.0f) {
            if (SCALE_EPSILON <= fabsf(scale - this->m_scale)) {
                this->m_scale = scale;
                this->UpdateScale(false);
            }
        } else {
            FrameScript_ActionError("Scale must be > 0");
        }
    }
    if (flags2 & 0x800) {
        this->SetIsFrameBuffer((flags2 >> 12) & 1);
    }
    if (regionFlags & 0x10000) {
        const char* cursor = frame->m_registerForDrag.m_text;
        char token[0x400];
        uint32_t buttons = 0;
        SStrTokenize(&cursor, token, sizeof(token), " ,", nullptr);
        while (token[0] != '\0') {
            buttons |= StringToMouseButton(token);
            SStrTokenize(&cursor, token, sizeof(token), " ,", nullptr);
        }
        this->m_dragButtons = buttons;
    }
    for (XMLOBJECT* child = node->m_child; child != nullptr; child = child->m_next) {
        switch (child->m_type) {
            case XMLTYPE_ATTRIBUTES:
                this->LoadXMLAttributes(child, status);
                break;
            case XMLTYPE_RESIZEBOUNDS: {
                float minWidth = 0.0f;
                float minHeight = 0.0f;
                float maxWidth = 0.0f;
                float maxHeight = 0.0f;
                XMLOBJECT* minimum = XMLObjectFindChild(child, XMLTYPE_MINRESIZE);
                if (minimum != nullptr) {
                    XMLDimensionGet(static_cast<XMLDIMENSION*>(minimum), &minWidth, &minHeight, status);
                }
                XMLOBJECT* maximum = XMLObjectFindChild(child, XMLTYPE_MAXRESIZE);
                if (maximum != nullptr) {
                    XMLDimensionGet(static_cast<XMLDIMENSION*>(maximum), &maxWidth, &maxHeight, status);
                }
                this->m_minResizeHeight = minHeight;
                this->m_minResizeWidth = minWidth;
                this->m_maxResizeHeight = maxHeight;
                this->m_maxResizeWidth = maxWidth;
                break;
            }
            case XMLTYPE_HITRECTINSETS: {
                float left;
                float right;
                float top;
                float bottom;
                if (XMLInsetGet(static_cast<XMLINSET*>(child), &left, &right, &top, &bottom, status)) {
                    this->m_hitRectInsets.minY = bottom;
                    this->m_hitRectInsets.minX = left;
                    this->m_hitRectInsets.maxY = top;
                    this->m_hitRectInsets.maxX = right;
                }
                break;
            }
            case XMLTYPE_LAYERS:
                this->LoadXMLLayers(child, status);
                break;
            case XMLTYPE_SCRIPTS:
                this->LoadXMLScripts(child, status, (node->m_attributes & 0x420) != 0x420);
                break;
            case XMLTYPE_FRAMES:
                for (XMLOBJECT* frameNode = child->m_child; frameNode != nullptr; frameNode = frameNode->m_next) {
                    FrameXML_CreateFrame(frameNode, this, scriptStatus);
                }
                break;
            default:
                break;
        }
    }
    CSimpleFrame_LoadAnimations(this, node, status);
}

// The frame's name and id, and the loading mark that holds OnShow back.
// ref: FUN_140619d00
void CSimpleFrame::LoadXMLName(XMLOBJECT* node, CStatus* status) {
    (void)status;
    if (node->m_attributes & 0x40) {
        this->SetName(node->m_name.m_text);
    }
    if (static_cast<XMLFRAME*>(node)->m_frameFlags & 0x80000) {
        this->m_id = static_cast<XMLFRAME*>(node)->m_id;
    }
    this->FrameScript_Object::m_flags |= FSOBJ_LOADING;
}

// Loading is over: a visible frame's regions get their layout, animations started on the
// frame while it loaded take hold, then the region half runs OnLoad.
// ref: FUN_140619a40
void CSimpleFrame::PostLoadXML(XMLOBJECT* node, CStatus* status) {
    this->FrameScript_Object::m_flags &= ~FSOBJ_LOADING;
    if (this->m_layoutState & LAYOUT_VISIBLE) {
        this->CLayoutFrame::m_flags |= FLAG_DIRTY;
        ForEachRegion(this, [](CSimpleRegion* region) {
            if (region->m_layoutState & LAYOUT_VISIBLE) {
                region->CLayoutFrame::m_flags |= FLAG_DIRTY;
            }
        });
    }
    bool wanted = (this->m_layoutState & LAYOUT_VISIBLE) && (this->WantsUpdate() || (this->m_frameFlags & FRAME_HAS_ONUPDATE));
    if (wanted != this->m_updateLink.IsLinked()) {
        CSimpleTop::s_instance->AddUpdateCheck(this);
    }
    if (this->IsPlaying()) {
        bool alpha = this->IsAnimatingAlpha();
        bool transform = this->IsAnimatingTransform();
        bool rectOffset = this->IsAnimatingTransform();
        this->CLayoutFrame::Resize(true);
        if (alpha) {
            this->CLayoutFrame::SetAnimAlpha(this->m_alpha);
        }
        ForEachRegion(this, [&](CSimpleRegion* region) {
            region->OnAnimStarted(alpha, transform, rectOffset);
        });
    }
    this->CScriptRegion::PostLoadXML(node, status);
}

// ref: FUN_140618210
void CSimpleFrame::LoadXMLAttributes(XMLOBJECT* node, CStatus* status) {
    lua_State* L = FrameScript_GetContext();
    auto scriptStatus = static_cast<CScriptStatus*>(status);
    for (XMLOBJECT* child = node->m_child; child != nullptr; child = child->m_next) {
        if (child->m_type != XMLTYPE_ATTRIBUTE) {
            XMLStatusAddAt(scriptStatus, child, 1, "Frame %s: Unknown attributes element %s", DisplayName(this), child->m_tag.m_text);
            continue;
        }
        auto attribute = static_cast<XMLATTRIBUTE*>(child);
        if (!(child->m_attributes & 0x40)) {
            XMLStatusAddAt(scriptStatus, child, 1, "Frame %s: unnamed attribute element", DisplayName(this));
            continue;
        }
        const char* type = (attribute->m_attributeFlags & 2) ? attribute->m_valueType.m_text : "string";
        const char* name = child->m_name.m_text;
        if (!(attribute->m_attributeFlags & 1) && _stricmp(type, "nil") != 0) {
            XMLStatusAddAt(scriptStatus, child, 1, "Frame %s: attribute element named %s missing value", DisplayName(this), name);
            continue;
        }
        PushAttributeValue(L, type, attribute->m_value.m_text);
        FRAMEATTR* attr = this->m_attributes.Find(name);
        if (attr == nullptr) {
            attr = this->m_attributes.Add(name, 0, 0);
        } else {
            luaL_unref(L, LUA_REGISTRYINDEX, attr->m_ref);
        }
        const char* previous = lua_taint;
        FrameScript_EnterTaint(L, nullptr);
        lua_settaint(L, -1, nullptr);
        attr->m_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        attr->m_taint = previous;
        FrameScript_LeaveTaint(L, previous);
    }
}

// Each <Layer>'s font strings, textures, mask textures and lines, in its level and sublevel.
// ref: FUN_140618500
void CSimpleFrame::LoadXMLLayers(XMLOBJECT* node, CStatus* status) {
    auto scriptStatus = static_cast<CScriptStatus*>(status);
    for (XMLOBJECT* layer = node->m_child; layer != nullptr; layer = layer->m_next) {
        if (layer->m_type != XMLTYPE_LAYER) {
            XMLStatusAddAt(scriptStatus, layer, 1, "Frame %s: Unknown child node in %s element: %s", DisplayName(this),
                node->m_tag.m_text, layer->m_tag.m_text);
            continue;
        }
        auto layerNode = static_cast<XMLLAYER*>(layer);
        uint8_t level = static_cast<uint8_t>(layerNode->m_level);
        int8_t subLevel = (layerNode->m_layerFlags & 2) ? layerNode->m_subLevel : 0;
        for (XMLOBJECT* child = layer->m_child; child != nullptr; child = child->m_next) {
            CSimpleRegion* region;
            int8_t regionSubLevel = subLevel;
            switch (child->m_type) {
                case XMLTYPE_FONTSTRING:
                    region = FrameXML_CreateFontString(child, this, scriptStatus);
                    regionSubLevel = 0;
                    break;
                case XMLTYPE_TEXTURE:
                    region = FrameXML_CreateTexture(child, this, scriptStatus);
                    break;
                case XMLTYPE_MASKTEXTURE:
                    region = FrameXML_CreateMaskTexture(child, this, scriptStatus);
                    break;
                case XMLTYPE_LINE:
                    region = FrameXML_CreateLine(child, this, scriptStatus);
                    break;
                default:
                    XMLStatusAddAt(scriptStatus, layer, 1, "Frame %s: Unknown child node in %s element: %s", DisplayName(this),
                        layer->m_tag.m_text, child->m_tag.m_text);
                    continue;
            }
            if (region != nullptr) {
                region->SetFrame(this, level, regionSubLevel, (region->m_layoutState & LAYOUT_SHOWN) != 0);
            }
        }
    }
}

// ---- hierarchy -----------------------------------------------------------------------------

// ref: FUN_14061b4b0
void CSimpleFrame::SetParentArg(PARENTARG* parent) {
    CSimpleFrame* frame = parent->m_set ? parent->m_frame : nullptr;
    if (frame == this->m_parent) {
        parent->m_set = false;
        return;
    }
    bool wasShown = (this->m_layoutState & LAYOUT_SHOWN) != 0;
    CSimpleFrame* old = this->m_parent;
    if (old == nullptr) {
        CSimpleTop::s_instance->MarkStrataDirty();
    } else {
        this->CLayoutFrame::PropagateAnyProtectionTo(old);
        old->RemoveChild(this);
    }
    PARENTARG arg = {frame, true};
    this->CScriptRegion::SetParentArg(&arg);
    CSimpleFrame* now = this->m_parent;
    if (now == nullptr) {
        this->SetFrameStrata(3);
        this->SetFrameLevel(0);
        this->SetRaiseOrder(0);
        this->UpdateScale(false);
        this->UpdateDepth(false);
        this->m_frameFlags &= ~FRAME_IN_SCROLL_FRAME;
        ForEachChild(this, [](CSimpleFrame* child) {
            if (!(child->m_frameFlags & FRAME_SCROLL_CHILD)) {
                child->SetInScrollFrame(false, 0);
            }
        });
        this->UpdateOwnBatch();
        if (!(this->m_frameFlags & FRAME_IGNORE_PARENT_ALPHA) && this->m_parentAlpha != 0xff) {
            this->m_parentAlpha = 0xff;
            this->UpdateEffectiveAlpha();
        }
        if (this->IsBatched()) {
            this->m_frameFlags &= ~FRAME_BATCHED;
            this->UpdateBatched();
        }
    } else {
        this->SetFrameStrata((now->m_flags2 >> 7) & 0xf);
        this->SetFrameLevel(static_cast<uint16_t>(now->m_level + 1));
        this->SetRaiseOrder(now->m_raiseOrder);
        this->UpdateScale(false);
        this->UpdateDepth(false);
        this->SetInScrollFrame(now->IsInScrollFrame(), 0);
        uint8_t alpha = now->GetEffectiveAlpha();
        if (!(this->m_frameFlags & FRAME_IGNORE_PARENT_ALPHA) && alpha != this->m_parentAlpha) {
            this->m_parentAlpha = alpha;
            this->UpdateEffectiveAlpha();
        }
        this->SetInherited200((now->m_frameFlags >> 9) & 1);
        bool batched = HasOwnBatch(now) || now->IsBatched();
        if (this->IsBatched() != batched) {
            this->m_frameFlags = batched ? (this->m_frameFlags | FRAME_BATCHED) : (this->m_frameFlags & ~FRAME_BATCHED);
            this->UpdateBatched();
        }
    }
    this->UpdateOwnBatch();
    this->MarkBatchDirty();
    this->UpdateClickRegistration();
    this->UpdateMotionRegistration();
    if (this->m_parent != nullptr) {
        this->CLayoutFrame::PropagateProtectionTo(this->m_parent);
        this->m_parent->AddChild(this);
    }
    if (!wasShown || (this->m_parent != nullptr && !(this->m_parent->m_layoutState & LAYOUT_VISIBLE))) {
        this->ApplyHide();
    } else {
        this->ApplyShow();
    }
    parent->m_set = false;
}

void CSimpleFrame::SetParent(CSimpleFrame* parent) {
    PARENTARG arg = {parent, true};
    this->SetParentArg(&arg);
}

// ref: FUN_14061a020
void CSimpleFrame::AddRegion(CSimpleRegion* region) {
    this->m_regions.LinkToTail(region);
}

// ref: FUN_14061c340
void CSimpleFrame::RemoveRegion(CSimpleRegion* region) {
    this->m_regions.Link(region)->Unlink();
}

// A region becomes visible: it takes the frame's scale and depth and joins the drawing.
// ref: FUN_140614300
void CSimpleFrame::AddDrawRegion(CSimpleRegion* region) {
    float scale = (region->m_regionFlags & CSimpleRegion::REGIONFLAG_IGNORE_PARENT_SCALE) ? 1.0f : this->m_layoutScale;
    region->SetLayoutScale(scale, false);
    region->CLayoutFrame::SetLayoutDepth(this->m_layoutDepth, false);
    this->m_drawRegions.LinkToTail(region);
    this->MarkBatchDirty();
    this->MarkScrollRangeDirty();
}

// ref: FUN_14061a0a0
void CSimpleFrame::RemoveDrawRegion(CSimpleRegion* region) {
    this->m_drawRegions.Link(region)->Unlink();
    this->MarkBatchDirty();
    this->MarkScrollRangeDirty();
}

// A child joins the list; it shares the frame-buffer frame unless it is one itself.
// ref: FUN_140619960
void CSimpleFrame::AddChild(CSimpleFrame* child) {
    auto node = static_cast<SIMPLEFRAMENODE*>(SMemAlloc(sizeof(SIMPLEFRAMENODE), 8, s_childNodeTag, 0, -2, 8));
    node->m_link.m_prevlink = nullptr;
    node->m_link.m_next = nullptr;
    this->m_children.LinkToTail(node);
    node->m_frame = child;
    MemoryWatchBlock(node, true);
    if (!(child->m_flags2 & FRAME2_FRAME_BUFFER) && child->m_frameBuffer != this->m_frameBuffer) {
        CSimpleFrame* owner = this->m_frameBuffer;
        child->m_frameBuffer = owner;
        ForEachChild(child, [&](CSimpleFrame* grandchild) {
            grandchild->SetFrameBufferOwner(owner);
        });
    }
    this->MarkBatchDirty();
}

// ref: FUN_14061c210
void CSimpleFrame::RemoveChild(CSimpleFrame* child) {
    for (SIMPLEFRAMENODE* node = this->m_children.Head(); node != nullptr; node = this->m_children.Link(node)->Next()) {
        if (node->m_frame != child) {
            continue;
        }
        if (child->m_frameBuffer == this->m_frameBuffer && !(child->m_flags2 & FRAME2_FRAME_BUFFER) && child->m_frameBuffer != nullptr) {
            child->m_frameBuffer = nullptr;
            ForEachChild(child, [](CSimpleFrame* grandchild) {
                grandchild->SetFrameBufferOwner(nullptr);
            });
        }
        node->m_link.Unlink();
        MemoryWatchBlock(node, false);
        SMemFree(node, s_childNodeTag, -2, 0);
        this->MarkBatchDirty();
        return;
    }
}

// ref: FUN_1406158a0
CScriptObject* CSimpleFrame::FindChildByParentKey(const char* key) {
    if (key == nullptr || *key == '\0') {
        return nullptr;
    }
    for (CSimpleRegion* region = this->m_regions.Head(); region != nullptr; region = this->m_regions.Link(region)->Next()) {
        const char* own = region->GetParentKey();
        if (own != nullptr && strcmp(own, key) == 0) {
            return region;
        }
    }
    for (SIMPLEFRAMENODE* node = this->m_children.Head(); node != nullptr; node = this->m_children.Link(node)->Next()) {
        const char* own = node->m_frame->GetParentKey();
        if (own != nullptr && strcmp(own, key) == 0) {
            return node->m_frame;
        }
    }
    return nullptr;
}

// The children scripts can see.
// ref: FUN_140616350
int32_t CSimpleFrame::GetNumChildren() {
    int32_t count = 0;
    for (SIMPLEFRAMENODE* node = this->m_children.Head(); node != nullptr; node = this->m_children.Link(node)->Next()) {
        uint16_t flags = static_cast<FrameScript_Object*>(node->m_frame)->m_flags;
        if ((flags & FSOBJ_ACCESSIBLE) && !(flags & FSOBJ_HIDDEN)) {
            count++;
        }
    }
    return count;
}

// ref: FUN_1406163a0
int32_t CSimpleFrame::GetNumRegions() {
    int32_t count = 0;
    for (CSimpleRegion* region = this->m_regions.Head(); region != nullptr; region = this->m_regions.Link(region)->Next()) {
        uint16_t flags = static_cast<FrameScript_Object*>(region)->m_flags;
        if ((flags & FSOBJ_ACCESSIBLE) && !(flags & FSOBJ_HIDDEN)) {
            count++;
        }
    }
    return count;
}

// The children scripts may see, in list order.
// ref: FUN_140616110
SIMPLEFRAMELIST* CSimpleFrame::GetScriptChildren(SIMPLEFRAMELIST* children) {
    children->reserve(static_cast<uint64_t>(this->GetNumChildren()));
    for (SIMPLEFRAMENODE* node = this->m_children.Head(); node != nullptr; node = this->m_children.Link(node)->Next()) {
        uint16_t flags = static_cast<FrameScript_Object*>(node->m_frame)->m_flags;
        if ((flags & FSOBJ_ACCESSIBLE) && !(flags & FSOBJ_HIDDEN)) {
            children->push_back(node->m_frame);
        }
    }
    return children;
}

// The regions scripts may see, in list order.
// ref: FUN_1406164a0
SIMPLEREGIONLIST* CSimpleFrame::GetScriptRegions(SIMPLEREGIONLIST* regions) {
    regions->reserve(static_cast<uint64_t>(this->GetNumRegions()));
    for (CSimpleRegion* region = this->m_regions.Head(); region != nullptr; region = this->m_regions.Link(region)->Next()) {
        uint16_t flags = static_cast<FrameScript_Object*>(region)->m_flags;
        if ((flags & FSOBJ_ACCESSIBLE) && !(flags & FSOBJ_HIDDEN)) {
            regions->push_back(region);
        }
    }
    return regions;
}

// ref: FUN_140615380
void CSimpleFrame::DesaturateHierarchy(float desaturation, bool excludeRoot) {
    this->DesaturateRegions(desaturation, 0, excludeRoot ? 1 : 0);
}

// The textures of this frame (when `depth` has reached `firstDepth`) and of every frame below
// it take `desaturation`.
// ref: FUN_140615250
void CSimpleFrame::DesaturateRegions(float desaturation, uint32_t depth, uint32_t firstDepth) {
    if (firstDepth <= depth) {
        // Each CSimpleTexture among m_regions takes SetDesaturation (FUN_1406349b0), then the
        // regions' deferred deletes are flushed; the texture class is not ported yet.
        FROST_UNIMPLEMENTED();
        CSimpleRegion::FlushDeferredDeletes();
    }
    for (SIMPLEFRAMENODE* node = this->m_children.Head(); node != nullptr; node = this->m_children.Link(node)->Next()) {
        if (node->m_frame != nullptr) {
            node->m_frame->DesaturateRegions(desaturation, depth + 1, firstDepth);
        }
    }
}

// Every texture of the frame turns to `radians` about (x, y).
// ref: FUN_14061a120
void CSimpleFrame::RotateTextures(float radians, float x, float y) {
    (void)radians;
    (void)x;
    (void)y;
    // Each CSimpleTexture among m_regions takes SetRotation (FUN_140635340) with the centre;
    // the texture class is not ported yet.
    FROST_UNIMPLEMENTED();
    CSimpleRegion::FlushDeferredDeletes();
}

// ref: FUN_140616c70
bool CSimpleFrame::IsChildShown(CScriptRegion* child) {
    return (child->m_layoutState & LAYOUT_SHOWN) != 0;
}

// ---- showing and hiding ------------------------------------------------------------------

// ref: FUN_140616a20
void CSimpleFrame::Hide() {
    if (!this->CanQueryRect() && CSimpleTop::s_instance->m_actionBlockedHook != nullptr) {
        CSimpleTop::s_instance->m_actionBlockedHook(this);
    }
    this->m_layoutState &= ~LAYOUT_SHOWN;
    this->ApplyHide();
}

// ref: FUN_14061baf0
void CSimpleFrame::Show() {
    if (!this->CanQueryRect() && CSimpleTop::s_instance->m_actionBlockedHook != nullptr) {
        CSimpleTop::s_instance->m_actionBlockedHook(this);
    }
    this->m_layoutState |= LAYOUT_SHOWN;
    this->ApplyShow();
}

// ref: FUN_14061ba30
void CSimpleFrame::SetShown(bool shown) {
    if (shown) {
        this->Show();
        return;
    }
    this->Hide();
}

// The frame and everything shown under it become visible, OnShow queued for each, all under
// one deferred-script scope.
// ref: FUN_14061bb40
void CSimpleFrame::ApplyShow() {
    if (!(this->m_layoutState & LAYOUT_SHOWN) || (this->m_parent != nullptr && !(this->m_parent->m_layoutState & LAYOUT_VISIBLE))
        || (this->m_layoutState & LAYOUT_VISIBLE)) {
        return;
    }
    CSimpleTop* top = CSimpleTop::s_instance;
    this->m_layoutState |= LAYOUT_VISIBLE;
    this->UpdateUpdateRegistration();
    top->AddShownChange(this);
    if (HasOwnBatch(this)) {
        this->m_frameFlags |= FRAME_BATCH_DIRTY;
        top->AddPendingBatchUpdate(this);
    }
    this->MarkParentBatchDirty();
    top->m_deferredScripts.Begin();
    ForEachRegion(this, [](CSimpleRegion* region) {
        region->ApplyShow();
    });
    ForEachChild(this, [&](CSimpleFrame* child) {
        if (child != nullptr && child->m_parent == this) {
            child->ApplyShow();
        }
    });
    if ((this->m_frameFlags & FRAME_TOPLEVEL) && this->m_raiseOrder != top->m_raiseCounter) {
        top->m_raiseCounter++;
        this->SetRaiseOrder(top->m_raiseCounter);
    }
    this->QueueOnShow();
    if ((this->CLayoutFrame::m_flags & FLAG_DIRTY) && !(this->CLayoutFrame::m_flags & FLAG_RESIZE_QUEUED)) {
        this->AddToResizeList();
    }
    top->m_deferredScripts.End();
}

// ref: FUN_140616a70
void CSimpleFrame::ApplyHide() {
    if (!(this->m_layoutState & LAYOUT_VISIBLE)) {
        return;
    }
    CSimpleTop* top = CSimpleTop::s_instance;
    if (this->m_flags2 & FRAME2_DRAGGING) {
        this->CancelDrag();
    }
    this->m_layoutState &= ~LAYOUT_VISIBLE;
    this->UpdateUpdateRegistration();
    top->AddHiddenChange(this);
    top->RemoveFrameBatch(this);
    this->m_frameFlags &= ~FRAME_BATCH_DIRTY;
    this->MarkParentBatchDirty();
    top->m_deferredScripts.Begin();
    ForEachRegion(this, [](CSimpleRegion* region) {
        region->ApplyHide();
    });
    ForEachChild(this, [&](CSimpleFrame* child) {
        if (child != nullptr && child->m_parent == this) {
            child->ApplyHide();
        }
    });
    this->QueueOnHide();
    top->m_deferredScripts.End();
}

// ref: FUN_140619820
void CSimpleFrame::QueueOnShow() {
    this->CScriptRegion::QueueOnShow();
    this->MarkScrollRangeDirty();
}

// ref: FUN_140619420
void CSimpleFrame::QueueOnHide() {
    this->CScriptRegion::QueueOnHide();
    this->MarkScrollRangeDirty();
}

// ref: FUN_1406172c0
bool CSimpleFrame::IsLayoutReady() {
    for (CSimpleRegion* region = this->m_regions.Head(); region != nullptr; region = this->m_regions.Link(region)->Next()) {
        if (!region->IsLayoutReady()) {
            return false;
        }
    }
    for (SIMPLEFRAMENODE* node = this->m_children.Head(); node != nullptr; node = this->m_children.Link(node)->Next()) {
        if (!node->m_frame->IsLayoutReady()) {
            return false;
        }
    }
    return true;
}

// ref: FUN_1406198e0
void CSimpleFrame::OnLayoutReady() {
    this->MarkBatchDirty();
}

// ref: FUN_140614a70
void CSimpleFrame::UpdateUpdateRegistration() {
    bool wanted = (this->m_layoutState & LAYOUT_VISIBLE) && (this->WantsUpdate() || (this->m_frameFlags & FRAME_HAS_ONUPDATE));
    if (wanted != this->m_updateLink.IsLinked()) {
        CSimpleTop::s_instance->AddUpdateCheck(this);
    }
}

// ref: FUN_14061bab0
bool CSimpleFrame::WantsUpdateCheck() {
    return (this->m_layoutState & LAYOUT_VISIBLE) && (this->WantsUpdate() || (this->m_frameFlags & FRAME_HAS_ONUPDATE));
}

// ---- strata, level and raising -------------------------------------------------------------

// ref: FUN_14061afc0
void CSimpleFrame::SetFrameStrata(uint32_t strata) {
    if ((this->m_frameFlags & FRAME_FIXED_STRATA) || strata == ((this->m_flags2 >> 7) & 0xf) || strata == 0) {
        return;
    }
    uint16_t flags = this->FrameScript_Object::m_flags;
    if (strata == 9 && !(flags & FSOBJ_LOCKDOWN) && (flags & FSOBJ_ACCESSIBLE)) {
        return;
    }
    this->m_flags2 = (this->m_flags2 & ~FRAME2_STRATA) | ((strata << 7) & FRAME2_STRATA);
    ForEachChild(this, [&](CSimpleFrame* child) {
        child->SetFrameStrata(strata);
    });
    this->MarkParentBatchDirty();
}

// ref: FUN_14061b0d0
void CSimpleFrame::SetFrameStrataChecked(uint32_t strata) {
    if (this->CanQueryRect()) {
        this->SetFrameStrata(strata);
        return;
    }
    if (CSimpleTop::s_instance->m_actionBlockedHook != nullptr) {
        CSimpleTop::s_instance->m_actionBlockedHook(this);
    }
}

// ref: FUN_14061b120
void CSimpleFrame::ClearFrameStrata() {
    if (!(this->m_flags2 & FRAME2_STRATA)) {
        return;
    }
    this->m_flags2 &= ~FRAME2_STRATA;
    ForEachChild(this, [](CSimpleFrame* child) {
        child->ClearFrameStrata();
    });
    this->MarkParentBatchDirty();
}

// Children of the same strata keep their offset from the frame.
// ref: FUN_14061add0
void CSimpleFrame::SetFrameLevel(uint16_t level) {
    if (this->m_frameFlags & FRAME_FIXED_LEVEL) {
        return;
    }
    uint16_t value = level < 10001 ? level : 10000;
    if ((this->m_frameFlags & FRAME_USE_PARENT_LEVEL) && this->m_parent != nullptr) {
        value = this->m_parent->m_level;
    }
    uint16_t old = this->m_level;
    if (value == old) {
        return;
    }
    this->m_level = value;
    ForEachChild(this, [&](CSimpleFrame* child) {
        if (((child->m_flags2 ^ this->m_flags2) & FRAME2_STRATA) == 0) {
            child->SetFrameLevel(static_cast<uint16_t>(child->m_level - old + value));
        }
    });
    this->MarkParentBatchDirty();
}

// ref: FUN_14061af00
void CSimpleFrame::SetFrameLevelChecked(uint16_t level) {
    if (this->CanQueryRect()) {
        this->SetFrameLevel(level);
        return;
    }
    if (CSimpleTop::s_instance->m_actionBlockedHook != nullptr) {
        CSimpleTop::s_instance->m_actionBlockedHook(this);
    }
}

// ref: FUN_14061b8e0
void CSimpleFrame::SetRaiseOrder(int32_t order) {
    if (order == this->m_raiseOrder) {
        return;
    }
    this->m_raiseOrder = order;
    ForEachChild(this, [&](CSimpleFrame* child) {
        if (((child->m_flags2 ^ this->m_flags2) & FRAME2_STRATA) == 0) {
            child->SetRaiseOrder(this->m_raiseOrder);
        }
    });
    this->MarkParentBatchDirty();
}

// The toplevel frame at or above this one goes above every other.
// ref: FUN_140619f20
void CSimpleFrame::Raise() {
    CSimpleFrame* frame = this;
    while (!(frame->m_frameFlags & FRAME_TOPLEVEL)) {
        frame = frame->m_parent;
        if (frame == nullptr) {
            return;
        }
    }
    CSimpleTop* top = CSimpleTop::s_instance;
    if (frame->m_raiseOrder == top->m_raiseCounter) {
        return;
    }
    top->m_raiseCounter++;
    frame->SetRaiseOrder(top->m_raiseCounter);
}

// ref: FUN_140618700
void CSimpleFrame::Lower() {
    CSimpleFrame* frame = this;
    while (!(frame->m_frameFlags & FRAME_TOPLEVEL)) {
        frame = frame->m_parent;
        if (frame == nullptr) {
            return;
        }
    }
    frame->SetRaiseOrder(0);
}

void CSimpleFrame::SetToplevel(bool toplevel) {
    if (((this->m_frameFlags & FRAME_TOPLEVEL) != 0) == toplevel) {
        return;
    }
    this->m_frameFlags = toplevel ? (this->m_frameFlags | FRAME_TOPLEVEL) : (this->m_frameFlags & ~FRAME_TOPLEVEL);
    this->UpdateOwnBatch();
}

// The draw order of a layer: strata, raise order, level, layer and sublevel, high to low.
// ref: FUN_140616300
uint64_t CSimpleFrame::GetDrawOrderKey(int32_t layer, int32_t subLevel) {
    // FUN_14060de90
    uint32_t strata = (this->m_flags2 >> 7) & 0xf;
    uint32_t layerKey = layer == 6 ? 0 : static_cast<uint32_t>(layer + 1);
    uint64_t key = static_cast<uint64_t>(static_cast<uint32_t>(this->m_raiseOrder)) | (static_cast<uint64_t>(strata) << 32);
    key = (key << 16) | this->m_level;
    key = (key << 6) | layerKey;
    return (key << 5) | static_cast<uint64_t>(static_cast<int64_t>(subLevel + 9));
}

// ref: FUN_1406166d0
uint64_t CSimpleFrame::GetDrawOrder() {
    return this->GetDrawOrderKey(6, -9);
}

// ---- batching ------------------------------------------------------------------------------

// The nearest frame drawing into its own batch (or this one) rebuilds it; with none, the
// root's strata lists are rebuilt.
// ref: FUN_1406187a0
void CSimpleFrame::MarkBatchDirty() {
    CSimpleFrame* frame = this;
    while (!HasOwnBatch(frame)) {
        frame = frame->m_parent;
        if (frame == nullptr) {
            CSimpleTop::s_instance->MarkStrataDirty();
            return;
        }
    }
    if (!(frame->m_frameFlags & FRAME_BATCH_DIRTY)) {
        frame->m_frameFlags |= FRAME_BATCH_DIRTY;
        if (frame->m_layoutState & LAYOUT_VISIBLE) {
            CSimpleTop::s_instance->AddPendingBatchUpdate(frame);
        }
    }
}

// ref: FUN_140618750
void CSimpleFrame::MarkParentBatchDirty() {
    for (CSimpleFrame* frame = this->m_parent; frame != nullptr; frame = frame->m_parent) {
        if (HasOwnBatch(frame)) {
            frame->MarkBatchDirty();
            return;
        }
    }
    CSimpleTop::s_instance->MarkStrataDirty();
}

// Inside a scroll frame, the scroll frame above the scroll child recomputes its range.
// ref: FUN_1406188b0
void CSimpleFrame::MarkScrollRangeDirty() {
    if (!this->IsInScrollFrame() || !(this->m_layoutState & LAYOUT_VISIBLE)) {
        return;
    }
    CSimpleFrame* frame = this;
    uint32_t flags;
    do {
        flags = frame->m_frameFlags;
        frame = frame->m_parent;
        if (flags & FRAME_SCROLL_CHILD) {
            break;
        }
        if (frame == nullptr) {
            return;
        }
    } while (true);
    if (frame != nullptr) {
        // CSimpleScrollFrame's range-dirty flag (+0x300), past the CSimpleFrame part
        reinterpret_cast<uint8_t*>(frame)[0x300] = 1;
    }
}

// Whether the frame needs its own batch changed: the flag follows, the children learn
// whether they are batched, and the batches above are rebuilt.
// ref: FUN_14061c6f0
void CSimpleFrame::UpdateOwnBatch() {
    bool own = HasOwnBatch(this);
    bool needs = this->NeedsOwnBatch();
    if (own == needs) {
        return;
    }
    this->m_frameFlags = needs ? (this->m_frameFlags | FRAME_OWN_BATCH) : (this->m_frameFlags & ~FRAME_OWN_BATCH);
    this->UpdateBatched();
    if (!needs) {
        CSimpleTop::s_instance->RemoveFrameBatch(this);
        this->m_frameFlags &= ~FRAME_BATCH_DIRTY;
    } else {
        this->MarkBatchDirty();
    }
    for (CSimpleFrame* frame = this->m_parent; frame != nullptr; frame = frame->m_parent) {
        uint32_t flags = frame->m_frameFlags;
        if (!(flags & FRAME_OWN_BATCH) && !(frame->m_flags2 & FRAME2_FRAME_BUFFER)) {
            continue;
        }
        if (flags & FRAME_BATCH_DIRTY) {
            return;
        }
        frame->m_frameFlags = flags | FRAME_BATCH_DIRTY;
        if (frame->m_layoutState & LAYOUT_VISIBLE) {
            CSimpleTop::s_instance->AddPendingBatchUpdate(frame);
        }
        return;
    }
    CSimpleTop::s_instance->MarkStrataDirty();
}

// ref: FUN_14061c4b0
void CSimpleFrame::UpdateBatched() {
    bool batched = HasOwnBatch(this) || this->IsBatched();
    ForEachChild(this, [&](CSimpleFrame* child) {
        child->SetBatched(batched);
    });
}

// ref: FUN_14061a8c0
void CSimpleFrame::SetBatched(bool batched) {
    if (this->IsBatched() == batched) {
        return;
    }
    this->m_frameFlags = batched ? (this->m_frameFlags | FRAME_BATCHED) : (this->m_frameFlags & ~FRAME_BATCHED);
    this->UpdateBatched();
}

// ref: FUN_14061ba90
bool CSimpleFrame::NeedsOwnBatch() {
    uint32_t flags = this->m_frameFlags;
    return (flags & FRAME_FLATTEN_RENDER_LAYERS) || (flags & FRAME_SCROLL_CHILD) || (flags & FRAME_TOPLEVEL)
        || (flags & FRAME_CLIP_CHILDREN);
}

// ref: FUN_140614630
bool CSimpleFrame::IsBatched() {
    return (this->m_frameFlags & FRAME_BATCHED) != 0;
}

// ref: FUN_14061b7f0
void CSimpleFrame::SetFrameBufferOwner(CSimpleFrame* owner) {
    if ((this->m_flags2 & FRAME2_FRAME_BUFFER) || this->m_frameBuffer == owner) {
        return;
    }
    this->m_frameBuffer = owner;
    ForEachChild(this, [&](CSimpleFrame* child) {
        child->SetFrameBufferOwner(owner);
    });
}

// ref: FUN_14061b3d0
void CSimpleFrame::SetIsFrameBuffer(bool frameBuffer) {
    if (((this->m_flags2 >> 13) & 1) == static_cast<uint32_t>(frameBuffer)) {
        return;
    }
    this->m_flags2 &= ~FRAME2_FRAME_BUFFER;
    this->SetFrameBufferOwner(frameBuffer ? this : nullptr);
    this->m_flags2 = (this->m_flags2 & ~FRAME2_FRAME_BUFFER) | (static_cast<uint32_t>(frameBuffer) << 13);
    this->UpdateEffectiveAlpha();
}

// ref: FUN_14061ad30
void CSimpleFrame::SetFlattenRenderLayers(bool flatten) {
    if (((this->m_frameFlags >> 15) & 1) == static_cast<uint32_t>(flatten)) {
        return;
    }
    this->m_frameFlags = flatten ? (this->m_frameFlags | FRAME_FLATTEN_RENDER_LAYERS) : (this->m_frameFlags & ~FRAME_FLATTEN_RENDER_LAYERS);
    this->UpdateOwnBatch();
}

// The batch clips to the scroll frame's rect (for a scroll child) and to the frame's own
// (when it clips its children).
// ref: FUN_14061c120
void CSimpleFrame::UpdateClipRect() {
    CRenderBatch* batch = CSimpleTop::s_instance->FindFrameBatch(this);
    if (batch == nullptr) {
        return;
    }
    bool scrollChild = (this->m_frameFlags & FRAME_SCROLL_CHILD) != 0;
    CRect clip = {};
    if (scrollChild) {
        this->m_parent->GetHitRect(&clip);
    }
    if (this->m_frameFlags & FRAME_CLIP_CHILDREN) {
        CRect own;
        this->GetHitRect(&own);
        if (!scrollChild) {
            clip = own;
        } else {
            clip.minY = own.minY <= clip.minY ? clip.minY : own.minY;
            clip.minX = own.minX <= clip.minX ? clip.minX : own.minX;
            clip.maxY = clip.maxY <= own.maxY ? clip.maxY : own.maxY;
            clip.maxX = clip.maxX <= own.maxX ? clip.maxX : own.maxX;
        }
    } else if (!scrollChild) {
        return;
    }
    batch->m_clipRect = clip;
    if (!batch->m_clipped) {
        batch->m_clipped = 1;
    }
}

// ref: FUN_1406189e0
void CSimpleFrame::RefreshBatch() {
    uint32_t flags = this->m_frameFlags;
    if (HasOwnBatch(this) && !(flags & FRAME_BATCH_DIRTY)) {
        this->m_frameFlags = flags | FRAME_BATCH_DIRTY;
        if (this->m_layoutState & LAYOUT_VISIBLE) {
            CSimpleTop::s_instance->AddPendingBatchUpdate(this);
        }
    }
    this->Unknown2B8();
    ForEachRegion(this, [](CSimpleRegion* region) {
        region->Unknown2C8();
    });
}

// ref: FUN_140618aa0
void CSimpleFrame::RefreshRegions() {
    this->Unknown2C0();
    ForEachRegion(this, [](CSimpleRegion* region) {
        region->Unknown2D0();
    });
}

// ref: FUN_140618d20
void CSimpleFrame::AddRegionsToBatch(CRenderBatch* batch) {
    for (CSimpleRegion* region = this->m_drawRegions.Head(); region != nullptr;) {
        CSimpleRegion* next = this->m_drawRegions.Link(region)->Next();
        if (this->m_flags2 & (1u << (region->m_drawLayer & 0x1f)) & FRAME2_LAYERS) {
            region->AddToBatch(batch);
        }
        region = next;
    }
}

// Visible children: those without their own batch add their regions to this one, the others
// add their own batch (rebuilt first when dirty).
// ref: FUN_140613b80
void CSimpleFrame::RenderChildren(CRenderBatch* batch) {
    ForEachChild(this, [&](CSimpleFrame* child) {
        if (!(child->m_layoutState & LAYOUT_VISIBLE)) {
            return;
        }
        if (!HasOwnBatch(child)) {
            child->AddRegionsToBatch(batch);
            child->RenderChildren(batch);
            return;
        }
        CRenderBatch* childBatch = CSimpleTop::s_instance->GetFrameBatch(child);
        if (child->m_frameFlags & FRAME_BATCH_DIRTY) {
            child->RenderToBatch(childBatch);
        }
        batch->AddChildBatch(child, child->GetDrawOrder(), childBatch);
    });
}

// ref: FUN_140614b70
void CSimpleFrame::RenderToBatch(CRenderBatch* batch) {
    this->m_frameFlags &= ~FRAME_BATCH_DIRTY;
    batch->Clear();
    this->AddRegionsToBatch(batch);
    this->RenderChildren(batch);
    this->UpdateClipRect();
}

// ref: FUN_140614460
void CSimpleFrame::AddToParentBatch(CRenderBatch* batch) {
    if (!(this->m_layoutState & LAYOUT_VISIBLE) || this->IsBatched()) {
        return;
    }
    if (!HasOwnBatch(this)) {
        this->AddRegionsToBatch(batch);
        return;
    }
    CRenderBatch* own = CSimpleTop::s_instance->FindFrameBatch(this);
    if (own != nullptr) {
        batch->AddChildBatch(this, this->GetDrawOrder(), own);
    }
}

// ref: FUN_140614bc0
void CSimpleFrame::RebuildBatchIfDirty() {
    if ((this->m_layoutState & LAYOUT_VISIBLE) && HasOwnBatch(this) && (this->m_frameFlags & FRAME_BATCH_DIRTY)) {
        this->RenderToBatch(CSimpleTop::s_instance->GetFrameBatch(this));
    }
}

// ---- alpha, scale, depth ---------------------------------------------------------------------

// ref: FUN_14061a8a0
void CSimpleFrame::SetAlpha(uint8_t alpha) {
    if (alpha != this->m_alpha) {
        this->m_alpha = alpha;
        this->UpdateEffectiveAlpha();
    }
}

// ref: FUN_140615cb0
uint8_t CSimpleFrame::GetAlpha() {
    return this->m_alpha;
}

// ref: FUN_140616280
uint8_t CSimpleFrame::GetEffectiveAlpha() {
    return static_cast<uint8_t>(static_cast<uint32_t>(this->m_parentAlpha) * this->m_alpha / 0xff);
}

// The regions take the new alpha, then the children (unless they ignore it; a frame-buffer
// frame hands down full alpha).
// ref: FUN_14061c3a0
void CSimpleFrame::UpdateEffectiveAlpha() {
    ForEachRegion(this, [](CSimpleRegion* region) {
        region->UpdateEffectiveAlpha();
    });
    uint8_t alpha = (this->m_flags2 & FRAME2_FRAME_BUFFER) ? 0xff : this->GetEffectiveAlpha();
    ForEachChild(this, [&](CSimpleFrame* child) {
        if (!(child->m_frameFlags & FRAME_IGNORE_PARENT_ALPHA) && alpha != child->m_parentAlpha) {
            child->m_parentAlpha = alpha;
            child->UpdateEffectiveAlpha();
        }
    });
}

// ref: FUN_14061b300
void CSimpleFrame::SetIgnoreParentAlpha(bool ignore) {
    if (ignore) {
        if (!(this->m_frameFlags & FRAME_IGNORE_PARENT_ALPHA) && this->m_parentAlpha != 0xff) {
            this->m_parentAlpha = 0xff;
            this->UpdateEffectiveAlpha();
        }
        this->m_frameFlags |= FRAME_IGNORE_PARENT_ALPHA;
        return;
    }
    this->m_frameFlags &= ~FRAME_IGNORE_PARENT_ALPHA;
    if (this->m_parent != nullptr) {
        uint8_t alpha = this->m_parent->GetEffectiveAlpha();
        if (!(this->m_frameFlags & FRAME_IGNORE_PARENT_ALPHA) && alpha != this->m_parentAlpha) {
            this->m_parentAlpha = alpha;
            this->UpdateEffectiveAlpha();
        }
    }
}

// ref: FUN_14061b3a0
void CSimpleFrame::SetIgnoreParentScale(bool ignore) {
    this->m_frameFlags = ignore ? (this->m_frameFlags | FRAME_IGNORE_PARENT_SCALE) : (this->m_frameFlags & ~FRAME_IGNORE_PARENT_SCALE);
    this->UpdateScale(true);
}

// ref: FUN_14061af60
bool CSimpleFrame::SetScale(float scale, bool force) {
    if (scale <= 0.0f) {
        FrameScript_ActionError("Scale must be > 0");
        return false;
    }
    if (force || SCALE_EPSILON <= fabsf(scale - this->m_scale)) {
        this->m_scale = scale;
        return this->UpdateScale(force);
    }
    return false;
}

// The layout scale is the frame's scale times the parent's (unless ignored); the regions
// (those that follow it) and children follow.
// ref: FUN_14061c840
bool CSimpleFrame::UpdateScale(bool force) {
    float scale = this->m_scale;
    if (!(this->m_frameFlags & FRAME_IGNORE_PARENT_SCALE) && this->m_parent != nullptr) {
        scale = scale * this->m_parent->m_layoutScale;
    }
    if ((!force && fabsf(scale - this->m_layoutScale) < SCALE_EPSILON) || scale == 0.0f) {
        return false;
    }
    this->CLayoutFrame::SetLayoutScale(scale, force);
    ForEachRegion(this, [&](CSimpleRegion* region) {
        if (!(region->m_regionFlags & CSimpleRegion::REGIONFLAG_IGNORE_PARENT_SCALE)) {
            region->SetLayoutScale(scale, force);
        }
    });
    ForEachChild(this, [](CSimpleFrame* child) {
        child->UpdateScale(false);
    });
    return true;
}

// ref: FUN_14061ad70
bool CSimpleFrame::SetDepth(float depth, bool force) {
    if (!force && fabsf(depth - this->m_depth) < SCALE_EPSILON) {
        return false;
    }
    this->m_depth = depth;
    return this->UpdateDepth(force);
}

// ref: FUN_14061c540
bool CSimpleFrame::UpdateDepth(bool force) {
    float depth = this->m_effectiveDepth;
    if (!force && fabsf(depth - this->m_layoutDepth) < SCALE_EPSILON) {
        return false;
    }
    this->CLayoutFrame::SetLayoutDepth(depth, force);
    ForEachRegion(this, [&](CSimpleRegion* region) {
        region->CLayoutFrame::SetLayoutDepth(depth, force);
    });
    ForEachChild(this, [&](CSimpleFrame* child) {
        if (force || SCALE_EPSILON <= fabsf(depth - child->m_effectiveDepth)) {
            child->m_effectiveDepth = depth;
            child->UpdateDepth(force);
        }
    });
    return true;
}

// ---- draw layers, input and flags ------------------------------------------------------------

// ref: FUN_140615800
void CSimpleFrame::EnableDrawLayer(uint8_t layer) {
    uint32_t bit = 1u << (layer & 0x1f);
    if (!(this->m_flags2 & bit & FRAME2_LAYERS)) {
        this->m_flags2 |= bit & FRAME2_LAYERS;
        this->MarkBatchDirty();
    }
}

// ref: FUN_140615560
void CSimpleFrame::DisableDrawLayer(uint8_t layer) {
    uint32_t bit = 1u << (layer & 0x1f);
    if (this->m_flags2 & bit & FRAME2_LAYERS) {
        this->m_flags2 &= ~(bit & FRAME2_LAYERS);
        this->MarkBatchDirty();
    }
}

// ref: FUN_14061acd0
void CSimpleFrame::SetDrawLayerEnabled(uint8_t layer, bool enabled) {
    if (enabled) {
        this->EnableDrawLayer(layer);
        return;
    }
    this->DisableDrawLayer(layer);
}

// ref: FUN_140615860
void CSimpleFrame::EnableKeyboard(bool enable) {
    this->SetInputEvent(1, enable);
    this->SetInputEvent(0, enable);
}

// ref: FUN_140615840
void CSimpleFrame::EnableGamePadButton(bool enable) {
    this->SetInputEvent(5, enable);
}

// ref: FUN_140615850
void CSimpleFrame::EnableGamePadStick(bool enable) {
    this->SetInputEvent(6, enable);
}

// ref: FUN_14061adb0
void CSimpleFrame::SetFrameFlag(uint32_t flag, bool set) {
    if (set) {
        this->m_frameFlags |= flag;
        return;
    }
    this->m_frameFlags &= ~flag;
}

// ref: FUN_14061b440
void CSimpleFrame::SetMovable(bool movable) {
    if (movable && !ScriptRegion_CheckAction(ACTION_SET_MOVABLE, this->GetScriptRegion(), "SetMovable")) {
        return;
    }
    this->SetFrameFlag(FRAME_MOVABLE, movable);
}

// ref: FUN_14061b9c0
void CSimpleFrame::SetResizable(bool resizable) {
    if (resizable && !ScriptRegion_CheckAction(ACTION_SET_RESIZABLE, this->GetScriptRegion(), "SetResizable")) {
        return;
    }
    this->SetFrameFlag(FRAME_RESIZABLE, resizable);
}

// ref: FUN_14061ba50
void CSimpleFrame::SetUserPlaced(bool placed) {
    if (!(this->m_frameFlags & FRAME_MOVABLE) && !(this->m_frameFlags & FRAME_RESIZABLE)) {
        FrameScript_ActionError("Frame is not movable or resizable");
        return;
    }
    this->SetFrameFlag(FRAME_USER_PLACED, placed);
}

// ref: FUN_14061b2c0
void CSimpleFrame::SetHyperlinksEnabled(bool enabled) {
    if (((this->m_frameFlags >> 17) & 1) == static_cast<uint32_t>(enabled)) {
        return;
    }
    this->SetFrameFlag(FRAME_HYPERLINKS_ENABLED, enabled);
    this->UpdateHyperlinkRegistration();
}

// ref: FUN_14061c690
void CSimpleFrame::UpdateHyperlinkRegistration() {
    this->UpdateClickRegistration();
    this->UpdateMotionRegistration();
    ForEachChild(this, [](CSimpleFrame* child) {
        child->UpdateHyperlinkRegistration();
    });
}

// A scroll frame's descendants are marked down to its scroll child's own children.
// ref: FUN_14061a9d0
void CSimpleFrame::SetInScrollFrame(bool inScrollFrame, int32_t mode) {
    this->SetFrameFlag(FRAME_IN_SCROLL_FRAME, inScrollFrame);
    if (mode != 0) {
        this->SetFrameFlag(FRAME_SCROLL_CHILD, mode == 2);
    }
    ForEachChild(this, [&](CSimpleFrame* child) {
        if (!(child->m_frameFlags & FRAME_SCROLL_CHILD)) {
            child->SetInScrollFrame(inScrollFrame, 0);
        }
    });
    this->UpdateOwnBatch();
}

// ref: FUN_14061ac50
void CSimpleFrame::SetInherited200(bool set) {
    this->SetFrameFlag(FRAME_INHERITED_200, set);
    ForEachChild(this, [&](CSimpleFrame* child) {
        child->SetInherited200(set);
    });
}

// ref: FUN_14061b290
void CSimpleFrame::SetHitRectInsets(float left, float right, float top, float bottom) {
    this->m_hitRectInsets.minY = bottom;
    this->m_hitRectInsets.minX = left;
    this->m_hitRectInsets.maxX = right;
    this->m_hitRectInsets.maxY = top;
}

// ref: FUN_14061aa90
void CSimpleFrame::SetClampRectInsets(float left, float right, float top, float bottom) {
    this->m_clampRectInsets.minY = bottom;
    this->m_clampRectInsets.minX = left;
    this->m_clampRectInsets.maxX = right;
    this->m_clampRectInsets.maxY = top;
}

// ref: FUN_1406162f0
const float* CSimpleFrame::GetHitRectInsets() {
    return &this->m_hitRectInsets.minY;
}

// ref: FUN_140616250
void CSimpleFrame::GetClampRectInsets(float* left, float* right, float* top, float* bottom) {
    *left = this->m_clampRectInsets.minX;
    *right = this->m_clampRectInsets.maxX;
    *top = this->m_clampRectInsets.maxY;
    *bottom = this->m_clampRectInsets.minY;
}

// ref: FUN_140617180
bool CSimpleFrame::IsInScrollFrame() {
    return (this->m_frameFlags & FRAME_IN_SCROLL_FRAME) != 0;
}

// ref: FUN_1406157d0
bool CSimpleFrame::GetHyperlinksEnabled() {
    return (this->m_frameFlags & FRAME_HYPERLINKS_ENABLED) || this->GetHyperlinksPropagated();
}

// ref: FUN_140614a30
bool CSimpleFrame::GetHyperlinksPropagated() {
    return (this->m_frameFlags & FRAME_PROPAGATE_HYPERLINKS) && this->m_parent != nullptr && this->m_parent->GetHyperlinksEnabled();
}

namespace {

// A hyperlink's rect (bottom, left, top, right in UI units of `region`) as the left, bottom,
// width and height in pixels at 768 high that the handlers get.
void HyperlinkRectToPixels(const CRect* rect, CScriptRegion* region, float* values) {
    const COORDINATESPACE& space = g_coordinateSpace;
    float pixels = space.m_aspectCompensation * 1024.0f;
    float across = 1.0f / space.m_diagonalWidth;
    float scale = 1.0f / region->m_layoutScale;
    values[0] = rect->minX * scale * across * pixels;
    values[1] = rect->maxY * scale * across * pixels;
    values[2] = (rect->maxX - rect->minX) * scale * across * pixels;
    values[3] = (rect->maxY - rect->minY) * scale * across * pixels;
}

}

// The OnHyperlinkClick handler, or the parent's when this frame passes its links up.
// ref: FUN_14061a390
void CSimpleFrame::RunOnHyperlinkClick(const FRAMESCRIPT_LSTRING* link, const FRAMESCRIPT_LSTRING* text,
    const char* button, CSimpleRegion* region, const CRect* rect) {
    if ((this->m_frameFlags & FRAME_PROPAGATE_HYPERLINKS) == 0) {
        float values[4];
        HyperlinkRectToPixels(rect, region, values);
        FRAMESCRIPT_REF ref = { region->GetLuaRef() };
        this->RunScript(0x17, nullptr, *link, *text, button, ref, values[0], values[1], values[2], values[3]);
    } else if (this->m_parent != nullptr && this->m_parent->GetHyperlinksEnabled()) {
        this->m_parent->RunOnHyperlinkClick(link, text, button, region, rect);
    }
}

// ref: FUN_14061a5c0
void CSimpleFrame::RunOnHyperlinkEnter(const FRAMESCRIPT_LSTRING* link, const FRAMESCRIPT_LSTRING* text,
    CSimpleRegion* region, const CRect* rect) {
    if ((this->m_frameFlags & FRAME_PROPAGATE_HYPERLINKS) == 0) {
        float values[4];
        HyperlinkRectToPixels(rect, region, values);
        FRAMESCRIPT_REF ref = { region->GetLuaRef() };
        this->RunScript(0x18, nullptr, *link, *text, ref, values[0], values[1], values[2], values[3]);
    } else if (this->m_parent != nullptr && this->m_parent->GetHyperlinksEnabled()) {
        this->m_parent->RunOnHyperlinkEnter(link, text, region, rect);
    }
}

// ref: FUN_14061a7d0
void CSimpleFrame::RunOnHyperlinkLeave() {
    CSimpleFrame* frame = this;
    while ((frame->m_frameFlags & FRAME_PROPAGATE_HYPERLINKS) != 0) {
        if (frame->m_parent == nullptr || !frame->m_parent->GetHyperlinksEnabled()) {
            return;
        }
        frame = frame->m_parent;
    }
    frame->RunScript(0x19, nullptr);
}

// ref: FUN_14061b1f0
void CSimpleFrame::LockHighlight(bool lock) {
    if (((this->m_frameFlags >> 24) & 1) == static_cast<uint32_t>(lock)) {
        return;
    }
    this->SetFrameFlag(FRAME_HIGHLIGHT_LOCKED, lock);
    if (!lock) {
        // the highlight stays while the cursor is over the frame
        for (CScriptRegion* region : CSimpleTop::s_instance->m_mouseMotion) {
            if (region == this) {
                return;
            }
        }
        this->DisableDrawLayer(DRAWLAYER_HIGHLIGHT);
        return;
    }
    this->EnableDrawLayer(DRAWLAYER_HIGHLIGHT);
}

// ---- the region slots for clipping, scrolling and protection -----------------------------------

// Clipping ancestors (scroll frames, and frames clipping their children) cut the rect down.
// ref: FUN_140614510
CRect* CSimpleFrame::TransformRect(CRect* out, const CRect* rect) {
    CRect clip = *rect;
    CSimpleFrame* frame = this;
    while (frame != nullptr && (HasOwnBatch(frame) || frame->IsBatched())) {
        if (frame->m_frameFlags & FRAME_SCROLL_CHILD) {
            CRect parent;
            frame->m_parent->GetHitRect(&parent);
            clip.minY = parent.minY <= clip.minY ? clip.minY : parent.minY;
            clip.minX = parent.minX <= clip.minX ? clip.minX : parent.minX;
            clip.maxY = clip.maxY <= parent.maxY ? clip.maxY : parent.maxY;
            clip.maxX = clip.maxX <= parent.maxX ? clip.maxX : parent.maxX;
        }
        if (frame->m_frameFlags & FRAME_CLIP_CHILDREN) {
            CRect own;
            frame->GetHitRect(&own);
            clip.minY = own.minY <= clip.minY ? clip.minY : own.minY;
            clip.minX = own.minX <= clip.minX ? clip.minX : own.minX;
            clip.maxY = clip.maxY <= own.maxY ? clip.maxY : own.maxY;
            clip.maxX = clip.maxX <= own.maxX ? clip.maxX : own.maxX;
        }
        frame = frame->m_parent;
    }
    *out = clip;
    return out;
}

// ref: FUN_140619910
void CSimpleFrame::OnAnchorRestrictedChanged(bool restricted) {
    if (restricted) {
        if (CSimpleTop::s_instance->m_dragFrame == this) {
            CSimpleTop::s_instance->StopDrag();
        }
        this->m_frameFlags &= ~(FRAME_MOVABLE | FRAME_RESIZABLE);
    }
    this->CLayoutFrame::OnAnchorRestrictedChanged(restricted);
}

// The insets scale with the frame, and the regions with them.
// ref: FUN_140618b30
void CSimpleFrame::ScaleBy(float* scale) {
    this->CLayoutFrame::ScaleBy(scale);
    this->m_hitRectInsets.minY *= *scale;
    this->m_hitRectInsets.minX *= *scale;
    this->m_hitRectInsets.maxY *= *scale;
    this->m_hitRectInsets.maxX *= *scale;
    float copy = *scale;
    ForEachRegion(this, [&](CSimpleRegion* region) {
        region->ScaleBy(&copy);
    });
}

// ref: FUN_140619e60
void CSimpleFrame::OnProtectFlagsChanged(uint16_t flag) {
    if (this->m_parent != nullptr) {
        this->m_parent->CLayoutFrame::SetProtectFlag(flag);
    }
}

// Whether a child is protected (one still settling reports through `propagated`).
// ref: FUN_140614930
bool CSimpleFrame::IsProtectedSelf(bool* propagated) {
    for (SIMPLEFRAMENODE* node = this->m_children.Head(); node != nullptr; node = this->m_children.Link(node)->Next()) {
        CSimpleFrame* child = node->m_frame;
        if (child == nullptr) {
            continue;
        }
        if (child->CLayoutFrame::m_flags & FLAG_PROTECT_BUSY) {
            *propagated = true;
        } else if (child->CLayoutFrame::IsProtected()) {
            return true;
        }
    }
    return false;
}

// ref: FUN_140618910
void CSimpleFrame::OnLayoutVersionChanged() {
    ForEachRegion(this, [](CSimpleRegion* region) {
        region->OnLayoutVersionChanged();
    });
    ForEachChild(this, [](CSimpleFrame* child) {
        child->OnLayoutVersionChanged();
    });
}

// A resize re-clips the frame's batch, runs OnSizeChanged when the size really changed and
// lets the UI root refresh the drag and the mouse focus.
// ref: FUN_140618db0
void CSimpleFrame::OnFrameSizeChanged(const CRect* rect, const CRect* previous) {
    this->CLayoutFrame::OnFrameSizeChanged(rect, previous);
    this->MarkScrollRangeDirty();
    if (HasOwnBatch(this)) {
        this->UpdateClipRect();
    }
    float width = rect->maxX - rect->minX;
    float height = rect->maxY - rect->minY;
    if (SIZE_EPSILON <= fabsf((previous->maxX - previous->minX) - width)
        || SIZE_EPSILON <= fabsf((previous->maxY - previous->minY) - height)) {
        float inverse = 1.0f / this->m_layoutScale;
        this->OnSizeChanged(inverse * width, height * inverse);
    }
    CSimpleTop::s_instance->OnFrameSizeChanged(this);
}

// OnSizeChanged gets the size in UI units.
// ref: FUN_140618ea0
void CSimpleFrame::OnSizeChanged(float width, float height) {
    if (this->m_flags2 & FRAME2_NO_SIZE_SCRIPT) {
        return;
    }
    const COORDINATESPACE& space = g_coordinateSpace;
    float unit = space.m_aspectCompensation * 1024.0f * (1.0f / space.m_diagonalWidth);
    // FUN_140612970
    this->RunScript(9, nullptr, unit * width, unit * height);
}

// ---- input ---------------------------------------------------------------------------------

// ref: FUN_14061c9c0
bool CSimpleFrame::HasCharHandler() {
    return this->HasScriptAny(0x16);
}

// ref: FUN_14061ca20
bool CSimpleFrame::HasKeyHandler(const void* event) {
    (void)event;
    return this->HasScriptAny(0x1b) || this->HasScriptAny(0x1c);
}

// ref: FUN_14061c9d0
bool CSimpleFrame::HasPadButtonHandler(void* event) {
    if (static_cast<uint8_t*>(event)[8] > 0x1f) {
        return false;
    }
    return this->HasScriptAny(0x1d) || this->HasScriptAny(0x1e);
}

// ref: FUN_14061ca10
bool CSimpleFrame::HasPadStickHandler(void* event) {
    if (static_cast<uint8_t*>(event)[9] > 7) {
        return false;
    }
    return this->HasScriptAny(0x1f);
}

// ref: FUN_14061ca50
bool CSimpleFrame::HasWheelHandler() {
    return this->HasScriptAny(0x12);
}

// OnChar gets the character as UTF-8; a frame propagating keyboard input passes it on unless
// it is protected.
// ref: FUN_140618fd0
bool CSimpleFrame::OnChar(int32_t key) {
    if (!this->HasScriptAny(0x16)) {
        return false;
    }
    char text[16] = {};
    SUniSPutUTF8(static_cast<uint32_t>(key), reinterpret_cast<uint8_t*>(text));
    this->RunScript(0x16, nullptr, static_cast<const char*>(text));
    if ((this->m_frameFlags & FRAME_PROPAGATE_KEYBOARD) && !this->CLayoutFrame::IsProtected()) {
        return false;
    }
    return true;
}

// ref: FUN_140619440
bool CSimpleFrame::OnKeyDown(void* event) {
    if (!this->HasScriptAny(0x1b) && !this->HasScriptAny(0x1c)) {
        return false;
    }
    const char* key = KeyToString(*static_cast<int32_t*>(event));
    CSimpleTop::s_instance->m_inMouseScript = 1;
    this->RunScript(0x1b, nullptr, key);
    CSimpleTop::s_instance->m_inMouseScript = 0;
    if ((this->m_frameFlags & FRAME_PROPAGATE_KEYBOARD) && !this->CLayoutFrame::IsProtected()) {
        return false;
    }
    return true;
}

// ref: FUN_1406194d0
bool CSimpleFrame::OnKeyUp(void* event) {
    if (!this->HasScriptAny(0x1c)) {
        return false;
    }
    const char* key = KeyToString(*static_cast<int32_t*>(event));
    CSimpleTop::s_instance->m_inMouseScript = 1;
    this->RunScript(0x1c, nullptr, key);
    CSimpleTop::s_instance->m_inMouseScript = 0;
    if ((this->m_frameFlags & FRAME_PROPAGATE_KEYBOARD) && !this->CLayoutFrame::IsProtected()) {
        return false;
    }
    return true;
}

// ref: FUN_140619200
bool CSimpleFrame::OnPadButtonDown(void* event) {
    if (!this->HasScriptAny(0x1d) && !this->HasScriptAny(0x1e)) {
        return false;
    }
    const char* button = GamePadButtonToString(static_cast<uint8_t*>(event)[8]);
    CSimpleTop::s_instance->m_inMouseScript = 1;
    this->RunScript(0x1d, nullptr, button);
    CSimpleTop::s_instance->m_inMouseScript = 0;
    if ((this->m_frameFlags & FRAME_PROPAGATE_KEYBOARD) && !this->CLayoutFrame::IsProtected()) {
        return false;
    }
    return true;
}

// ref: FUN_140619290
bool CSimpleFrame::OnPadButtonUp(void* event) {
    if (!this->HasScriptAny(0x1e)) {
        return false;
    }
    const char* button = GamePadButtonToString(static_cast<uint8_t*>(event)[8]);
    CSimpleTop::s_instance->m_inMouseScript = 1;
    this->RunScript(0x1e, nullptr, button);
    CSimpleTop::s_instance->m_inMouseScript = 0;
    if ((this->m_frameFlags & FRAME_PROPAGATE_KEYBOARD) && !this->CLayoutFrame::IsProtected()) {
        return false;
    }
    return true;
}

// OnGamePadStick gets the stick's name and its position from the gamepad's state.
// ref: FUN_140619310
bool CSimpleFrame::OnPadStick(void* event) {
    if (!this->HasScriptAny(0x1f)) {
        return false;
    }
    (void)event;
    // FUN_141544110 (the stick's name) and FUN_14152cf90 (the gamepad state) belong to the
    // gamepad input layer, which is not ported.
    FROST_UNIMPLEMENTED();
    if ((this->m_frameFlags & FRAME_PROPAGATE_KEYBOARD) && !this->CLayoutFrame::IsProtected()) {
        return false;
    }
    return true;
}

// ref: FUN_1406197c0
bool CSimpleFrame::OnMouseWheel(MOUSEEVENT* event) {
    if (CSimpleTop::s_instance->m_inputSuspended || !this->HasScriptAny(0x12)) {
        return false;
    }
    // FUN_14060f420
    CSimpleTop::s_instance->m_inMouseScript = 1;
    this->RunScript(0x12, nullptr, event->m_unknown18 < 0 ? -1 : 1);
    CSimpleTop::s_instance->m_inMouseScript = 0;
    return true;
}

// A press on a frame not yet dragging: the button is remembered, the frame raised, a drag
// armed when the button may drag it, then OnMouseDown runs.
// ref: FUN_140619550
bool CSimpleFrame::OnMouseDown(MOUSEEVENT* event, const char* button) {
    CSimpleTop* top = CSimpleTop::s_instance;
    if (top->m_inputSuspended || (this->m_flags2 & FRAME2_DRAGGING)) {
        return false;
    }
    this->m_buttonsDown |= static_cast<uint32_t>(event->m_button);
    if (!(this->m_frameFlags & FRAME_TOPLEVEL)) {
        for (CSimpleFrame* parent = this->m_parent; parent != nullptr; parent = parent->m_parent) {
            if (parent->m_frameFlags & FRAME_TOPLEVEL) {
                parent->Raise();
                break;
            }
        }
    } else if (this->m_raiseOrder != top->m_raiseCounter) {
        top->m_raiseCounter++;
        this->SetRaiseOrder(top->m_raiseCounter);
    }
    if (button == nullptr) {
        if (static_cast<uint32_t>(event->m_button) & this->m_dragButtons) {
            this->m_flags2 = (this->m_flags2 & ~FRAME2_DRAGGING) | FRAME2_DRAG_PENDING;
            this->m_dragStartX = event->m_x;
            this->m_dragStartY = event->m_y;
        }
        button = MouseButtonToString(event->m_button);
    }
    const char* previous = top->m_currentButton;
    top->m_currentButton = button;
    // FUN_14060f3a0
    top->m_inMouseScript = 1;
    this->RunScript(16, nullptr, button);
    CSimpleTop::s_instance->m_inMouseScript = 0;
    top->m_currentButton = previous;
    return false;
}

// A release ends a drag (OnDragStop, then OnReceiveDrag for whatever is under the cursor);
// otherwise OnMouseUp runs with whether the release was inside.
// ref: FUN_140619680
bool CSimpleFrame::OnMouseUp(MOUSEEVENT* event, const char* button) {
    if (!(this->m_buttonsDown & static_cast<uint32_t>(event->m_button))) {
        return false;
    }
    this->m_buttonsDown &= ~static_cast<uint32_t>(event->m_button);
    CSimpleTop* top = CSimpleTop::s_instance;
    if (top->m_inputSuspended) {
        return false;
    }
    if (button == nullptr) {
        if (static_cast<uint32_t>(event->m_button) & this->m_dragButtons) {
            uint32_t flags2 = this->m_flags2;
            this->m_flags2 = flags2 & ~FRAME2_DRAG_PENDING;
            if (flags2 & FRAME2_DRAGGING) {
                this->OnDragStop(event);
                auto* over = static_cast<blz::vector<CScriptRegion*, s_regionVectorTag>*>(&top->m_mouseOver);
                for (CScriptRegion* region : *over) {
                    region->OnReceiveDrag(event);
                }
                this->m_flags2 &= ~FRAME2_DRAGGING;
                return false;
            }
        }
        button = MouseButtonToString(event->m_button);
    }
    const char* previous = top->m_currentButton;
    top->m_currentButton = button;
    float point[2] = {event->m_x, event->m_y};
    bool inside = this->IsMouseOver(point);
    // FUN_14060f3e0
    top->m_inMouseScript = 1;
    this->RunScript(17, nullptr, button, inside);
    CSimpleTop::s_instance->m_inMouseScript = 0;
    top->m_currentButton = previous;
    return false;
}

// A cursor that moved far enough from an armed press starts the drag.
// ref: FUN_140619840
bool CSimpleFrame::OnMouseMove(MOUSEEVENT* event) {
    float point[2] = {event->m_x, event->m_y};
    uint32_t flags2 = this->m_flags2;
    if ((flags2 & FRAME2_DRAG_PENDING) && !(flags2 & FRAME2_DRAGGING)) {
        float dx = point[0] - this->m_dragStartX;
        float dy = point[1] - this->m_dragStartY;
        if (DRAG_THRESHOLD <= dy * dy + dx * dx) {
            this->m_flags2 = flags2 | FRAME2_DRAGGING;
            this->OnDragStart(event);
            this->m_dragStartX = 0.0f;
            this->m_dragStartY = 0.0f;
        }
    }
    return this->IsMouseOver(point);
}

// The region (or child) the event lands on, front first; a frame armed for a drag past the
// threshold takes it.
// ref: FUN_1406159c0
CScriptRegion* CSimpleFrame::GetMouseTarget(int32_t type, MOUSEEVENT* event, bool* handled, bool force) {
    CSimpleTop* top = CSimpleTop::s_instance;
    if (top->m_inputSuspended) {
        return nullptr;
    }
    bool self;
    if (type == INPUT_EVENT_MOTION) {
        self = this->IsMouseMotionEnabled();
    } else {
        self = this->IsMouseClickEnabled() && (force || !(static_cast<uint32_t>(event->m_button) & this->m_passThroughButtons));
        if (self && (this->m_flags2 & FRAME2_DRAG_PENDING) && !(this->m_flags2 & FRAME2_DRAGGING)) {
            float dx = event->m_x - this->m_dragStartX;
            float dy = event->m_y - this->m_dragStartY;
            if (DRAG_THRESHOLD <= dx * dx + dy * dy) {
                return this;
            }
        }
    }
    if (this->GetHyperlinksEnabled()) {
        // a SimpleHTML lays its text out before its regions are hit-tested, without OnSizeChanged
        bool html = this->IsObjectType(CSimpleHTML_GetObjectType());
        if (html) {
            if (this->CLayoutFrame::m_flags & FLAG_DIRTY) {
                this->CLayoutFrame::Resize(true);
            }
            this->m_flags2 |= FRAME2_NO_SIZE_SCRIPT;
        }
        CScriptRegion* best = nullptr;
        ForEachRegion(this, [&](CSimpleRegion* region) {
            MOUSEEVENT copy = *event;
            int32_t hit = region->HitTest(type, &copy, force);
            if (hit != 0 && (best == nullptr || top->IsRegionAbove(best, region))) {
                best = region;
                *handled = hit == 2;
            }
        });
        if (html) {
            this->m_flags2 &= ~FRAME2_NO_SIZE_SCRIPT;
        }
        if (best != nullptr) {
            return best;
        }
    }
    if (!self) {
        return nullptr;
    }
    float point[2] = {event->m_x, event->m_y};
    return this->IsMouseOver(point) ? this : nullptr;
}

// The highlight layer shows while the cursor is over the frame (unless locked).
// ref: FUN_140619140
void CSimpleFrame::OnEnter(bool motion) {
    if (CSimpleTop::s_instance->m_inputSuspended) {
        return;
    }
    if (!(this->m_frameFlags & FRAME_HIGHLIGHT_LOCKED) && !(this->m_flags2 & 0x10)) {
        this->m_flags2 |= 0x10;
        this->MarkBatchDirty();
    }
    this->CScriptRegion::OnEnter(motion);
}

// ref: FUN_1406191a0
void CSimpleFrame::OnLeave(bool motion) {
    if (CSimpleTop::s_instance->m_inputSuspended) {
        return;
    }
    if (!(this->m_frameFlags & FRAME_HIGHLIGHT_LOCKED) && (this->m_flags2 & 0x10)) {
        this->m_flags2 &= ~0x10u;
        this->MarkBatchDirty();
    }
    this->CScriptRegion::OnLeave(motion);
}

// ref: FUN_140617190
bool CSimpleFrame::IsDragging() {
    if (this->m_flags2 & FRAME2_DRAGGING) {
        return true;
    }
    return this->m_parent != nullptr && this->m_parent->IsDragging();
}

// ref: FUN_140619120
void CSimpleFrame::OnMouseFocusLost(bool clear, bool quiet) {
    (void)clear;
    if (!CSimpleTop::s_instance->m_inputSuspended && quiet) {
        this->m_flags2 &= ~(FRAME2_DRAGGING | FRAME2_DRAG_PENDING);
    }
}

// OnDragStart gets the first button of the event.
// ref: FUN_140618ca0
void CSimpleFrame::OnDragStart(MOUSEEVENT* event) {
    if (CSimpleTop::s_instance->m_inputSuspended) {
        return;
    }
    uint32_t bit = 0;
    for (int32_t i = 0; i < 0x1f; i++) {
        bit = 1u << i;
        if (bit & static_cast<uint32_t>(event->m_button)) {
            break;
        }
        bit = 0;
    }
    this->RunScript(0x13, nullptr, MouseButtonToString(static_cast<int32_t>(bit)));
}

// ref: FUN_140618d00
void CSimpleFrame::OnDragStop(MOUSEEVENT* event) {
    (void)event;
    if (!CSimpleTop::s_instance->m_inputSuspended) {
        this->RunScript(0x14, nullptr);
    }
}

// ref: FUN_1406198f0
void CSimpleFrame::OnReceiveDrag(MOUSEEVENT* event) {
    (void)event;
    if (!CSimpleTop::s_instance->m_inputSuspended) {
        this->RunScript(0x15, nullptr);
    }
}

// ref: FUN_140614ad0
void CSimpleFrame::OnScriptHandlerLoaded(XMLOBJECT* handler) {
    uint32_t flags = static_cast<XMLSCRIPTHANDLER*>(handler)->m_handlerFlags;
    if ((flags & 0x204) == 0x204) {
        this->EnableInputEvent(0);
    }
    if ((flags & 0x208) == 0x208) {
        this->EnableInputEvent(1);
    }
    if ((flags & 0x210) == 0x210) {
        this->EnableInputEvent(5);
    }
    if ((flags & 0x220) == 0x220) {
        this->EnableInputEvent(6);
    }
    this->CScriptRegion::OnScriptHandlerLoaded(handler);
}

// ref: FUN_14061a830
void CSimpleFrame::RunOnLoad() {
    this->CScriptRegion::RunOnLoad();
    if (s_loadHook != nullptr) {
        s_loadHook(this, s_loadHookContext);
    }
}

// ref: FUN_1406198d0
void CSimpleFrame::RunOnUpdate(float elapsed) {
    // FUN_140603740
    this->RunScript(10, nullptr, elapsed);
}

void CSimpleFrame::Unknown280() {
}

bool CSimpleFrame::WantsUpdate() {
    return false;
}

void CSimpleFrame::Unknown2B8() {
}

void CSimpleFrame::Unknown2C0() {
}

// ref: FUN_140618c20
void CSimpleFrame::Unknown2C8() {
    ForEachRegion(this, [](CSimpleRegion* region) {
        region->Unknown290();
    });
}

void CSimpleFrame::OnEditFocusChanged(bool focused, bool runScripts, bool userInput) {
    (void)focused;
    (void)runScripts;
    (void)userInput;
}

// ---- dragging ------------------------------------------------------------------------------

// ref: FUN_140619f80
void CSimpleFrame::RegisterForDrag(const uint32_t* buttons, uint64_t count) {
    uint32_t mask = 0;
    for (uint64_t i = 0; i < count; i++) {
        mask |= buttons[i];
    }
    this->m_dragButtons = mask;
}

// ref: FUN_14061bda0
void CSimpleFrame::StartMoving(bool alwaysStartFromMouse) {
    if (!(this->m_frameFlags & FRAME_MOVABLE)) {
        FrameScript_ActionError("Frame is not movable");
        return;
    }
    CSimpleTop* top = CSimpleTop::s_instance;
    if (top->m_dragFrame != nullptr) {
        return;
    }
    float x = this->m_dragStartX;
    float y = this->m_dragStartY;
    if (alwaysStartFromMouse || (x == 0.0f && y == 0.0f)) {
        x = top->m_mouseEvent.m_x;
        y = top->m_mouseEvent.m_y;
    }
    top->StartDrag(this, 3, x, y, 4);
}

// ref: FUN_14061be30
void CSimpleFrame::StartSizing(int32_t corner, bool alwaysStartFromMouse) {
    CSimpleTop* top = CSimpleTop::s_instance;
    if (top->m_dragFrame != nullptr) {
        return;
    }
    float x = this->m_dragStartX;
    float y = this->m_dragStartY;
    if (alwaysStartFromMouse || (x == 0.0f && y == 0.0f)) {
        x = top->m_mouseEvent.m_x;
        y = top->m_mouseEvent.m_y;
    }
    top->StartDrag(this, 3, x, y, corner);
}

// ref: FUN_14061bea0
void CSimpleFrame::StartSizingChecked(const int32_t* corner, bool alwaysStartFromMouse) {
    if (!(this->m_frameFlags & FRAME_RESIZABLE)) {
        FrameScript_ActionError("Frame is not resizable");
        return;
    }
    if (reinterpret_cast<const uint8_t*>(corner)[4] != 0) {
        this->StartSizing(*corner, alwaysStartFromMouse);
        return;
    }
    this->StartSizing(8, alwaysStartFromMouse);
}

// ref: FUN_140617280
bool CSimpleFrame::IsKeyboardEnabled() {
    return (this->m_enabledInput & (1 << 1)) != 0 || (this->m_enabledInput & (1 << 0)) != 0;
}

// ref: FUN_140617210
bool CSimpleFrame::IsGamePadButtonEnabled() {
    return (this->m_enabledInput & (1 << 5)) != 0;
}

// ref: FUN_140617240
bool CSimpleFrame::IsGamePadStickEnabled() {
    return (this->m_enabledInput & (1 << 6)) != 0;
}

// ref: FUN_1406162a0
void CSimpleFrame::GetHitRectInsetsLua(float* left, float* right, float* top, float* bottom) {
    const float* insets = this->GetHitRectInsets();
    *left = insets[1];
    *right = insets[3];
    *top = insets[2];
    *bottom = insets[0];
}

// ref: FUN_14061bfb0
void CSimpleFrame::StopMovingOrSizing() {
    if (CSimpleTop::s_instance->m_dragFrame == this) {
        CSimpleTop::s_instance->StopDrag();
    }
}

// A drag in progress ends without a release: the capture goes and OnDragStop runs with the
// cursor at the centre.
// ref: FUN_140613b10
void CSimpleFrame::CancelDrag() {
    if (!(this->m_flags2 & FRAME2_DRAGGING)) {
        return;
    }
    this->m_flags2 &= ~(FRAME2_DRAGGING | FRAME2_DRAG_PENDING);
    CSimpleTop::s_instance->ReleaseButtonCapture(this);
    MOUSEEVENT event = {};
    event.m_x = 0.5f;
    event.m_y = 0.5f;
    event.m_flags = 0x100;
    this->OnDragStop(&event);
}

// Hands a drag in progress to another frame, with the buttons and drag buttons it had.
// ref: FUN_140616c80
void CSimpleFrame::InterceptStartDrag(CSimpleFrame* other) {
    CSimpleTop* top = CSimpleTop::s_instance;
    if (other == nullptr || other == this) {
        FrameScript_ActionError("A valid intercept frame was not provided");
        return;
    }
    if (!(this->m_flags2 & FRAME2_DRAGGING)) {
        return;
    }
    const char* error;
    if (!top->AddButtonCapture(other)) {
        error = "Couldn't add click capture for intercept frame";
    } else if (top->ReleaseButtonCapture(this)) {
        this->m_flags2 &= ~(FRAME2_DRAGGING | FRAME2_DRAG_PENDING);
        other->m_buttonsDown = this->m_buttonsDown;
        uint32_t dragButtons = this->m_dragButtons;
        this->m_buttonsDown = 0;
        other->m_flags2 |= FRAME2_DRAGGING | FRAME2_DRAG_PENDING;
        other->m_dragButtons = dragButtons;
        return;
    } else {
        error = "Cannot intercept the drag without the frame having capture";
    }
    FrameScript_ActionError(error);
}

// ---- attributes ------------------------------------------------------------------------------

// ref: FUN_140615cc0
bool CSimpleFrame::GetAttribute(const char* name, int32_t* ref, const char** taint) {
    FRAMEATTR* attr = this->m_attributes.Find(name);
    if (attr != nullptr && attr->m_ref != -1) {
        *ref = attr->m_ref;
        *taint = attr->m_taint;
        return true;
    }
    return false;
}

// ref: FUN_14061a940
void CSimpleFrame::SetAttribute(const char* name, int32_t ref, const char* taint, bool quiet) {
    FRAMEATTR* attr = this->m_attributes.Find(name);
    if (attr == nullptr) {
        attr = this->m_attributes.Add(name, 0, 0);
    }
    attr->m_ref = ref;
    attr->m_taint = taint;
    if (!quiet) {
        this->RunOnAttributeChanged(name, ref);
    }
}

// OnAttributeChanged gets the name in lower case and the value, untainted.
// ref: FUN_14061a210
void CSimpleFrame::RunOnAttributeChanged(const char* name, int32_t ref) {
    if (!this->HasScriptAny(0x20)) {
        return;
    }
    lua_State* L = FrameScript_GetContext();
    const char* previous = lua_taint;
    FrameScript_EnterTaint(L, nullptr);
    uint64_t length = strlen(name);
    char lower[0x400];
    char* text = length < sizeof(lower) ? lower : static_cast<char*>(SMemAlloc(length + 1, 0, nullptr, 0, 0, 0));
    memcpy(text, name, length + 1);
    _strlwr(text);
    // FUN_140612ad0
    this->RunScript(0x20, nullptr, static_cast<const char*>(text), FRAMESCRIPT_REF{ref});
    if (text != lower) {
        SMemFree(text, nullptr, 0, 0);
    }
    FrameScript_LeaveTaint(L, previous);
}

// ---- bounds ----------------------------------------------------------------------------------

// The union of the frame's rect, its regions' and its shown children's.
// ref: FUN_140615ea0
int32_t CSimpleFrame::CalculateBounds(CRect* rect) {
    int32_t reason = ScriptRegion_CheckRestriction(ACTION_FRAME_MEASUREMENT, this->GetScriptRegion());
    if (reason != 0) {
        return reason;
    }
    if (this->CLayoutFrame::m_flags & FLAG_DIRTY) {
        this->CLayoutFrame::Resize(true);
    }
    CRect own = {};
    if (this->CLayoutFrame::GetRect(&own)) {
        rect->minX = own.minX <= rect->minX ? own.minX : rect->minX;
        rect->maxX = rect->maxX <= own.maxX ? own.maxX : rect->maxX;
        rect->minY = own.minY <= rect->minY ? own.minY : rect->minY;
        rect->maxY = own.maxY <= rect->maxY ? rect->maxY : own.maxY;
    }
    ForEachRegion(this, [&](CSimpleRegion* region) {
        if (!(region->m_layoutState & LAYOUT_SHOWN) || (static_cast<FrameScript_Object*>(region)->m_flags & FSOBJ_HIDDEN)) {
            return;
        }
        if (region->CLayoutFrame::m_flags & FLAG_DIRTY) {
            region->CLayoutFrame::Resize(true);
        }
        if (region->CLayoutFrame::GetRect(&own)) {
            rect->minX = rect->minX <= own.minX && own.minX != rect->minX ? rect->minX : own.minX;
            rect->maxX = own.maxX < rect->maxX ? rect->maxX : own.maxX;
            rect->minY = rect->minY <= own.minY && own.minY != rect->minY ? rect->minY : own.minY;
            rect->maxY = own.maxY < rect->maxY ? rect->maxY : own.maxY;
        }
    });
    for (SIMPLEFRAMENODE* node = this->m_children.Head(); node != nullptr;) {
        SIMPLEFRAMENODE* next = this->m_children.Link(node)->Next();
        CSimpleFrame* child = node->m_frame;
        if (this->IsChildShown(child)) {
            int32_t childReason = child->CalculateBounds(rect);
            // FUN_14062d1f0
            if (childReason > 1) {
                return childReason;
            }
        }
        node = next;
    }
    if (rect->minX < rect->maxX && rect->minY < rect->maxY) {
        return 0;
    }
    return 1;
}

// ref: FUN_140615d10
CRect* CSimpleFrame::GetBoundsRect(CRect* rect) {
    CRect bounds = {3.4028235e+38f, 3.4028235e+38f, 1.1754944e-38f, 1.1754944e-38f};
    if (this->CalculateBounds(&bounds) == 0) {
        float inverse = 1.0f / this->m_layoutScale;
        rect->minY = bounds.minY * inverse;
        rect->minX = bounds.minX * inverse;
        rect->maxY = bounds.maxY * inverse;
        rect->maxX = bounds.maxX * inverse;
        return rect;
    }
    ScriptRegion_CheckAction(ACTION_FRAME_MEASUREMENT, this->GetScriptRegion(), "GetBoundsRect");
    *rect = {};
    return rect;
}

// ref: FUN_140615dc0
void CSimpleFrame::GetBoundsRectLua(float* left, float* bottom, float* width, float* height) {
    CRect bounds = {3.4028235e+38f, 3.4028235e+38f, 1.1754944e-38f, 1.1754944e-38f};
    if (this->CalculateBounds(&bounds) == 0) {
        float inverse = 1.0f / this->m_layoutScale;
        bounds.minY *= inverse;
        bounds.minX *= inverse;
        bounds.maxY *= inverse;
        bounds.maxX *= inverse;
    } else {
        ScriptRegion_CheckAction(ACTION_FRAME_MEASUREMENT, this->GetScriptRegion(), "GetBoundsRect");
        bounds = {};
    }
    *left = bounds.minX;
    *bottom = bounds.minY;
    *width = bounds.maxX - bounds.minX;
    *height = bounds.maxY - bounds.minY;
}

// ---- the animatable half -----------------------------------------------------------------

// ref: FUN_140619d50
void CSimpleFrame::Unknown08() {
    if (this->CLayoutFrame::HasAnimAlpha()) {
        this->SetAlpha(this->CLayoutFrame::GetAnimAlpha());
    }
    ForEachChild(this, [](CSimpleFrame* child) {
        static_cast<CAnimatableObject*>(child)->Unknown08();
    });
    ForEachRegion(this, [](CSimpleRegion* region) {
        static_cast<CAnimatableObject*>(region)->Unknown08();
    });
}

// ref: FUN_140619c20
void CSimpleFrame::Unknown10() {
    ForEachChild(this, [](CSimpleFrame* child) {
        static_cast<CAnimatableObject*>(child)->Unknown10();
    });
    ForEachRegion(this, [](CSimpleRegion* region) {
        static_cast<CAnimatableObject*>(region)->Unknown10();
    });
}

// ref: FUN_14061bed0
void CSimpleFrame::StopAnimating() {
    this->CAnimatableObject::StopAnimating();
    ForEachChild(this, [](CSimpleFrame* child) {
        static_cast<CAnimatableObject*>(child)->StopAnimating();
    });
    ForEachRegion(this, [](CSimpleRegion* region) {
        static_cast<CAnimatableObject*>(region)->StopAnimating();
    });
}

// ref: FUN_140614640
void CSimpleFrame::OnAnimStarted(bool alpha, bool transform, bool rectOffset) {
    if (this->m_groups == nullptr || this->m_groups->m_playing == nullptr || this->m_groups->m_playing->Head() == nullptr) {
        this->CLayoutFrame::Resize(true);
    }
    if (alpha) {
        this->CLayoutFrame::SetAnimAlpha(this->m_alpha);
    }
    ForEachChild(this, [&](CSimpleFrame* child) {
        static_cast<CAnimatableObject*>(child)->OnAnimStarted(alpha, transform, rectOffset);
    });
    ForEachRegion(this, [&](CSimpleRegion* region) {
        static_cast<CAnimatableObject*>(region)->OnAnimStarted(alpha, transform, rectOffset);
    });
}

// ref: FUN_140614770
void CSimpleFrame::OnAnimStopped(CSimpleAnimGroup* group, bool alpha, bool transform, bool rectOffset) {
    uint8_t flags = CSimpleAnimGroup_GetFlags(group);
    if (!(flags & 8) && (flags & 0x10)) {
        uint8_t current = this->CLayoutFrame::HasAnimAlpha() ? this->CLayoutFrame::GetAnimAlpha() : this->GetAlpha();
        this->CLayoutFrame::SetAnimAlpha(CSimpleAnimGroup_GetFinalAlpha(group, this, current));
    }
    if (alpha) {
        this->SetAlpha(this->CLayoutFrame::GetAnimAlpha());
        this->CLayoutFrame::ClearAnimAlpha();
    }
    if (transform) {
        this->CLayoutFrame::ClearAnimTransform();
    }
    ForEachChild(this, [&](CSimpleFrame* child) {
        static_cast<CAnimatableObject*>(child)->OnAnimStopped(group, alpha, transform, rectOffset);
    });
    ForEachRegion(this, [&](CSimpleRegion* region) {
        static_cast<CAnimatableObject*>(region)->OnAnimStopped(group, alpha, transform, rectOffset);
    });
}

// The animation values pass down to every child and region.
// ref: FUN_140614210
void CSimpleFrame::Unknown40(void* value) {
    ForEachChild(this, [&](CSimpleFrame* child) {
        static_cast<CAnimatableObject*>(child)->Unknown40(value);
    });
    uint64_t copy = *static_cast<uint64_t*>(value);
    ForEachRegion(this, [&](CSimpleRegion* region) {
        static_cast<CAnimatableObject*>(region)->Unknown40(&copy);
    });
}

// ref: FUN_140613dc0
void CSimpleFrame::Unknown48(void* value) {
    ForEachChild(this, [&](CSimpleFrame* child) {
        static_cast<CAnimatableObject*>(child)->Unknown48(value);
    });
    uint64_t copy = *static_cast<uint64_t*>(value);
    ForEachRegion(this, [&](CSimpleRegion* region) {
        static_cast<CAnimatableObject*>(region)->Unknown48(&copy);
    });
}

// ref: FUN_140614120
void CSimpleFrame::Unknown50(void* value) {
    ForEachChild(this, [&](CSimpleFrame* child) {
        static_cast<CAnimatableObject*>(child)->Unknown50(value);
    });
    uint64_t copy = *static_cast<uint64_t*>(value);
    ForEachRegion(this, [&](CSimpleRegion* region) {
        static_cast<CAnimatableObject*>(region)->Unknown50(&copy);
    });
}

// ref: FUN_140613eb0
void CSimpleFrame::Unknown58(int32_t index, void* value, float extra) {
    ForEachChild(this, [&](CSimpleFrame* child) {
        static_cast<CAnimatableObject*>(child)->Unknown58(index, value, extra);
    });
    uint64_t copy = *static_cast<uint64_t*>(value);
    ForEachRegion(this, [&](CSimpleRegion* region) {
        static_cast<CAnimatableObject*>(region)->Unknown58(index, &copy, extra);
    });
}

// ref: FUN_140613fe0
void CSimpleFrame::Unknown60(int32_t index, void* first, void* second) {
    ForEachChild(this, [&](CSimpleFrame* child) {
        static_cast<CAnimatableObject*>(child)->Unknown60(index, first, second);
    });
    uint64_t firstCopy = *static_cast<uint64_t*>(first);
    uint64_t secondCopy = *static_cast<uint64_t*>(second);
    ForEachRegion(this, [&](CSimpleRegion* region) {
        static_cast<CAnimatableObject*>(region)->Unknown60(index, &firstCopy, &secondCopy);
    });
}

// ref: FUN_140613c80
void CSimpleFrame::Unknown68(int32_t index, void* first, void* second) {
    ForEachChild(this, [&](CSimpleFrame* child) {
        static_cast<CAnimatableObject*>(child)->Unknown68(index, first, second);
    });
    uint64_t firstCopy = *static_cast<uint64_t*>(first);
    uint64_t secondCopy = *static_cast<uint64_t*>(second);
    ForEachRegion(this, [&](CSimpleRegion* region) {
        static_cast<CAnimatableObject*>(region)->Unknown68(index, &firstCopy, &secondCopy);
    });
}

// ref: FUN_14061a920
void CSimpleFrame::SetAnimAlpha(uint8_t alpha) {
    this->SetAlpha(alpha);
}

// ref: FUN_1406149e0
CScriptRegion* CSimpleFrame::GetScriptRegion() {
    return this;
}

// ---- animations in XML ------------------------------------------------------------------------

// ref: FUN_14062fc00
void CSimpleFrame_LoadAnimations(CScriptRegion* object, XMLOBJECT* node, CStatus* status) {
    for (XMLOBJECT* child = node->m_child; child != nullptr; child = child->m_next) {
        if (child->m_type != XMLTYPE_ANIMATIONS) {
            continue;
        }
        for (XMLOBJECT* group = child->m_child; group != nullptr; group = group->m_next) {
            if (group->m_type == XMLTYPE_ANIMATIONGROUP) {
                FrameXML_CreateAnimGroup(group, object, status);
            }
        }
    }
}

// ---- key names ---------------------------------------------------------------------------------

namespace {

// 0x1426c9d50
const char* const s_padButtonNames[32] = {
    "PADDUP", "PADDRIGHT", "PADDDOWN", "PADDLEFT", "PAD1", "PAD2", "PAD3", "PAD4",
    "PAD5", "PAD6", "PADLSTICK", "PADRSTICK", "PADLSHOULDER", "PADRSHOULDER", "PADLTRIGGER", "PADRTRIGGER",
    "PADLSTICKUP", "PADLSTICKRIGHT", "PADLSTICKDOWN", "PADLSTICKLEFT", "PADRSTICKUP", "PADRSTICKRIGHT", "PADRSTICKDOWN", "PADRSTICKLEFT",
    "PADPADDLE1", "PADPADDLE2", "PADPADDLE3", "PADPADDLE4", "PADFORWARD", "PADBACK", "PADSYSTEM", "PADSOCIAL",
};

// The name of a printable key (0x142e97b20).
char s_keyName[8];

}

// ref: FUN_14060fd50
const char* GamePadButtonToString(uint8_t button) {
    if (button < 0x20) {
        return s_padButtonNames[button];
    }
    return "UNKNOWN";
}

// Printable keys name themselves (as UTF-8); the rest have fixed names.
// ref: FUN_14060fff0
const char* KeyToString(int32_t key) {
    if (static_cast<uint32_t>(key - 0x21) < 0xdf) {
        // FUN_14060fb70: the key's Latin-1 character, converted
        uint8_t* end = SUniSPutUTF8(static_cast<uint32_t>(key) & 0xff, reinterpret_cast<uint8_t*>(s_keyName));
        *end = 0;
        return s_keyName;
    }
    switch (key) {
        case 0: return "LSHIFT";
        case 1: return "RSHIFT";
        case 2: return "LCTRL";
        case 3: return "RCTRL";
        case 4: return "LALT";
        case 5: return "RALT";
        case 6: return "LMETA";
        case 7: return "RMETA";
        case 0x20: return "SPACE";
        case -1: return "NONE";
        case 0x100: return "NUMPAD0";
        case 0x101: return "NUMPAD1";
        case 0x102: return "NUMPAD2";
        case 0x103: return "NUMPAD3";
        case 0x104: return "NUMPAD4";
        case 0x105: return "NUMPAD5";
        case 0x106: return "NUMPAD6";
        case 0x107: return "NUMPAD7";
        case 0x108: return "NUMPAD8";
        case 0x109: return "NUMPAD9";
        case 0x10a: return "NUMPADPLUS";
        case 0x10b: return "NUMPADMINUS";
        case 0x10c: return "NUMPADMULTIPLY";
        case 0x10d: return "NUMPADDIVIDE";
        case 0x10e: return "NUMPADDECIMAL";
        case 0x10f: return "ESCAPE";
        case 0x110: return "ENTER";
        case 0x111: return "BACKSPACE";
        case 0x112: return "TAB";
        case 0x113: return "LEFT";
        case 0x114: return "UP";
        case 0x115: return "RIGHT";
        case 0x116: return "DOWN";
        case 0x117: return "INSERT";
        case 0x118: return "DELETE";
        case 0x119: return "HOME";
        case 0x11a: return "END";
        case 0x11b: return "PAGEUP";
        case 0x11c: return "PAGEDOWN";
        case 0x11d: return "CAPSLOCK";
        case 0x11e: return "NUMLOCK";
        case 0x121: return "PRINTSCREEN";
        case 0x123: return "F1";
        case 0x124: return "F2";
        case 0x125: return "F3";
        case 0x126: return "F4";
        case 0x127: return "F5";
        case 0x128: return "F6";
        case 0x129: return "F7";
        case 0x12a: return "F8";
        case 0x12b: return "F9";
        case 0x12c: return "F10";
        case 0x12d: return "F11";
        case 0x12e: return "F12";
        case 0x12f: return "NUMPADEQUALS";
        case 0x130: return "F14";
        case 0x131: return "F15";
        case 0x132: return "F16";
        case 0x133: return "F17";
        case 0x134: return "F18";
        case 0x135: return "F19";
        default: return "UNKNOWN";
    }
}
