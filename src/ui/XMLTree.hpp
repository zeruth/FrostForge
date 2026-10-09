#ifndef UI_XML_TREE_HPP
#define UI_XML_TREE_HPP

// XMLTree (Engine/Source/FrameXMLParser/XMLTree.cpp): an XML document parsed by Expat into a
// tree of nodes - element name, attributes, body text and children - for FrameXML to read.

#include <cstdint>

#include "blz/String.hpp"
#include "framescript/FrameScript.hpp"

class CStatus;

// An attribute (0x68). The flag records that FrameXML read it, for validateFrameXML.
class CXMLAttribute {
    public:
    blz::inline_string<0x11> m_name;    // 0x00
    blz::inline_string<0x11> m_value;   // 0x30
    bool m_used;                        // 0x60
};
static_assert(sizeof(CXMLAttribute) == 0x68, "CXMLAttribute is 0x68 bytes");

class XMLNode {
    public:
    uint32_t m_flags;                   // 0x00
    XMLNode* m_parent;                  // 0x08
    XMLNode* m_child;                   // 0x10 first child
    blz::inline_string<0x11> m_name;    // 0x18
    char* m_body;                       // 0x48 the text, joined across the element's runs
    int32_t m_bodyLength;               // 0x50
    uint32_t m_pad54;
    // TSGrowableArray<CXMLAttribute, 8>
    uint32_t m_attributeCount;          // 0x58
    CXMLAttribute* m_attributes;        // 0x60
    uint32_t m_attributeCapacity;       // 0x68
    uint32_t m_pad6C;
    uint32_t m_attributeGranularity;    // 0x70
    uint32_t m_pad74;
    int32_t m_bodyOffset;               // 0x78 where the element starts in its parent's text
    SCRIPTLOCATION m_location;          // 0x7C
    XMLNode* m_next;                    // 0x88 next sibling (or next free node)

    void Init(XMLNode* parent, const char* name);
    const char* GetAttribute(const char* name) const;
    // As GetAttribute, and marks the attribute read.
    const char* GetAttributeMark(const char* name);
    const char* GetAttributeName(uint32_t index) const;
    bool IsAttributeUsed(uint32_t index) const;
    void MarkAttributesUsed();
    void SetAttribute(const char* name, const char* value);
    XMLNode* GetChild(const char* name) const;
    SCRIPTLOCATION* GetLocation();
    // Frees what the node holds (a node on the stack, not one of the pool).
    void Destroy();
};
static_assert(sizeof(XMLNode) == 0x90, "XMLNode is 0x90 bytes");

// The parse state Expat's handlers work on (0x20).
struct XMLTREE {
    XMLNode* m_root;
    XMLNode* m_current;
    CStatus* m_status;
    void* m_parser;
};

// Parses `size` bytes; null (and an error in `status`) for a malformed document.
XMLTREE* XMLTree_Load(const char* data, uint32_t size, CStatus* status, const char* file);
XMLNode* XMLTree_GetRoot(XMLTREE* tree);
void XMLTree_Free(XMLTREE* tree);
// Frees the parser and every recycled node.
void XMLTree_Shutdown();

#endif
