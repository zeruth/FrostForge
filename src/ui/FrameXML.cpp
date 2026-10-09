#include "ui/FrameXML.hpp"

#include <cstddef>
#include <cstring>

#include "console/Var.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "storm/hash/TSHashTable.hpp"
#include "ui/BinaryXML.hpp"
#include "util/Unimplemented.hpp"

namespace {

// HASHKEY_STRI: an owned copy of the name, compared case-insensitively.
struct FrameFactoryNode : TSHashObject<FrameFactoryNode> {
    char* m_str;                // 0x30
    FRAMEFACTORY m_factory;     // 0x38
    bool m_unknown;             // 0x40
};
static_assert(offsetof(FrameFactoryNode, m_str) == 0x30, "FrameFactoryNode layout");
static_assert(offsetof(FrameFactoryNode, m_factory) == 0x38, "FrameFactoryNode layout");
static_assert(sizeof(FrameFactoryNode) == 0x48, "FrameFactoryNode is 0x48 bytes");

const char s_nodeTag[] = ".?AV?$TSList@UFrameFactoryNode@@V?$TSGetExplicitLink@UFrameFactoryNode@@@@$07@@";
const char s_tableTag[] = ".?AV?$TSHashTable@UFrameFactoryNode@@VHASHKEY_STRI@@$07@@";

// TSHashTable<FrameFactoryNode, HASHKEY_STRI, 8> (vtable 0x1426ce4d8).
class FrameFactoryTable : public TSHashTable<FrameFactoryNode> {
    public:
    // ref: FUN_140661ca0
    void Delete(FrameFactoryNode* node) override {
        // FUN_140662500: out of the table, the name freed, then both links
        if (node->m_table) {
            static_cast<FrameFactoryTable*>(node->m_table)->Unlink(node);
        }
        SMemFree(node->m_str, nullptr, 0, 0);
        node->m_str = nullptr;
        node->m_linktofull.Unlink();
        node->m_linktoslot.Unlink();
        SMemFree(node, s_tableTag, -2, 0);
    }

    // ref: FUN_140661cd0
    FrameFactoryNode* NewNode(TSExplicitList<FrameFactoryNode>* slot, uint32_t extra, uint32_t flags) override {
        auto node = static_cast<FrameFactoryNode*>(
            SMemAlloc(uint64_t(extra) + sizeof(FrameFactoryNode), 8, s_nodeTag, 0, -2, flags | SMEM_FLAG_ZEROMEMORY));
        node->m_linktoslot.m_prevlink = nullptr;
        node->m_linktoslot.m_next = nullptr;
        node->m_linktofull.m_prevlink = nullptr;
        node->m_linktofull.m_next = nullptr;
        node->m_table = nullptr;
        node->m_hashval = 0;
        node->m_str = nullptr;
        slot->LinkToHead(node);
        return node;
    }

