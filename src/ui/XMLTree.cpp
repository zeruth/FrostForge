#include "ui/XMLTree.hpp"

#include <cstring>
#include <intrin.h>
#include <new>

#include "base/DataAllocator.hpp"
#include "base/Status.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"

#include "expat.h"

namespace {

const char s_file[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\FrameXMLParser\\XMLTree.cpp";
const char s_stringTag[] = ".D";
const char s_nodeTag[] = ".?AVXMLNode@@";
const char s_growableTag[] = ".?AV?$TSGrowableArray@VCXMLAttribute@@$07@@";
const char s_fixedTag[] = ".?AV?$TSFixedArray@VCXMLAttribute@@$07@@";

CDataAllocator s_nodes(sizeof(XMLNode), 0x1C7);   // 0x142e9a240
XMLNode* s_freeNodes;                             // 0x142e9a268
XML_Parser s_parser;                              // 0x142e9a270

void SetString(blz::inline_string<0x11>* text, const char* value) {
    blz::string_assign(text->header(), value, strlen(value));
}

// Constructs an empty attribute in raw storage.
void ConstructAttribute(CXMLAttribute* attribute) {
    new (&attribute->m_name) blz::inline_string<0x11>();
    new (&attribute->m_value) blz::inline_string<0x11>();
    attribute->m_used = false;
}

void DestroyAttribute(CXMLAttribute* attribute) {
    blz::string_free(attribute->m_value.header());
    blz::string_free(attribute->m_name.header());
}

// TSGrowableArray<CXMLAttribute, 8>: room for `extra` more, rounded up to the granularity
// (8, or the largest power of two in a smaller count). Attributes move to the new array.
// ref: FUN_140660eb0
void AttributesReserve(XMLNode* node, uint32_t extra, const char* tag) {
    uint32_t count = node->m_attributeCount + extra;
    if (count <= node->m_attributeCapacity) {
        return;
    }
    uint32_t granularity = node->m_attributeGranularity;
    if (granularity == 0) {
        if (count < 8) {
            granularity = count;
            for (uint32_t low = (count - 1) & count; low != 0; low = (low - 1) & low) {
                granularity = low;
            }
            if (granularity == 0) {
                granularity = 1;
            }
        } else {
            node->m_attributeGranularity = 8;
            granularity = 8;
        }
    }
    if (count % granularity != 0) {
        count += granularity - count % granularity;
    }
    CXMLAttribute* old = node->m_attributes;
    for (uint32_t i = count; i < node->m_attributeCount; i++) {
        DestroyAttribute(&old[i]);
    }
    node->m_attributeCapacity = count;
    node->m_attributes = nullptr;
    if (count != 0) {
        node->m_attributes = static_cast<CXMLAttribute*>(SMemAlloc(uint64_t(count) * sizeof(CXMLAttribute), 8, tag, 0, -7, 0));
    }
    if (old) {
        uint32_t keep = node->m_attributeCount < count ? node->m_attributeCount : count;
        for (uint32_t i = 0; i < keep; i++) {
            // FUN_140660870: the strings move (a heap buffer is handed over)
            CXMLAttribute* to = &node->m_attributes[i];
            ConstructAttribute(to);
            blz::string_assign(to->m_name.header(), old[i].m_name.m_data, old[i].m_name.m_size);
            blz::string_assign(to->m_value.header(), old[i].m_value.m_data, old[i].m_value.m_size);
            to->m_used = old[i].m_used;
            DestroyAttribute(&old[i]);
        }
        SMemFree(old, s_growableTag, -2, 0);
    }
}

// Sets the attribute count, constructing or destroying the difference.
// ref: FUN_140661170
void AttributesSetCount(XMLNode* node, uint32_t count) {
    uint32_t current = node->m_attributeCount;
    if (current < count) {
        AttributesReserve(node, count - current, static_cast<const char*>(_ReturnAddress()));
        for (uint32_t i = node->m_attributeCount; i < count; i++) {
            ConstructAttribute(&node->m_attributes[i]);
        }
        node->m_attributeCount = count;
        return;
    }
    for (uint32_t i = count; i < node->m_attributeCount; i++) {
        DestroyAttribute(&node->m_attributes[i]);
    }
    node->m_attributeCount = count;
}

// TSFixedArray<CXMLAttribute, 8>'s teardown.
// ref: FUN_140660960
void AttributesFree(XMLNode* node) {
    for (uint32_t i = 0; i < node->m_attributeCount; i++) {
        DestroyAttribute(&node->m_attributes[i]);
    }
    if (node->m_attributes) {
        SMemFree(node->m_attributes, s_fixedTag, -2, 0);
    }
}

// Returns a subtree's nodes to the free list (their names are kept for reuse).
// ref: FUN_140660e00
void ReleaseNode(XMLNode* node) {
    XMLNode* next = node->m_next;
    while (next) {
        XMLNode* after = next->m_next;
        next->m_next = nullptr;
        ReleaseNode(next);
        next = after;
    }
    node->m_next = nullptr;
    if (node->m_child) {
        ReleaseNode(node->m_child);
        node->m_child = nullptr;
    }
    if (node->m_body) {
        SMemFree(node->m_body, s_file, 0x164, 0);
        node->m_body = nullptr;
    }
    AttributesFree(node);
    node->m_attributeCapacity = 0;
    node->m_attributeCount = 0;
    node->m_attributes = nullptr;
    node->m_next = s_freeNodes;
    s_freeNodes = node;
}

// XMLNode's destructor, children and siblings first.
// ref: FUN_140660b20
void DestroyNode(XMLNode* node) {
    if (node->m_next) {
        DestroyNode(node->m_next);
    }
    if (node->m_child) {
        DestroyNode(node->m_child);
    }
    SMemFree(node->m_body, s_file, 0xF5, 0);
    AttributesFree(node);
    blz::string_free(node->m_name.header());
    s_nodes.PutData(node);
}

// Expat's allocator.
// ref: FUN_140661280
void* XmlMalloc(size_t size) {
    return SMemAlloc(size, 1, s_file, SStrHashFnv1a(s_file), 0x72, 0);
}

// ref: FUN_140661300
void* XmlRealloc(void* ptr, size_t size) {
    return SMemReAlloc(ptr, size, 1, s_file, SStrHashFnv1a(s_file), 0x77, 0);
}

// ref: FUN_1406612e0
void XmlFree(void* ptr) {
    SMemFree(ptr, s_file, 0x7C, 0);
}

// An element opens: a node under the current one, with its position and attributes.
// ref: FUN_140661670
void StartElement(void* userData, const XML_Char* name, const XML_Char** attributes) {
    auto tree = static_cast<XMLTREE*>(userData);
    XMLNode* node = s_freeNodes;
    if (node == nullptr) {
        node = static_cast<XMLNode*>(s_nodes.GetData(false, s_nodeTag, -2, 1));
        if (node != nullptr) {
            new (&node->m_name) blz::inline_string<0x11>();
            node->m_attributeCount = 0;
            node->m_attributes = nullptr;
            node->m_attributeCapacity = 0;
            node->m_attributeGranularity = 0;
            node->m_location = {};
            node->Init(tree->m_current, name);
        }
    } else {
        s_freeNodes = node->m_next;
        node->Init(tree->m_current, name);
    }
    if (tree->m_status && tree->m_parser) {
        int line = XML_GetCurrentLineNumber(static_cast<XML_Parser>(tree->m_parser));
        auto location = static_cast<SCRIPTLOCATION*>(tree->m_status->GetLocation());
        node->m_location.m_file = location->m_file;
        node->m_location.m_line = static_cast<uint16_t>(line < 1 ? 0 : line);
    }
    XMLNode* parent = tree->m_current;
    if (parent == nullptr) {
        tree->m_root = node;
    } else if (parent->m_child == nullptr) {
        parent->m_child = node;
    } else {
        XMLNode* last = parent->m_child;
        while (last->m_next) {
            last = last->m_next;
        }
        last->m_next = node;
    }
    tree->m_current = node;
    uint32_t count = 0;
    while (attributes[count * 2]) {
        count++;
    }
    AttributesSetCount(node, count);
    for (uint32_t i = 0; i < count; i++) {
        SetString(&node->m_attributes[i].m_name, attributes[i * 2]);
        SetString(&node->m_attributes[i].m_value, attributes[i * 2 + 1]);
    }
}

// ref: FUN_1406619c0
void EndElement(void* userData, const XML_Char* name) {
    (void)name;
    auto tree = static_cast<XMLTREE*>(userData);
    tree->m_current = tree->m_current->m_parent;
}

// Text: appended to the current element's body; leading whitespace-only runs are dropped.
// ref: FUN_1406619d0
void CharacterData(void* userData, const XML_Char* text, int length) {
    if (length == 0) {
        return;
    }
    XMLNode* node = static_cast<XMLTREE*>(userData)->m_current;
    if (node->m_body == nullptr) {
        if (length < 1) {
            return;
        }
        int i = 0;
        while (static_cast<uint8_t>(text[i]) < 0x21 && ((0x100002600ull >> (text[i] & 0x3F)) & 1) != 0) {
            if (++i >= length) {
                return;
            }
        }
    }
    node->m_body = static_cast<char*>(SMemReAlloc(node->m_body, uint64_t(node->m_bodyLength + 1 + length), 1, s_file,
        SStrHashFnv1a(s_file), 0x6A, 0));
    memcpy(node->m_body + node->m_bodyLength, text, length);
    node->m_bodyLength += length;
    node->m_body[node->m_bodyLength] = '\0';
}

}  // namespace

// ref: FUN_140660d50
void XMLNode::Init(XMLNode* parent, const char* name) {
    this->m_parent = parent;
    this->m_child = nullptr;
    blz::string_assign(this->m_name.header(), name, strlen(name));
    this->m_body = nullptr;
    this->m_bodyLength = 0;
    this->m_bodyOffset = (parent && parent->m_body) ? parent->m_bodyLength : 0;
    this->m_flags &= ~1u;
    this->m_next = nullptr;
}

// ref: FUN_140660bf0
const char* XMLNode::GetAttribute(const char* name) const {
    for (uint32_t i = 0; i < this->m_attributeCount; i++) {
        if (_stricmp(this->m_attributes[i].m_name.m_data, name) == 0) {
            return this->m_attributes[i].m_value.m_data;
        }
    }
    return nullptr;
}

// ref: FUN_140660c60
const char* XMLNode::GetAttributeMark(const char* name) {
    for (uint32_t i = 0; i < this->m_attributeCount; i++) {
        if (_stricmp(this->m_attributes[i].m_name.m_data, name) == 0) {
            this->m_attributes[i].m_used = true;
            return this->m_attributes[i].m_value.m_data;
        }
    }
    return nullptr;
}

// ref: FUN_140660cd0
const char* XMLNode::GetAttributeName(uint32_t index) const {
    return this->m_attributes[index].m_name.m_data;
}

// ref: FUN_140660db0
bool XMLNode::IsAttributeUsed(uint32_t index) const {
    return index < this->m_attributeCount && this->m_attributes[index].m_used;
}

// ref: FUN_140660dd0
void XMLNode::MarkAttributesUsed() {
    for (uint32_t i = 0; i < this->m_attributeCount; i++) {
        this->m_attributes[i].m_used = true;
    }
}

// ref: FUN_1406610b0
void XMLNode::SetAttribute(const char* name, const char* value) {
    for (uint32_t i = 0; i < this->m_attributeCount; i++) {
        if (_stricmp(name, this->m_attributes[i].m_name.m_data) == 0) {
            SetString(&this->m_attributes[i].m_value, value);
            return;
        }
    }
    uint32_t index = this->m_attributeCount;
    AttributesSetCount(this, index + 1);
    SetString(&this->m_attributes[index].m_name, name);
    SetString(&this->m_attributes[index].m_value, value);
}

// ref: FUN_140660ce0
XMLNode* XMLNode::GetChild(const char* name) const {
    for (XMLNode* child = this->m_child; child; child = child->m_next) {
        if (_stricmp(child->m_name.m_data, name) == 0) {
            return child;
        }
    }
    return nullptr;
}

// ref: FUN_140660d40
SCRIPTLOCATION* XMLNode::GetLocation() {
    return &this->m_location;
}

// ref: FUN_140661480
XMLTREE* XMLTree_Load(const char* data, uint32_t size, CStatus* status, const char* file) {
    const char* name = file ? file : "UnknownFile";
    if (status) {
        // FUN_1406606d0: the file's position, at line 1 (the name registry is never set)
        uint32_t id = 0;
        status->SetFile(&id);
    }
    auto tree = static_cast<XMLTREE*>(SMemAlloc(sizeof(XMLTREE), 1, s_file, SStrHashFnv1a(s_file), 0x8A, 0));
    tree->m_root = nullptr;
    tree->m_current = nullptr;
    tree->m_status = status;
    tree->m_parser = nullptr;
    if (s_parser == nullptr) {
        XML_Memory_Handling_Suite memory = { XmlMalloc, XmlRealloc, XmlFree };
        s_parser = XML_ParserCreate_MM(nullptr, &memory, nullptr);
    } else {
        XML_ParserReset(s_parser, nullptr);
    }
    XML_SetElementHandler(s_parser, StartElement, EndElement);
    XML_SetCharacterDataHandler(s_parser, CharacterData);
    XML_SetUserData(s_parser, tree);
    tree->m_parser = s_parser;
    if (XML_Parse(s_parser, data, static_cast<int>(size), 1) == 0) {
        const XML_LChar* error = XML_ErrorString(XML_GetErrorCode(s_parser));
        int line = XML_GetCurrentLineNumber(s_parser);
        if (status) {
            status->Add(2, "%s(%d): error: %s", name, line, error);
        }
        if (tree->m_root) {
            ReleaseNode(tree->m_root);
        }
        ::operator delete(tree, sizeof(XMLTREE));
        return nullptr;
    }
    return tree;
}

// ref: FUN_140661460
XMLNode* XMLTree_GetRoot(XMLTREE* tree) {
    return tree->m_root;
}

// ref: FUN_140661430
void XMLTree_Free(XMLTREE* tree) {
    if (tree->m_root) {
        ReleaseNode(tree->m_root);
    }
    ::operator delete(tree, sizeof(XMLTREE));
}

// ref: FUN_140661360
void XMLTree_Shutdown() {
    if (s_parser) {
        XML_ParserFree(s_parser);
        s_parser = nullptr;
    }
    while (XMLNode* node = s_freeNodes) {
        s_freeNodes = node->m_next;
        node->m_next = nullptr;
        if (node->m_child) {
            DestroyNode(node->m_child);
        }
        SMemFree(node->m_body, s_file, 0xF5, 0);
        AttributesFree(node);
        blz::string_free(node->m_name.header());
        s_nodes.PutData(node);
    }
}

// A node the caller owns lets its children, text, attributes and name go (FUN_140660a10).
// ref: FUN_140660a10
void XMLNode::Destroy() {
    if (this->m_next != nullptr) {
        DestroyNode(this->m_next);
    }
    if (this->m_child != nullptr) {
        DestroyNode(this->m_child);
    }
    SMemFree(this->m_body, s_file, 0xF5, 0);
    this->m_body = nullptr;
    AttributesFree(this);
    this->m_attributes = nullptr;
    this->m_attributeCount = 0;
    this->m_attributeCapacity = 0;
    this->m_next = nullptr;
    this->m_child = nullptr;
    blz::string_free(this->m_name.header());
    // (left an empty inline string: m_name's own destructor frees it again when a stack node
    // goes out of scope after Destroy, and CreateFrameFromInfo's did, freeing a name buffer the
    // frame it created had since been handed as a child node. Enter World then walked a freed
    // node in SetFrameLevel/UpdateScale, 2026-10-21)
    this->m_name.m_data = this->m_name.m_inline;
    this->m_name.m_size = 0;
    this->m_name.m_capacity = blz::string::INLINE | (sizeof(this->m_name.m_inline) - 1);
    this->m_name.m_inline[0] = '\0';
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_140660960 = (void*)&AttributesFree;
extern "C" void* const gen_p_142e9a240 = (void*)&s_nodes;
// ---- end of generated globals ----
