#include "ui/FrameXML.hpp"
#include "ui/CSimpleTexture.hpp"

#include <cstdarg>
#include <cstddef>
#include <cstring>

#include "cas/CAS.hpp"
#include "console/Var.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "os/Os.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "storm/hash/TSHashTable.hpp"
#include "ui/BinaryXML.hpp"
#include "ui/CLayoutFrame.hpp"
#include "ui/CScriptObject.hpp"
#include "ui/CSimpleFont.hpp"
#include "ui/CSimpleFrame.hpp"
#include "ui/XMLTree.hpp"
#include "util/Unimplemented.hpp"

extern "C" {
#include "lblizzard.h"
#include "lua.h"
}

namespace {

// The bounded copy the reference inlines: at most size - 1 characters, always terminated.
void CopyBounded(char* dest, const char* source, uint64_t size) {
    uint64_t i = 0;
    for (; i + 1 < size && source[i] != '\0'; i++) {
        dest[i] = source[i];
    }
    dest[i] = '\0';
}

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

int32_t s_debugLevel;               // 0x142e9a288 above 0: log each node created
bool s_loadingToc;                  // 0x142e9a28c
FRAMEXMLPROGRESS s_progress;        // 0x142e9a290
void* s_progressContext;            // 0x142e9a298
uint32_t s_progressCount;           // 0x142e9a2a0
uint32_t s_progressTotal;           // 0x142e9a2a4
uint32_t s_scopedState;             // 0x142e9a2a8 the scoped-modifier flags in force
uint32_t s_scopedMask;              // 0x142e9a2ac flags no modifier may set

// Whether files may come from local overrides, for the load in progress on this thread.
thread_local bool t_localFiles;     // TLS +0x2210

// The status position a load step reports against, restored when the step ends.
struct LOCATIONGUARD {
    SCRIPTLOCATION m_saved;
    CScriptStatus* m_status;
    bool m_active;