    FrameFactoryNode* Find(const char* name) {
        if (this->m_slotmod == 0xFFFFFFFF) {
            return nullptr;
        }
        uint32_t hash = SStrHashHT(name);
        TSExplicitList<FrameFactoryNode>* slot = &this->m_slots[hash % this->m_slotmod];
        for (FrameFactoryNode* node = slot->Head(); node; ) {
            if (node->m_hashval == hash && _stricmp(node->m_str, name) == 0) {
                return node;
            }
            FrameFactoryNode* next = slot->Link(node)->m_next;
            uintptr_t raw = reinterpret_cast<uintptr_t>(next);
            node = (raw & 1) || raw == 0 ? nullptr : next;
        }
        return nullptr;
    }
};

// A virtual or intrinsic element, compiled, by name (HASHKEY_STRI).
struct HashedNode : TSHashObject<HashedNode> {
    char* m_str;                // 0x30
    XMLOBJECT* m_object;        // 0x38
    const char* m_taint;        // 0x40 the taint it was defined under
    bool m_busy;                // 0x48 being expanded
    int32_t m_refs;             // 0x4C expansions in progress
};
static_assert(offsetof(HashedNode, m_object) == 0x38, "HashedNode layout");
static_assert(offsetof(HashedNode, m_refs) == 0x4c, "HashedNode layout");
static_assert(sizeof(HashedNode) == 0x50, "HashedNode is 0x50 bytes");

const char s_hashedNodeTag[] = ".?AV?$TSList@UHashedNode@@V?$TSGetExplicitLink@UHashedNode@@@@$07@@";
const char s_hashedTableTag[] = ".?AV?$TSHashTable@UHashedNode@@VHASHKEY_STRI@@$07@@";

// TSHashTable<HashedNode, HASHKEY_STRI, 8> (vtable 0x1426ce4b0).
class HashedNodeTable : public TSHashTable<HashedNode> {
    public:
    // ref: FUN_140661ad0
    void Delete(HashedNode* node) override {
        XMLObjectFree(node->m_object, true);
        // FUN_140613680: out of the table, the name freed, then both links
        if (node->m_table) {
            static_cast<HashedNodeTable*>(node->m_table)->Unlink(node);
        }
        SMemFree(node->m_str, nullptr, 0, 0);
        node->m_str = nullptr;
        node->m_linktofull.Unlink();
        node->m_linktoslot.Unlink();
        SMemFree(node, s_hashedTableTag, -2, 0);
    }

    // ref: FUN_140661b10
    HashedNode* NewNode(TSExplicitList<HashedNode>* slot, uint32_t extra, uint32_t flags) override {
        auto node = static_cast<HashedNode*>(
            SMemAlloc(uint64_t(extra) + sizeof(HashedNode), 8, s_hashedNodeTag, 0, -2, flags | SMEM_FLAG_ZEROMEMORY));
        node->m_linktoslot.m_prevlink = nullptr;
        node->m_linktoslot.m_next = nullptr;
        node->m_linktofull.m_prevlink = nullptr;
        node->m_linktofull.m_next = nullptr;
        node->m_table = nullptr;
        node->m_hashval = 0;
        node->m_str = nullptr;
        node->m_object = nullptr;
        node->m_taint = nullptr;
        node->m_busy = false;
        node->m_refs = 0;
        slot->LinkToHead(node);
        return node;
    }