    explicit LOCATIONGUARD(CScriptStatus* status) : m_saved(status->m_location), m_status(status), m_active(true) {
    }
    ~LOCATIONGUARD() {
        this->Restore();
    }
    void Restore() {
        if (this->m_active) {
            XMLStatusRestoreLocation(this->m_status, &this->m_saved);
            this->m_active = false;
        }
    }
};

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
    return new CSimpleFrame(parent);
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

// ---- the load state ---------------------------------------------------------------------

// ref: FUN_140663620
int32_t FrameXML_GetDebugLevel() {
    return s_debugLevel;
}

// ref: FUN_140665150
void FrameXML_SetDebugLevel(int32_t level) {
    s_debugLevel = level;
}

// ref: FUN_1406638f0
bool FrameXML_IsLoadingToc() {
    return s_loadingToc;
}

// ref: FUN_1406650f0
void FrameXML_SetProgress(FRAMEXMLPROGRESS progress, void* context, uint32_t total) {
    s_progress = progress;
    s_progressContext = context;
    s_progressCount = 0;
    s_progressTotal = total;
}

// ref: FUN_140663630
uint32_t FrameXML_GetScopedState() {
    return s_scopedState;
}

// ref: FUN_140665160
void FrameXML_SetScopedState(uint32_t state) {
    s_scopedState = ~s_scopedMask & state;
}

// ref: FUN_140663410
void FrameXML_AddScopedMask(uint32_t mask) {
    s_scopedMask |= mask;
    s_scopedState &= ~s_scopedMask;
}

// ref: FUN_140663430
void FrameXML_ClearScopedMask() {
    s_scopedMask = 0;
}

bool FrameXML_GetLocalFiles() {
    return t_localFiles;
}

// ref: FUN_140667570
void XMLStatusSetLocation(CScriptStatus* status, XMLOBJECT* object) {
    status->m_location = object->m_location;
}

// ref: FUN_140667500
void XMLStatusAddAt(CScriptStatus* status, XMLOBJECT* object, int32_t severity, const char* format, ...) {
    SCRIPTLOCATION saved = status->m_location;
    status->m_location = object->m_location;
    va_list args;
    va_start(args, format);
    status->AddV(severity, format, args);
    va_end(args);
    status->m_location = saved;
}

// ref: FUN_140667580
void XMLStatusRestoreLocation(CScriptStatus* status, const SCRIPTLOCATION* location) {
    status->m_location = *location;
}

// ---- the registries ---------------------------------------------------------------------

// ref: FUN_1406635a0
void FrameXML_FreeVirtuals() {
    s_virtualNodes.Clear(false);
}

// ref: FUN_140663520
void FrameXML_FreeIntrinsics() {
    s_intrinsicNodes.Clear(false);
}

// ref: FUN_140662bc0
void FrameXML_FreeFactories() {
    s_factories.Clear(false);
    XMLTree_Shutdown();
}

// ref: FUN_1406638d0
bool FrameXML_HasVirtual(const char* name) {
    return s_virtualNodes.Find(name) != nullptr;
}

// The compiled virtual of a name, marked in use; *busy says whether it already was.
// ref: FUN_140662b60
XMLOBJECT* FrameXML_AcquireVirtual(const char* name, const char** taint, bool* busy) {
    HashedNode* node = s_virtualNodes.Find(name);
    if (node) {
        *busy = node->m_busy;
        *taint = node->m_taint;
        node->m_refs++;
        node->m_busy = true;
        return node->m_object;
    }
    return nullptr;
}

// ref: FUN_140665130
void FrameXML_ReleaseVirtual(const char* name) {
    HashedNode* node = s_virtualNodes.Find(name);
    node->m_refs--;
    node->m_busy = false;
}

// The parent a frame names, directly or through the virtuals it inherits from (depth first).
// ref: FUN_1406628f0
const char* FrameXML_GetInheritedParent(XMLOBJECT* object) {
    auto frame = static_cast<XMLFRAME*>(object);
    if (frame->m_frameFlags & 0x40000) {
        return frame->m_parent.m_text;
    }
    if ((object->m_attributes & 0x100) == 0) {
        return nullptr;
    }
    const char* cursor = object->m_inherits.m_text;
    char token[0x400];
    SStrTokenize(&cursor, token, sizeof(token), " ,", nullptr);
    while (token[0] != '\0') {
        XMLOBJECT* found = nullptr;
        char name[0x80] = {};
        bool busy = true;
        {
            char* dest = name;
            const char* src = token;
            uint64_t left = 0x7f;
            while (true) {
                char c = *src++;
                *dest++ = c;
                if (c == '\0') {
                    break;
                }
                if (--left == 0) {
                    *dest = '\0';
                    break;
                }
            }
        }
        HashedNode* node = s_virtualNodes.Find(name);
        if (node) {
            busy = node->m_busy;
            node->m_refs++;
            node->m_busy = true;
            if (node->m_object) {
                found = node->m_object;
            }
        }
        if (found && (found->m_flags & 0x100)) {
            if (const char* parent = FrameXML_GetInheritedParent(found)) {
                if (!busy) {
                    HashedNode* release = s_virtualNodes.Find(name);
                    release->m_refs--;
                    release->m_busy = false;
                }
                return parent;
            }
        }
        SStrTokenize(&cursor, token, sizeof(token), " ,", nullptr);
        if (found && !busy) {
            HashedNode* release = s_virtualNodes.Find(name);
            release->m_refs--;
            release->m_busy = false;
        }
    }
    return nullptr;
}

// Compiles an element into a registry (virtual or intrinsic) under its name. A redefinition
// replaces an entry only when the old one carried a taint and the new one does not.
// ref: FUN_140666700
bool FrameXML_AddDeferredNode(bool intrinsic, XMLOBJECT* object, const char* name, const char* taint, CScriptStatus* status) {
    HashedNodeTable* table = intrinsic ? &s_intrinsicNodes : &s_virtualNodes;
    HashedNode* node = table->Find(name);
    if (node == nullptr) {
        uint32_t hash = SStrHashHT(name);
        node = table->New(hash, 0, 0);
        node->m_hashval = hash;
        if (node->m_str != name) {
            SMemFree(node->m_str, nullptr, 0, 0);
            node->m_str = SStrDupA(name, nullptr, 0);
        }
    } else {
        if (node->m_taint == nullptr || taint != nullptr) {
            status->Add(1, "Deferred XML Node object named %s already exists", name);
            return false;
        }
        XMLObjectFree(node->m_object, true);
    }
    uint64_t size;
    XMLOBJECT* compiled = XMLObjectCompile(object, &size);
    node->m_object = compiled;
    compiled->m_flags |= 0x20;
    node->m_taint = taint;
    if ((object->m_attributes & 0x1000) && taint == nullptr) {
        lua_State* L = FrameScript_GetContext();
        if (L != nullptr && object->m_secureMixin.m_text != nullptr) {
            FrameScript_CreateSecureMixins(L, object->m_secureMixin.m_text, name, status);
        }
    }
    if (s_debugLevel > 0) {
        status->Add(0, "-- Added Deferred XML Node %s", name);
    }
    return true;
}

// ---- loading ----------------------------------------------------------------------------

// Where an included file is: beside the including file if it exists there, else under the
// TOC's directory.
// ref: FUN_140662820
void FrameXML_BuildPath(char* dest, uint64_t size, const char* path, const char* dir, const char* file, bool localFiles) {
    char base[0x30c];
    OsPathGetDirectory(base, sizeof(base), path);
    if (base[0] == '\0') {
        uint64_t length = strlen(file);
        if (size != 0) {
            if (size - 1 < length) {
                length = size - 1;
            }
            memcpy(dest, file, length);
            dest[length] = '\0';
        }
        return;
    }
    OsPathJoin(dest, size, base, file);
    if (!cas::CASFileExistsByName(dest, localFiles)) {
        OsPathJoin(dest, size, dir, file);
    }
}

// The files a TOC lists: its non-empty lines that are not comments.
// ref: FUN_140663840
int32_t FrameXML_CountTocFiles(const char* text) {
    int32_t count = 0;
    char c = *text;
    while (c != '\0') {
        while (c == ' ' || static_cast<uint8_t>(c - 9) <= 4) {
            c = *++text;
            if (c == '\0') {
                break;
            }
        }
        c = *text;
        const char* end = text;
        const char* next = text;
        if (c != '\0') {
            do {
                next++;
                const char* last = end;
                if (c != '\r') {
                    last = next;
                    if (c == '\n') {
                        last = end;
                        break;
                    }
                }
                c = *next;
                end = last;
            } while (c != '\0');
            while (text < end) {
                c = *end;
                if (c != '\0' && c != ' ' && static_cast<uint8_t>(c - 9) > 4) {
                    if ((end - text) != -1 && *text != '#') {
                        count++;
                    }
                    break;
                }
                end--;
            }
        }
        c = *next;
        text = next;
    }
    return count;
}

// Reports nodes and attributes no parser consumed (validateFrameXML).
// ref: FUN_140663440
void FrameXML_Validate(XMLNode* node, CScriptStatus* status) {
    for (; node != nullptr; node = node->m_next) {
        if ((node->m_flags & 1) == 0) {
            XMLStatusRestoreLocation(status, node->GetLocation());
            status->Add(1, "Unrecognized XML: %s", node->m_name.m_data);
        }
        for (uint32_t i = 0; i < node->m_attributeCount; i++) {
            if (!node->IsAttributeUsed(i)) {
                XMLStatusRestoreLocation(status, node->GetLocation());
                status->Add(1, "Unrecognized XML attribute: %s", node->GetAttributeName(i));
            }
        }
        FrameXML_Validate(node->m_child, status);
    }
}

// Runs a document's top level: includes, scripts, fonts, scoped modifiers, and its virtual,
// intrinsic and concrete frames.
// ref: FUN_140663e50
void FrameXML_Walk(const char* path, const char* dir, const char* arg, CScriptStatus* status, XMLOBJECT* ui, bool localFiles, uint32_t flags) {
    LOCATIONGUARD outer(status);
    XMLStatusSetLocation(status, ui);
    XMLOBJECT** link = &ui->m_child;
    for (XMLOBJECT* node = *link; node != nullptr; node = *link) {
        bool registered = false;
        LOCATIONGUARD inner(status);
        XMLStatusSetLocation(status, node);
        uint32_t state = s_scopedState;
        node->m_flags = (node->m_flags & ~0x1fu) | (state & 0x1f);
        if (state & 0x1f) {
            node->m_scriptState = FrameScript_GetCurrentObject();
            FrameScript_SetCurrentObjectScoped(true);
            state = s_scopedState;
        }
        const char* taint = lua_taint;
        XMLOBJECT** next = &node->m_next;
        uint32_t type = node->m_type;
        if (type == XMLTYPE_INCLUDE) {
            auto include = static_cast<XMLFILE*>(node);
            if ((include->m_fileFlags & 1) == 0) {
                status->Add(2, "Element 'Include' without file attribute");
            } else {
                char file[0x104];
                FrameXML_BuildPath(file, sizeof(file), path, dir, include->m_file.m_text, localFiles);
                FrameXML_LoadFile(file, dir, arg, status, localFiles, flags);
            }
        } else if (type == XMLTYPE_SCRIPT) {
            auto script = static_cast<XMLSCRIPT*>(node);
            if (script->m_scriptFlags & 1) {
                char file[0x104];
                FrameXML_BuildPath(file, sizeof(file), path, dir, script->m_file.m_text, localFiles);
                FrameScript_ExecuteFile(file, arg, status, flags, t_localFiles);
            }
            if (script->m_bodyFlags & 1) {
                char name[0x10f];
                SStrPrintf(name, sizeof(name), "%s:<Scripts>", path);
                FrameScript_Execute(script->m_body.m_text, name, lua_taint);
            }
        } else if (type == XMLTYPE_FONT) {
            if ((node->m_attributes & 0x40) == 0) {
                status->Add(1, "Unnamed font node at top level");
            } else {
                CSimpleFont* font = CSimpleFont::GetFont(node->m_name.m_text, true);
                font->LoadXML(node, status);
            }
        } else if (type == XMLTYPE_SCOPEDMODIFIER) {
            uint32_t modifier = static_cast<XMLSCOPEDMODIFIER*>(node)->m_modifierFlags;
            uint32_t scoped = state;
            if (modifier & 1) {
                scoped = state & ~1u;
                if (modifier & 2) {
                    scoped = state | 1;
                }
            }
            if (modifier & 4) {
                if ((modifier & 8) == 0) {
                    scoped &= ~4u;
                } else {
                    scoped |= 4;
                }
            }
            if (modifier & 0x40) {
                if (modifier & 0x80) {
                    scoped |= 0x10;
                } else {
                    scoped &= ~0x10u;
                }
            }
            if (modifier & 0x10) {
                if ((modifier & 0x20) == 0) {
                    scoped &= ~8u;
                } else {
                    scoped |= 8;
                }
            }
            s_scopedState = ~s_scopedMask & scoped;
            // the reference passes its arg and directory swapped here
            FrameXML_Walk(path, arg, dir, status, node, localFiles, flags);
            s_scopedState = ~s_scopedMask & state;
        } else if (type == XMLTYPE_FONTFAMILY) {
            if ((node->m_attributes & 0x40) == 0) {
                status->Add(1, "Unnamed font family node at top level");
            } else {
                // Each <Member alphabet="..."> builds the family's font for one of five
                // alphabets (roman, korean, simplifiedchinese, traditionalchinese, russian);
                // a missing one is reported, and the family takes the client's alphabet.
                // Needs CSimpleFont's layout (its five member slots, 0x98..0xb8).
                CSimpleFont* family = CSimpleFont::GetFont(node->m_name.m_text, true);
                (void)family;
                FROST_UNIMPLEMENTED();
            }
        } else {
            uint32_t attributes = node->m_attributes;
            if ((attributes & 0x420) == 0x420) {
                if (attributes & 0x100) {
                    status->Add(1, "Intrinsic node %s cannot inherit from other nodes");
                    node->m_attributes &= ~0x100u;
                    attributes = node->m_attributes;
                }
                if ((attributes & 0x40) == 0) {
                    status->Add(1, "Unnamed intrinsic node");
                } else {
                    const char* name = node->m_name.m_text;
                    if (s_virtualNodes.Find(name) == nullptr) {
                        registered = FrameXML_AddDeferredNode(true, node, name, taint, status);
                    } else {
                        status->Add(1, "Intrinsic object named %s already exists as an virtual");
                    }
                }
            } else if ((attributes & 0x210) == 0x210) {
                if ((attributes & 0x40) == 0) {
                    status->Add(1, "Unnamed virtual node at top level");
                } else {
                    const char* name = node->m_name.m_text;
                    if (s_intrinsicNodes.Find(name) == nullptr) {
                        registered = FrameXML_AddDeferredNode(false, node, name, taint, status);
                    } else {
                        status->Add(1, "Virtual object named %s already exists as an intrinsic");
                    }
                }
            } else if (node->m_flags & 0x100) {
                FrameXML_CreateFrame(node, nullptr, status);
                CLayoutFrame::ResizePending();
            }
            if (registered) {
                // the registry holds its own compiled copy; drop the node from the document
                *link = node->m_next;
                next = link;
            }
        }
        inner.Restore();
        link = next;
    }
}

// ref: FUN_140663bb0
bool FrameXML_LoadXML(const char* path, const char* dir, const char* arg, CScriptStatus* status, bool localFiles, uint32_t flags) {
    XMLBUILDER builder = {};
    LOCATIONGUARD guard(status);
    bool result = false;
    uint8_t* data;
    uint32_t size;
    if (!cas::CASFileLoadByName(path, &data, &size, t_localFiles, 0x20)) {
        status->Add(2, "Couldn't open %s", path);
        guard.Restore();
    } else {
        XMLTREE* tree = XMLTree_Load(reinterpret_cast<const char*>(data), size, status, path);
        cas::CASFileFree(data);
        if (tree == nullptr) {
            guard.Restore();
        } else {
            XMLNode* root = XMLTree_GetRoot(tree);
            XMLOBJECT* ui = XMLObjectCreateDocument(root, &builder);
            if (s_validateFrameXML != nullptr && s_validateFrameXML->m_intValue != 0) {
                FrameXML_Validate(root, status);
            }
            XMLTree_Free(tree);
            guard.Restore();
            if (ui != nullptr) {
                FrameXML_Walk(path, dir, arg, status, ui, localFiles, flags);
                XMLObjectFree(ui, false);
                result = true;
            }
        }
    }
    // TSFixedArray<void*, 32> teardown
    for (uint32_t i = 0; i < builder.m_blockCount; i++) {
        SMemFree(builder.m_blocks[i], "Engine/Source/FrameXML/BinaryXML.h", 0x83, 0);
    }
    if (builder.m_blocks != nullptr) {
        SMemFree(builder.m_blocks, ".?AV?$TSFixedArray@PEAX$0CA@@@", -2, 0);
    }
    return result;
}

// One file a TOC (or an Include) names: .lua files run, anything else loads as XML.
// ref: FUN_140663900
bool FrameXML_LoadFile(const char* path, const char* dir, const char* arg, CScriptStatus* status, bool localFiles, uint32_t flags) {
    char buffer[0x30c];
    OsPathNormalize(buffer, sizeof(buffer), path);
    if (strstr(path, "..") != nullptr) {
        // "dir/../" pairs are collapsed out of the path
        char* dest = buffer;
        uint64_t left = 0x30b;
        while (true) {
            char c = *path++;
            *dest++ = c;
            if (c == '\0') {
                break;
            }
            if (--left == 0) {
                *dest = '\0';
                break;
            }
        }
        char* up = strstr(buffer, "..");
        while (up != nullptr && (up[-1] == '\\' || up[-1] == '/') && (up[2] == '\\' || up[2] == '/')) {
            char* start = up - 2;
            while (buffer <= start && *start != '\\' && *start != '/') {
                start--;
            }
            const char* from = up + 3;
            char* to = start + 1;
            uint64_t room = buffer + sizeof(buffer) - to;
            if (room != 0) {
                while (true) {
                    char c = *from++;
                    *to = c;
                    if (c == '\0') {
                        break;
                    }
                    to++;
                    if (--room == 0) {
                        *to = '\0';
                        break;
                    }
                }
            }
            up = strstr(buffer, "..");
        }
        path = buffer;
    }
    const char* extension = OsPathExtension(path);
    if (extension != nullptr) {
        const char* lua = ".lua";
        while (true) {
            int32_t a = *extension++;
            int32_t b = *lua++;
            if (static_cast<uint32_t>(a - 'A') < 26) {
                a += 0x20;
            }
            if (static_cast<uint32_t>(b - 'A') < 26) {
                b += 0x20;
            }
            if (a != b) {
                break;
            }
            if (a == 0) {
                return FrameScript_ExecuteFile(path, arg, status, flags, t_localFiles);
            }
        }
    }
    return FrameXML_LoadXML(path, dir, arg, status, localFiles, flags);
}

// Loads a .toc: every listed file, in order, relative to the TOC's directory.
// ref: FUN_1406630a0
bool FrameXML_LoadToc(const char* toc, const char* arg, CScriptStatus* status, bool localFiles, uint32_t flags) {
    bool wasLoading = s_loadingToc;
    s_loadingToc = true;
    char dir[0x30c];
    OsPathGetDirectory(dir, sizeof(dir), toc);
    uint8_t* data;
    uint32_t size;
    if (!cas::CASFileLoadByName(toc, &data, &size, uint64_t(localFiles) | 2, 0x20)) {
        status->Add(2, "Couldn't open %s", toc);
        return false;
    }
    int32_t line = 1;
    const char* text = reinterpret_cast<const char*>(data);
    if (static_cast<uint8_t>(text[0]) == 0xEF && static_cast<uint8_t>(text[1]) == 0xBB && static_cast<uint8_t>(text[2]) == 0xBF) {
        text += 3;
    }
    bool savedLocalFiles = t_localFiles;
    t_localFiles = localFiles;
    LOCATIONGUARD guard(status);
    char c = *text;
    while (c != '\0') {
        // skip blank space, counting newlines
        do {
            if (c != ' ' && c != '\t') {
                if (c == '\n') {
                    line++;
                } else if (static_cast<uint8_t>(c - 0xb) > 2) {
                    break;
                }
            }
            c = *++text;
        } while (c != '\0');
        c = *text;
        const char* next = text;
        const char* end = text;
        if (c != '\0') {
            const char* last;
            do {
                next++;
                last = end;
                if (c != '\r') {
                    last = next;
                    if (c == '\n') {
                        line++;
                        last = end;
                        break;
                    }
                }
                c = *next;
                end = last;
            } while (c != '\0');
            // trim the line's trailing space; a one-character line is never loaded
            while (text < last) {
                c = *last;
                if (c != '\0' && c != ' ' && static_cast<uint8_t>(c - 9) > 4) {
                    uint64_t length = static_cast<uint64_t>(last + 1 - text);
                    if (length != 0 && *text != '#') {
                        status->m_location.m_line = static_cast<uint16_t>(line - 1 < 1 ? 0 : line - 1);
                        blz::inline_string<0x30c> entry;
                        blz::string_assign(entry.header(), text, length);
                        char file[0x30c];
                        OsPathJoin(file, sizeof(file), dir, entry.m_data);
                        FrameXML_LoadFile(file, dir, arg, status, localFiles, flags);
                        if (s_progress != nullptr && s_progressCount < s_progressTotal) {
                            s_progressCount++;
                            s_progress(static_cast<float>(s_progressCount) / static_cast<float>(s_progressTotal), s_progressContext);
                        }
                    }
                    break;
                }
                last--;
            }
        }
        c = *next;
        text = next;
    }
    XMLTree_Shutdown();
    cas::CASFileFree(data);
    t_localFiles = savedLocalFiles;
    s_loadingToc = wasLoading;
    guard.Restore();
    return true;
}

// ---- frames -----------------------------------------------------------------------------

// Creates the frame a definition describes, under `parent` (or the parent it names). An
// element named after an intrinsic is made by the factory of the type the intrinsic is, and
// loads the intrinsic first.
// ref: FUN_140662c30
CSimpleFrame* FrameXML_CreateFrame(XMLOBJECT* object, CSimpleFrame* parent, CScriptStatus* status) {
    if (s_debugLevel > 0) {
        if (!(object->m_attributes & 0x40)) {
            status->Add(0, "-- Creating unnamed %s", object->m_tag.m_text);
        } else {
            status->Add(0, "-- Creating %s named %s", object->m_tag.m_text, object->m_name.m_text);
        }
    }
    const char* intrinsic = nullptr;
    FrameFactoryNode* factory = s_factories.Find(object->m_tag.m_text);
    if (factory == nullptr) {
        char name[0x80];
        CopyBounded(name, object->m_tag.m_text, sizeof(name));
        const char* taint = nullptr;
        bool recursive = false;
        XMLOBJECT* found = FrameXML_AcquireIntrinsic(name, &taint, &recursive);
        if (found != nullptr) {
            if (!recursive) {
                factory = s_factories.Find(found->m_tag.m_text);
                intrinsic = object->m_tag.m_text;
                if (factory != nullptr) {
                    FrameXML_ReleaseIntrinsic(name);
                }
            } else {
                status->Add(1, "Multiply inherited intrinsic node: %s", object->m_tag.m_text);
            }
        }
        if (factory == nullptr) {
            status->Add(1, "Unknown frame type: %s", object->m_tag.m_text);
            if (found != nullptr && !recursive) {
                FrameXML_ReleaseIntrinsic(name);
            }
            return nullptr;
        }
    }
    if (!(object->m_flags & 0x100)) {
        return nullptr;
    }
    const char* parentName = FrameXML_GetInheritedParent(object);
    if (parentName != nullptr && *parentName != '\0') {
        bool lockdown = (s_scopedState >> 3) & 1;
        lua_State* L = FrameScript_GetContext();
        lua_pushstring(L, parentName);
        lua_rawget(L, LUA_GLOBALSINDEX);
        parent = static_cast<CSimpleFrame*>(
            FrameScript_Object::GetObjectFromLua(nullptr, L, CSimpleFrame::GetObjectType(), -1, false, lockdown));
        lua_settop(L, -2);
        if (parent == nullptr) {
            status->Add(1, "Couldn't find frame parent: %s", parentName);
        }
    }
    CSimpleFrame* frame = factory->m_factory(parent);
    if (frame == nullptr) {
        status->Add(1, "Unable to create frame type: %s", object->m_tag.m_text);
        return nullptr;
    }
    if (factory->m_unknown) {
        // a one-shot factory goes once it has made its frame
        s_factories.Unlink(factory);
        s_factories.Delete(factory);
    }
    frame->LoadXMLName(object, status);
    frame->FrameScript_Object::LoadXML(object, status, intrinsic);
    frame->LoadXML(object, status, intrinsic);
    frame->PostLoadXML(object, status);
    return frame;
}

// ---- regions, animations and references --------------------------------------------------------
// Their classes (CSimpleFontString, CSimpleTexture, CSimpleMaskTexture, CSimpleLine,
// CSimpleAnimGroup) are not ported yet.

// ref: FUN_1406685c0
CSimpleRegion* FrameXML_CreateFontString(XMLOBJECT* object, CSimpleFrame* frame, CScriptStatus* status) {
    (void)object;
    (void)frame;
    (void)status;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_140668680
CSimpleRegion* FrameXML_CreateTexture(XMLOBJECT* object, CSimpleFrame* frame, CScriptStatus* status) {
    auto texture = new CSimpleTexture(frame, 2, 0, true);
    if ((object->m_attributes & 0x40) != 0) {
        texture->SetName(object->m_name.m_text);
    }
    texture->FrameScript_Object::LoadXML(object, status, nullptr);
    texture->LoadXML(object, status, nullptr);
    texture->PostLoadXML(object, status);
    return texture;
}

// ref: FUN_140667f50
CSimpleRegion* FrameXML_CreateMaskTexture(XMLOBJECT* object, CSimpleFrame* frame, CScriptStatus* status) {
    (void)object;
    (void)frame;
    (void)status;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_140667e90
CSimpleRegion* FrameXML_CreateLine(XMLOBJECT* object, CSimpleFrame* frame, CScriptStatus* status) {
    (void)object;
    (void)frame;
    (void)status;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_140667a30
void FrameXML_CreateAnimGroup(XMLOBJECT* object, CScriptRegion* owner, CStatus* status) {
    (void)object;
    (void)owner;
    (void)status;
    FROST_UNIMPLEMENTED();
}

// The map of secure reference keys (0x142cf58d8) hashes blz strings with a hash not ported
// yet (FUN_142363ca0).
// ref: FUN_140667010
void FrameXML_SetSecureReferenceKey(const char* key, FrameScript_Object* object) {
    (void)key;
    (void)object;
    FROST_UNIMPLEMENTED();
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

// Appends the object to the array `name` of its parent's table, creating the array.
// ref: FUN_1406683d0
void FrameXML_AddToParentArray(CScriptObject* object, const char* name, CStatus* status) {
    lua_State* L = FrameScript_GetContext();
    CScriptObject* parent = object->GetParent();
    if (parent == nullptr) {
        return;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, parent->GetLuaRef());
    lua_pushstring(L, name);
    lua_rawget(L, -2);
    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_settop(L, -2);
        lua_pushstring(L, name);
        lua_createtable(L, 0, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, name);
        lua_rawget(L, -2);
        if (lua_type(L, -1) != LUA_TTABLE) {
            const char* owner = object->GetName();
            if (owner == nullptr) {
                owner = object->GetObjectTypeName();
            }
            status->Add(1, "%s: Has bad parentArray: %s", owner, name);
        }
    }
    uint64_t length = lua_objlen(L, -1);
    lua_pushnumber(L, static_cast<double>(length + 1));
    lua_rawgeti(L, LUA_REGISTRYINDEX, object->GetLuaRef());
    lua_rawset(L, -3);
    lua_settop(L, -3);
}