    // ref: FUN_140619e80
    HashedNode* Find(const char* name) {
        if (this->m_slotmod == 0xFFFFFFFF) {
            return nullptr;
        }
        uint32_t hash = SStrHashHT(name);
        TSExplicitList<HashedNode>* slot = &this->m_slots[hash % this->m_slotmod];
        for (HashedNode* node = slot->Head(); node; ) {
            if (node->m_hashval == hash && _stricmp(node->m_str, name) == 0) {
                return node;
            }
            HashedNode* next = slot->Link(node)->m_next;
            uintptr_t raw = reinterpret_cast<uintptr_t>(next);
            node = (raw & 1) || raw == 0 ? nullptr : next;
        }
        return nullptr;
    }
};

HashedNodeTable s_virtualNodes;     // 0x142cf57e0
HashedNodeTable s_intrinsicNodes;   // 0x142cf5830

FrameFactoryTable s_factories;   // 0x142cf5880
CVar* s_validateFrameXML;        // 0x142e9a280

// The engine widget factories: each allocates its class from the class's CDataAllocator pool
// and constructs it under `parent`. The widget classes are not ported yet.

// ref: FUN_140661ea0
CSimpleFrame* CreateButton(CSimpleFrame* parent) {
    (void)parent;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_140661ef0
CSimpleFrame* CreateCheckButton(CSimpleFrame* parent) {
    (void)parent;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_140661f90
CSimpleFrame* CreateEditBox(CSimpleFrame* parent) {
    (void)parent;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_140661fe0
CSimpleFrame* CreateFrame(CSimpleFrame* parent) {
    (void)parent;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_140662080
CSimpleFrame* CreateMessageFrame(CSimpleFrame* parent) {
    (void)parent;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_1406620d0
CSimpleFrame* CreateModel(CSimpleFrame* parent) {
    (void)parent;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_140662240
CSimpleFrame* CreateScrollFrame(CSimpleFrame* parent) {
    (void)parent;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_140662290
CSimpleFrame* CreateSlider(CSimpleFrame* parent) {
    (void)parent;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_140662030
CSimpleFrame* CreateSimpleHTML(CSimpleFrame* parent) {
    (void)parent;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_1406622e0
CSimpleFrame* CreateStatusBar(CSimpleFrame* parent) {
    (void)parent;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_140661f40
CSimpleFrame* CreateColorSelect(CSimpleFrame* parent) {
    (void)parent;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_140662120
CSimpleFrame* CreateMovieFrame(CSimpleFrame* parent) {
    (void)parent;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_140661e50
CSimpleFrame* CreateBrowser(CSimpleFrame* parent) {
    (void)parent;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_1406621f0
CSimpleFrame* CreateOffScreenFrame(CSimpleFrame* parent) {
    (void)parent;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

}  // namespace

// ref: FUN_140664ff0
bool FrameXML_RegisterFactory(const char* type, FRAMEFACTORY factory, bool unknown) {
    if (s_factories.Find(type)) {
        return false;
    }
    uint32_t hash = SStrHashHT(type);
    FrameFactoryNode* node = s_factories.New(hash, 0, 0);
    node->m_hashval = hash;
    if (node->m_str != type) {
        SMemFree(node->m_str, nullptr, 0, 0);
        node->m_str = SStrDupA(type, nullptr, 0);
    }
    node->m_factory = factory;
    node->m_unknown = unknown;
    return true;
}

FRAMEFACTORY FrameXML_GetFactory(const char* type) {
    FrameFactoryNode* node = s_factories.Find(type);
    return node ? node->m_factory : nullptr;
}

// ref: FUN_140662af0
XMLOBJECT* FrameXML_AcquireIntrinsic(const char* name, const char** taint, bool* recursive) {
    HashedNode* node = s_intrinsicNodes.Find(name);
    if (node) {
        *taint = node->m_taint;
        node->m_refs++;
        node->m_busy = true;
        XMLOBJECT* object = node->m_object;
        *recursive = node->m_refs > 1;
        return object;
    }
    *recursive = false;
    return nullptr;
}

// ref: FUN_140665110
void FrameXML_ReleaseIntrinsic(const char* name) {
    HashedNode* node = s_intrinsicNodes.Find(name);
    node->m_refs--;
    node->m_busy = false;
}

// ref: FUN_140664ea0
bool FrameXML_RegisterDefault() {
    FrameXML_RegisterFactory("Button", CreateButton, false);
    FrameXML_RegisterFactory("CheckButton", CreateCheckButton, false);
    FrameXML_RegisterFactory("EditBox", CreateEditBox, false);
    FrameXML_RegisterFactory("Frame", CreateFrame, false);
    FrameXML_RegisterFactory("MessageFrame", CreateMessageFrame, false);
    FrameXML_RegisterFactory("Model", CreateModel, false);
    FrameXML_RegisterFactory("ScrollFrame", CreateScrollFrame, false);
    FrameXML_RegisterFactory("Slider", CreateSlider, false);
    FrameXML_RegisterFactory("SimpleHTML", CreateSimpleHTML, false);
    FrameXML_RegisterFactory("StatusBar", CreateStatusBar, false);
    FrameXML_RegisterFactory("ColorSelect", CreateColorSelect, false);
    FrameXML_RegisterFactory("MovieFrame", CreateMovieFrame, false);
    FrameXML_RegisterFactory("Browser", CreateBrowser, false);
    FrameXML_RegisterFactory("OffScreenFrame", CreateOffScreenFrame, false);
    return true;
}

// ref: FUN_140664e50
void FrameXML_RegisterCVars() {
    s_validateFrameXML = CVarRegister("validateFrameXML", "Display warning when FrameXML detects unparsed elements",
        0, "0", nullptr, 5, false, nullptr, false);
}
