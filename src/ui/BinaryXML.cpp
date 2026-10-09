#include "ui/BinaryXML.hpp"

#include <cstdlib>
#include <cstring>
#include <intrin.h>

#include "base/Coordinate.hpp"
#include "base/Status.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "gxu/GxuFont.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "ui/FrameXML.hpp"
#include "ui/UITypes.hpp"
#include "ui/XMLTree.hpp"

// The reference allocates its blocks from BinaryXML.h's inline helpers.
#define BINARYXML_H_FILE "Engine/Source/FrameXML/BinaryXML.h"

namespace {

constexpr uint64_t XML_BLOCK_SIZE = 0x8000;
constexpr uint64_t XML_BLOCK_RESERVE = 0x4000;
constexpr float DEG_TO_RAD = 0.017453292f;     // 0x14263a04c
constexpr float UI_HEIGHT = 1024.0f;            // 0x14261a9c4

// The attribute a node carries, marked as read.
inline const char* Attr(XMLOBJECT* object, const char* name) {
    return object->m_node->GetAttributeMark(name);
}

// The flag pattern every boolean attribute is stored with: one bit that says the attribute
// was given, and one bit holding its value.
inline void SetBoolFlag(uint32_t* flags, uint32_t given, uint32_t valueShift, const char* value) {
    *flags |= given;
    bool b = StringToBOOL(value);
    *flags &= ~(1u << valueShift);
    *flags |= uint32_t(b) << valueShift;
}

// strtol's radix for an integer attribute: hex after "0x", else decimal.
inline int32_t IntRadix(const char* value) {
    return value[0] == '0' && (value[1] == 'x' || value[1] == 'X') ? 16 : 10;
}

inline int32_t SizeOf(XMLOBJECT* object, XMLBUILDER* builder) {
    return int32_t(builder->m_cursor - reinterpret_cast<char*>(object));
}

// A UI length scaled from the 1024-unit screen to normalized coordinates.
inline float ScaleUI(float value) {
    return (value / (g_coordinateSpace.m_aspectCompensation * UI_HEIGHT)) * g_coordinateSpace.m_diagonalWidth;
}

// The colour channel clamp the reference inlines (re-reading the attribute each time).
inline float ClampUnit(const char* value) {
    float f = SStrToFloat(value);
    if (1.0f <= f || 0.0f < SStrToFloat(value)) {
        f = 1.0f;
        if (SStrToFloat(value) < 1.0f) {
            f = SStrToFloat(value);
        }
        return f;
    }
    return 0.0f;
}

inline uint32_t PackChannel(float value) {
    return uint32_t(int64_t(value * 255.0f + 0.5f)) & 0xff;
}

// Copies a name into a 0x80 buffer (at most 0x7f characters) and lower-cases it.
inline void LowerName(char* dest, const char* name) {
    uint64_t left = 0x7f;
    while (true) {
        char c = *name++;
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

bool XMLParseHeader(XMLOBJECT* object, XMLBUILDER* builder);
bool XMLParseDimension(XMLDIMENSION* object, XMLBUILDER* builder);
bool XMLParseInset(XMLINSET* object, XMLBUILDER* builder);
bool XMLParseValue(XMLVALUE* object, XMLBUILDER* builder);
bool XMLParseColor(XMLCOLOR* object, XMLBUILDER* builder);
bool XMLParseAnchor(XMLANCHOR* object, XMLBUILDER* builder);
bool XMLParseAnimation(XMLANIMATION* object, XMLBUILDER* builder);
bool XMLParseFont(XMLFONT* object, XMLBUILDER* builder);
bool XMLParseRegion(XMLREGION* object, XMLBUILDER* builder);
bool XMLParseFrame(XMLFRAME* object, XMLBUILDER* builder);
bool XMLParseTexture(XMLTEXTURE* object, XMLBUILDER* builder);
bool XMLParseFontString(XMLFONTSTRING* object, XMLBUILDER* builder);
bool XMLParseButton(XMLBUTTON* object, XMLBUILDER* builder);
bool XMLParseModel(XMLMODEL* object, XMLBUILDER* builder);
bool XMLParseScriptHandler(XMLSCRIPTHANDLER* object, XMLBUILDER* builder);
bool XMLParseActor(XMLACTOR* object, XMLBUILDER* builder);
bool XMLParseAnimationGroup(XMLANIMATIONGROUP* object, XMLBUILDER* builder);
bool XMLParseKeyValues(XMLOBJECT* object, XMLBUILDER* builder);
bool XMLParseScripts(XMLOBJECT* object, XMLBUILDER* builder);
bool XMLParseOffsetBody(XMLDIMENSION* object, XMLBUILDER* builder);
bool XMLParseViewInsets(XMLINSET* object, XMLBUILDER* builder);
bool XMLParseMaskedTextures(XMLOBJECT* object, XMLBUILDER* builder);
bool XMLParseAnimations(XMLOBJECT* object, XMLBUILDER* builder);

bool XMLParseDimensionChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name);
bool XMLParseInsetChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name);
bool XMLParseFontStringChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name);
bool XMLParseScriptsChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name);

// ---- builder ---------------------------------------------------------------------------

}  // namespace

// TSGrowableArray<void*, 32>::Add
// ref: FUN_1406693d0
void XMLBuilderAddBlocks(XMLBUILDER* builder, uint32_t count, void* const* blocks) {
    uint32_t total = builder->m_blockCount + count;
    if (builder->m_blockCapacity < total) {
        uint32_t granularity = builder->m_blockGranularity;
        if (granularity == 0) {
            if (total < 0x20) {
                granularity = total;
                for (uint32_t low = (total - 1) & total; low != 0; low = (low - 1) & low) {
                    granularity = low;
                }
                if (granularity == 0) {
                    granularity = 1;
                }
            } else {
                builder->m_blockGranularity = 0x20;
                granularity = 0x20;
            }
        }
        if (total % granularity != 0) {
            total += granularity - total % granularity;
        }
        void** old = builder->m_blocks;
        builder->m_blockCapacity = total;
        builder->m_blocks = nullptr;
        if (total != 0) {
            // The reference tags this allocation with its own return address.
            builder->m_blocks = static_cast<void**>(SMemAlloc(uint64_t(total) * 8, 8,
                static_cast<const char*>(_ReturnAddress()), 0, -7, 0));
        }
        if (old) {
            uint32_t keep = builder->m_blockCount <= total ? builder->m_blockCount : total;
            for (uint32_t i = 0; i < keep; i++) {
                builder->m_blocks[i] = old[i];
            }
            SMemFree(old, ".?AV?$TSGrowableArray@PEAX$0CA@@@", -2, 0);
        }
    }
    void** dest = builder->m_blocks + builder->m_blockCount;
    for (uint32_t i = 0; i < count; i++) {
        dest[i] = blocks[i];
    }
    builder->m_blockCount += count;
}

// ref: FUN_14066e0d0
char* XMLBuilderAllocBlock(XMLBUILDER* builder, uint64_t bytes) {
    void* block = SMemAlloc(bytes, 0x20, BINARYXML_H_FILE, SStrHashFnv1a(BINARYXML_H_FILE), 0x89, 0);
    XMLBuilderAddBlocks(builder, 1, &block);
    return static_cast<char*>(block);
}

// ref: FUN_1406695d0
bool XMLBuilderReserve(XMLBUILDER* builder) {
    if (builder->m_block == nullptr || builder->m_end < builder->m_cursor + XML_BLOCK_RESERVE) {
        builder->m_block = XMLBuilderAllocBlock(builder, XML_BLOCK_SIZE);
        memset(builder->m_block, 0, XML_BLOCK_SIZE);
        builder->m_cursor = builder->m_block;
        builder->m_end = builder->m_block + XML_BLOCK_SIZE;
    }
    return true;
}

// ---- objects ---------------------------------------------------------------------------

// ref: FUN_140669520
bool XMLObjectAddString(XMLOBJECT* object, XMLBUILDER* builder, XMLSTRING* slot, const char* text) {
    uint64_t bytes = strlen(text) + 1;
    char* dest = builder->m_strings;
    if (dest == nullptr
        || uint32_t(int32_t(reinterpret_cast<intptr_t>(builder->m_stringsEnd)) - int32_t(reinterpret_cast<intptr_t>(dest))) < bytes) {
        dest = XMLBuilderAllocBlock(builder, XML_BLOCK_SIZE);
        builder->m_strings = dest;
        builder->m_stringsEnd = dest + XML_BLOCK_SIZE;
    }
    slot->m_text = dest;
    memcpy(dest, text, bytes);
    builder->m_strings += bytes;
    slot->m_prev = object->m_strings;
    object->m_strings = int32_t(reinterpret_cast<char*>(slot) - reinterpret_cast<char*>(object));
    return true;
}

// ref: FUN_14066e150
bool XMLObjectParseChildren(XMLOBJECT* object, XMLCHILDPARSER parser, XMLBUILDER* builder) {
    XMLOBJECT** link = &object->m_child;
    while (*link) {
        link = &(*link)->m_next;
    }
    XMLNode* node = object->m_node->m_child;
    while (true) {
        XMLNode* current;
        do {
            current = node;
            if (current == nullptr) {
                *link = nullptr;
                return true;
            }
            node = current->m_next;
        } while (current->m_flags & 1);

        char* cursor;
        if (builder->m_block == nullptr || builder->m_end < builder->m_cursor + XML_BLOCK_RESERVE) {
            builder->m_block = XMLBuilderAllocBlock(builder, XML_BLOCK_SIZE);
            memset(builder->m_block, 0, XML_BLOCK_SIZE);
            cursor = builder->m_block;
            builder->m_cursor = cursor;
            builder->m_end = cursor + XML_BLOCK_SIZE;
        } else {
            cursor = builder->m_cursor;
        }
        auto child = reinterpret_cast<XMLOBJECT*>(cursor);
        child->m_node = current;
        char name[0x80];
        LowerName(name, current->m_name.m_data);
        _strlwr(name);
        if (parser(child, builder, name)) {
            current->m_flags |= 1;
            *link = child;
            link = &child->m_next;
        }
    }
}

// ref: FUN_140669bf0
XMLOBJECT* XMLObjectFindChild(XMLOBJECT* object, uint32_t type) {
    for (XMLOBJECT* child = object->m_child; child; child = child->m_next) {
        if (child->m_type == type) {
            return child;
        }
    }
    return nullptr;
}

// ref: FUN_14066e300
uint64_t XMLObjectGetCompactSize(XMLOBJECT* object) {
    uint64_t strings = 0;
    for (int32_t offset = object->m_strings; offset != 0; ) {
        auto slot = reinterpret_cast<XMLSTRING*>(reinterpret_cast<char*>(object) + uint32_t(offset));
        strings += strlen(slot->m_text) + 1;
        offset = slot->m_prev;
    }
    uint64_t children = 0;
    for (XMLOBJECT* child = object->m_child; child; child = child->m_next) {
        children += XMLObjectGetCompactSize(child);
    }
    return int64_t(object->m_size) + children + strings;
}

// ref: FUN_14066dff0
bool XMLObjectCompact(XMLOBJECT* object, char** dest) {
    char* record = *dest;
    *dest = record + object->m_size;
    char* out = *dest;
    for (int32_t offset = object->m_strings; offset != 0; ) {
        auto slot = reinterpret_cast<XMLSTRING*>(reinterpret_cast<char*>(object) + uint32_t(offset));
        uint64_t bytes = strlen(slot->m_text) + 1;
        memcpy(out, slot->m_text, bytes);
        slot->m_text = *dest;
        *dest += bytes;
        out = *dest;
        offset = slot->m_prev;
    }
    XMLOBJECT** link = &object->m_child;
    for (XMLOBJECT* child = *link; child; child = *link) {
        *link = reinterpret_cast<XMLOBJECT*>(*dest);
        XMLObjectCompact(child, dest);
        link = &(*link)->m_next;
    }
    memcpy(record, object, object->m_size);
    return true;
}

// ref: FUN_14066a420
void XMLObjectReleaseScripts(XMLOBJECT* object, bool force) {
    if ((object->m_flags & 0x20) != 0 && !force) {
        return;
    }
    if ((object->m_flags >> 10 & 1) == 0) {
        for (XMLOBJECT* child = object->m_child; child; child = child->m_next) {
            XMLObjectReleaseScripts(child, force);
        }
        return;
    }
    auto handler = static_cast<XMLSCRIPTHANDLER*>(object);
    int32_t* ref = handler->m_ref;
    if (ref != nullptr) {
        if (*ref > 0) {
            FrameScript_ReleaseFunction(*ref);
            *ref = 0;
        }
        SMemFree(handler->m_ref, __FILE__, 0x176, 0);
    }
}

// ref: FUN_14066e2c0
void XMLObjectFree(XMLOBJECT* object, bool force) {
    if (object != nullptr) {
        XMLObjectReleaseScripts(object, force);
        if (object->m_block != nullptr) {
            SMemFree(object->m_block, __FILE__, 400, 0);
        }
    }
}

// ref: FUN_140671f20
XMLOBJECT* XMLObjectCompile(XMLOBJECT* object, uint64_t* size) {
    uint64_t bytes = XMLObjectGetCompactSize(object);
    *size = bytes;
    if (bytes == 0) {
        return nullptr;
    }
    auto compiled = static_cast<XMLOBJECT*>(SMemAlloc(bytes, 0x20, __FILE__, SStrHashFnv1a(__FILE__), 0x150, 0));
    char* dest = reinterpret_cast<char*>(compiled);
    XMLObjectCompact(object, &dest);
    if (compiled != nullptr) {
        XMLObjectReleaseScripts(compiled, false);
        if (compiled->m_block != nullptr) {
            SMemFree(compiled->m_block, __FILE__, 400, 0);
        }
    }
    compiled->m_block = compiled;
    compiled->m_next = nullptr;
    return compiled;
}

// ref: FUN_1406698c0
XMLOBJECT* XMLObjectCreateDocument(XMLNode* root, XMLBUILDER* builder) {
    XMLBuilderReserve(builder);
    auto object = reinterpret_cast<XMLOBJECT*>(builder->m_cursor);
    object->m_node = root;
    object->m_next = nullptr;
    root->m_flags |= 1;
    object->m_type = XMLTYPE_UI;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLOBJECT);
    object->m_node->MarkAttributesUsed();
    object->m_size = SizeOf(object, builder);
    XMLParseHeader(object, builder);
    XMLObjectParseChildren(object, [](XMLOBJECT* child, XMLBUILDER* b, const char* name) -> bool {
        // ref: FUN_14066d1c0
        if (strcmp(name, "scopedmodifier") == 0) {
            auto modifier = static_cast<XMLSCOPEDMODIFIER*>(child);
            child->m_type = XMLTYPE_SCOPEDMODIFIER;
            b->m_cursor = reinterpret_cast<char*>(child) + sizeof(XMLSCOPEDMODIFIER);
            const char* value = Attr(child, "forbidden");
            if (value && *value) {
                SetBoolFlag(&modifier->m_modifierFlags, 1, 1, value);
            }
            value = Attr(child, "scriptsUseGivenEnv");
            if (value && *value) {
                SetBoolFlag(&modifier->m_modifierFlags, 4, 3, value);
            }
            value = Attr(child, "hideFromGlobalEnv");
            if (value && *value) {
                SetBoolFlag(&modifier->m_modifierFlags, 0x40, 7, value);
            }
            value = Attr(child, "fullLockdown");
            if (value && *value) {
                SetBoolFlag(&modifier->m_modifierFlags, 0x10, 5, value);
            }
            child->m_size = SizeOf(child, b);
            XMLParseHeader(child, b);
            // LAB_14066c750: a scoped modifier holds what the document root holds
            return XMLObjectParseChildren(child, XMLParseTopLevel, b);
        }
        return XMLParseTopLevel(child, b, name);
    }, builder);
    return object;
}

// ref: FUN_140669640
XMLOBJECT* XMLObjectCreateFrame(XMLNode* node) {
    XMLBUILDER builder = {};
    void* block = SMemAlloc(XML_BLOCK_SIZE, 0x20, BINARYXML_H_FILE, SStrHashFnv1a(BINARYXML_H_FILE), 0x89, 0);
    XMLBuilderAddBlocks(&builder, 1, &block);
    builder.m_block = static_cast<char*>(block);
    memset(block, 0, XML_BLOCK_SIZE);
    builder.m_end = builder.m_block + XML_BLOCK_SIZE;

    auto object = reinterpret_cast<XMLOBJECT*>(builder.m_block);
    object->m_node = node;
    object->m_next = nullptr;
    node->m_flags |= 1;
    char name[0x80];
    LowerName(name, node->m_name.m_data);
    _strlwr(name);
    builder.m_cursor = builder.m_block + sizeof(XMLREGION);
    static_cast<XMLREGION*>(object)->m_frameType = XMLTYPE_FRAMEDEF;
    XMLParseFrameType(object, &builder, name);

    XMLOBJECT* compiled = nullptr;
    uint64_t bytes = XMLObjectGetCompactSize(object);
    if (bytes != 0) {
        compiled = static_cast<XMLOBJECT*>(SMemAlloc(bytes, 0x20, __FILE__, SStrHashFnv1a(__FILE__), 0x150, 0));
        char* dest = reinterpret_cast<char*>(compiled);
        XMLObjectCompact(object, &dest);
        if (compiled != nullptr) {
            XMLObjectReleaseScripts(compiled, false);
            if (compiled->m_block != nullptr) {
                SMemFree(compiled->m_block, __FILE__, 400, 0);
            }
        }
        compiled->m_block = compiled;
        compiled->m_next = nullptr;
    }

    // TSFixedArray<void*, 32> teardown: every block, then the array
    for (uint32_t i = 0; i < builder.m_blockCount; i++) {
        SMemFree(builder.m_blocks[i], BINARYXML_H_FILE, 0x83, 0);
    }
    if (builder.m_blocks) {
        SMemFree(builder.m_blocks, ".?AV?$TSFixedArray@PEAX$0CA@@@", -2, 0);
    }
    return compiled;
}

// ---- getters ---------------------------------------------------------------------------

// ref: FUN_140669c20
bool XMLInsetGetAbs(XMLINSET* inset, float* left, float* right, float* top, float* bottom) {
    *left = inset->m_left;
    *right = inset->m_right;
    *top = inset->m_top;
    *bottom = inset->m_bottom;
    bool given = (inset->m_insetFlags & 0xf) != 0;
    *left = ScaleUI(*left);
    *right = ScaleUI(*right);
    *top = ScaleUI(*top);
    *bottom = ScaleUI(*bottom);
    return given;
}

// ref: FUN_14066a0d0
bool XMLInsetGet(XMLINSET* inset, float* left, float* right, float* top, float* bottom, CStatus* status) {
    if (XMLInsetGetAbs(inset, left, right, top, bottom)) {
        return true;
    }
    auto child = static_cast<XMLINSET*>(inset->m_child);
    if (child == nullptr) {
        status->Add(1, "No \"right\", \"left\", \"top\", or \"bottom\" attributes in element: %s", inset->m_tag.m_text);
    } else {
        if (child->m_type == XMLTYPE_ABSINSET) {
            return XMLInsetGetAbs(child, left, right, top, bottom);
        }
        if (child->m_type == XMLTYPE_RELINSET) {
            *left = child->m_left;
            *right = child->m_right;
            *top = child->m_top;
            *bottom = child->m_bottom;
            return (child->m_insetFlags & 0xf) != 0;
        }
        status->Add(1, "Unknown child node in %s element: %s", inset->m_tag.m_text, child->m_tag.m_text);
    }
    return false;
}

// ref: FUN_140669e50
bool XMLDimensionGet(XMLDIMENSION* dimension, float* x, float* y, CStatus* status) {
    *x = dimension->m_x;
    *y = dimension->m_y;
    uint32_t flags = dimension->m_dimensionFlags;
    *x = ScaleUI(*x);
    *y = ScaleUI(*y);
    if (flags & 3) {
        return true;
    }
    auto child = static_cast<XMLDIMENSION*>(dimension->m_child);
    if (child != nullptr) {
        if (child->m_type == XMLTYPE_ABSDIMENSION) {
            *x = child->m_x;
            *y = child->m_y;
            *x = ScaleUI(*x);
            *y = ScaleUI(*y);
            return (child->m_dimensionFlags & 3) != 0;
        }
        if (child->m_type == XMLTYPE_RELDIMENSION) {
            *x = child->m_x;
            *y = child->m_y;
            return (child->m_dimensionFlags & 3) != 0;
        }
        status->Add(1, "Unknown child node in %s element: %s", dimension->m_tag.m_text, child->m_tag.m_text);
    }
    return false;
}

// ref: FUN_140669cf0
bool XMLAnchorGet(XMLANCHOR* anchor, uint32_t* point, uint32_t* relativePoint, float* x, float* y, CStatus* status) {
    if ((anchor->m_anchorFlags & 2) == 0) {
        anchor->m_relativePoint = anchor->m_point;
    }
    *point = anchor->m_point;
    *relativePoint = anchor->m_relativePoint;
    *y = 0.0f;
    *x = 0.0f;
    auto offset = static_cast<XMLDIMENSION*>(XMLObjectFindChild(anchor, XMLTYPE_OFFSET));
    if (offset == nullptr) {
        XMLDimensionGet(anchor, x, y, status);
        return true;
    }
    return XMLDimensionGet(offset, x, y, status);
}

// ref: FUN_14066a1e0
bool XMLAnchorGetCentered(XMLANCHOR* anchor, uint32_t* point, uint32_t* relativePoint, float* x, float* y, CStatus* status) {
    if ((anchor->m_anchorFlags & 2) == 0) {
        anchor->m_relativePoint = anchor->m_point;
    }
    *point = anchor->m_point;
    *relativePoint = anchor->m_relativePoint;
    *y = 0.0f;
    *x = 0.0f;
    auto offset = static_cast<XMLDIMENSION*>(XMLObjectFindChild(anchor, XMLTYPE_OFFSET));
    if (offset == nullptr) {
        XMLDimensionGet(anchor, x, y, status);
    } else if (!XMLDimensionGet(offset, x, y, status)) {
        return false;
    }
    *point = FRAMEPOINT_CENTER;
    return true;
}

// ref: FUN_140669da0
bool XMLColorGet(XMLCOLOR* color, uint32_t* argb) {
    if (color->m_colorFlags & 1) {
        *argb = PackChannel(color->m_a) << 24 | PackChannel(color->m_r) << 16 | PackChannel(color->m_g) << 8
            | PackChannel(color->m_b);
    }
    return color->m_colorFlags & 1;
}

// ref: FUN_140669fa0
bool XMLGradientGet(XMLGRADIENT* gradient, uint32_t* orientation, uint32_t* minColor, uint32_t* maxColor) {
    *orientation = gradient->m_orientation;
    for (XMLOBJECT* child = gradient->m_child; child; child = child->m_next) {
        if (child->m_type == XMLTYPE_MINCOLOR) {
            auto color = static_cast<XMLCOLOR*>(child);
            if (color->m_colorFlags & 1) {
                *minColor = PackChannel(color->m_a) << 24 | PackChannel(color->m_r) << 16
                    | PackChannel(color->m_g) << 8 | PackChannel(color->m_b);
            }
        } else if (child->m_type == XMLTYPE_MAXCOLOR) {
            XMLColorGet(static_cast<XMLCOLOR*>(child), maxColor);
        }
    }
    return true;
}

// ref: FUN_14066a2b0
bool XMLOffsetGet(XMLDIMENSION* dimension, float* x, float* y, CStatus* status) {
    for (XMLOBJECT* child = dimension->m_child; child; child = child->m_next) {
        if (child->m_type == XMLTYPE_OFFSET) {
            return !XMLDimensionGet(static_cast<XMLDIMENSION*>(child), x, y, status);
        }
    }
    *x = dimension->m_x;
    *y = dimension->m_y;
    return (dimension->m_dimensionFlags & 3) != 0;
}

// ref: FUN_14066a310
bool XMLValueGet(XMLVALUE* value, float* result, CStatus* status) {
    *result = value->m_value;
    *result = ScaleUI(*result);
    if (value->m_valueFlags & 1) {
        return true;
    }
    auto child = static_cast<XMLVALUE*>(value->m_child);
    if (child == nullptr) {
        status->Add(1, "No \"val\" attribute in element: %s", value->m_tag.m_text);
        return false;
    }
    if (child->m_type == XMLTYPE_ABSVALUE) {
        *result = child->m_value;
        *result = ScaleUI(*result);
        return child->m_valueFlags & 1;
    }
    if (child->m_type != XMLTYPE_RELVALUE) {
        status->Add(1, "Unknown child node in %s element: %s", value->m_tag.m_text, child->m_tag.m_text);
        return false;
    }
    *result = child->m_value;
    return child->m_valueFlags & 1;
}

// ---- element parsers -------------------------------------------------------------------

namespace {

// The attributes every element may carry: name, virtual, intrinsic, inherits, mixins.
// ref: FUN_140671cc0
bool XMLParseHeader(XMLOBJECT* object, XMLBUILDER* builder) {
    XMLObjectAddString(object, builder, &object->m_tag, object->m_node->m_name.m_data);
    const char* value = Attr(object, "virtual");
    if (value) {
        object->m_attributes |= 0x10;
        bool on = _stricmp(value, "true") == 0;
        object->m_attributes &= ~0x200u;
        object->m_attributes |= on ? 0x200 : 0;
    }
    value = Attr(object, "intrinsic");
    if (value) {
        object->m_attributes |= 0x20;
        bool on = _stricmp(value, "true") == 0;
        object->m_attributes &= ~0x400u;
        object->m_attributes |= on ? 0x400 : 0;
    }
    value = Attr(object, "name");
    if (value && *value) {
        XMLObjectAddString(object, builder, &object->m_name, value);
        object->m_attributes |= 0x40;
    }
    value = Attr(object, "secureReferenceKey");
    if (value && *value && (object->m_attributes & 0x210) != 0x210) {
        XMLObjectAddString(object, builder, &object->m_secureReferenceKey, value);
        object->m_attributes |= 0x80;
    }
    value = Attr(object, "inherits");
    if (value && *value) {
        object->m_attributes |= 0x100;
        XMLObjectAddString(object, builder, &object->m_inherits, value);
    }
    value = Attr(object, "mixin");
    if (value && *value) {
        object->m_attributes |= 0x800;
        XMLObjectAddString(object, builder, &object->m_mixin, value);
    }
    value = Attr(object, "secureMixin");
    if (value && *value) {
        object->m_attributes |= 0x1000;
        XMLObjectAddString(object, builder, &object->m_secureMixin, value);
    }
    object->m_location = *object->m_node->GetLocation();
    object->m_size = SizeOf(object, builder);
    return true;
}

// ref: FUN_140671210
bool XMLParseDimension(XMLDIMENSION* object, XMLBUILDER* builder) {
    const char* value = Attr(object, "x");
    if (value && *value) {
        float x = SStrToFloat(value);
        object->m_dimensionFlags |= 1;
        object->m_x = x;
    }
    value = Attr(object, "y");
    if (value && *value) {
        float y = SStrToFloat(value);
        object->m_dimensionFlags |= 2;
        object->m_y = y;
    }
    object->m_size = SizeOf(object, builder);
    return XMLParseHeader(object, builder);
}

// ref: FUN_1406712b0
bool XMLParseInset(XMLINSET* object, XMLBUILDER* builder) {
    const char* value = Attr(object, "left");
    if (value && *value) {
        float f = SStrToFloat(value);
        object->m_insetFlags |= 1;
        object->m_left = f;
    }
    value = Attr(object, "right");
    if (value && *value) {
        float f = SStrToFloat(value);
        object->m_insetFlags |= 2;
        object->m_right = f;
    }
    value = Attr(object, "top");
    if (value && *value) {
        float f = SStrToFloat(value);
        object->m_insetFlags |= 4;
        object->m_top = f;
    }
    value = Attr(object, "bottom");
    if (value && *value) {
        float f = SStrToFloat(value);
        object->m_insetFlags |= 8;
        object->m_bottom = f;
    }
    object->m_size = SizeOf(object, builder);
    XMLParseHeader(object, builder);
    return true;
}

// ref: FUN_14066e390
bool XMLParseValue(XMLVALUE* object, XMLBUILDER* builder) {
    object->m_size = SizeOf(object, builder);
    const char* value = Attr(object, "val");
    if (value && *value) {
        float f = SStrToFloat(value);
        object->m_valueFlags |= 1;
        object->m_value = f;
    }
    object->m_size = SizeOf(object, builder);
    XMLParseHeader(object, builder);
    return true;
}

// ref: FUN_14066ea90
bool XMLParseColor(XMLCOLOR* object, XMLBUILDER* builder) {
    object->m_colorFlags |= 1;
    object->m_r = 0.0f;
    object->m_g = 0.0f;
    object->m_b = 0.0f;
    object->m_a = 1.0f;
    const char* value = Attr(object, "color");
    if (value == nullptr) {
        value = Attr(object, "r");
        if (value) {
            object->m_r = ClampUnit(value);
        }
        value = Attr(object, "g");
        if (value) {
            object->m_g = ClampUnit(value);
        }
        value = Attr(object, "b");
        if (value) {
            object->m_b = ClampUnit(value);
        }
    } else {
        GXUCOLORNAMELOOKUP lookup = GxuFontGetColorNameLookup();
        if (lookup) {
            bool found = false;
            const uint8_t* bgra = lookup(value, &found);
            if (found) {
                object->m_r = float(bgra[2]) * (1.0f / 255.0f);
                object->m_g = float(bgra[1]) * (1.0f / 255.0f);
                object->m_b = float(bgra[0]) * (1.0f / 255.0f);
                object->m_a = float(bgra[3]) * (1.0f / 255.0f);
            }
        }
    }
    value = Attr(object, "a");
    if (value) {
        object->m_a = ClampUnit(value);
    }
    object->m_size = SizeOf(object, builder);
    XMLParseHeader(object, builder);
    return true;
}

// ref: FUN_140670ba0
bool XMLParseRect(XMLRECT* object, XMLBUILDER* builder) {
    const char* const names[] = { "ULx", "ULy", "URx", "URy", "LLx", "LLy", "LRx", "LRy" };
    float* const fields[] = { &object->m_ulx, &object->m_uly, &object->m_urx, &object->m_ury,
        &object->m_llx, &object->m_lly, &object->m_lrx, &object->m_lry };
    for (uint32_t i = 0; i < 8; i++) {
        const char* value = Attr(object, names[i]);
        if (value) {
            object->m_rectFlags |= 1u << i;
            *fields[i] = SStrToFloat(value);
        }
    }
    object->m_size = SizeOf(object, builder);
    return XMLParseHeader(object, builder);
}

// LAB_14066d030
bool XMLParseTexCoordsChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "rect") != 0) {
        return false;
    }
    object->m_type = XMLTYPE_RECT;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLRECT);
    return XMLParseRect(static_cast<XMLRECT*>(object), builder);
}

// ref: FUN_140671520
bool XMLParseTexCoords(XMLTEXCOORDS* object, XMLBUILDER* builder) {
    const char* value = Attr(object, "flip");
    if (value) {
        SetBoolFlag(&object->m_texFlags, 0x40, 7, value);
    }
    value = Attr(object, "degrees");
    if (value) {
        object->m_texFlags |= 0x10;
        object->m_degrees = SStrToFloat(value);
    }
    value = Attr(object, "radians");
    if (value) {
        object->m_texFlags |= 0x20;
        object->m_radians = SStrToFloat(value);
    }
    value = Attr(object, "left");
    if (value) {
        object->m_texFlags |= 1;
        object->m_left = SStrToFloat(value);
    }
    value = Attr(object, "right");
    if (value) {
        object->m_texFlags |= 2;
        object->m_right = SStrToFloat(value);
    }
    value = Attr(object, "top");
    if (value) {
        object->m_texFlags |= 4;
        object->m_top = SStrToFloat(value);
    }
    value = Attr(object, "bottom");
    if (value) {
        object->m_texFlags |= 8;
        object->m_bottom = SStrToFloat(value);
    }
    object->m_size = SizeOf(object, builder);
    XMLParseHeader(object, builder);
    return XMLObjectParseChildren(object, XMLParseTexCoordsChild, builder);
}

// ref: FUN_14066e400
bool XMLParseAnchor(XMLANCHOR* object, XMLBUILDER* builder) {
    const char* value = Attr(object, "relativeTo");
    if (value) {
        object->m_anchorFlags |= 4;
        XMLObjectAddString(object, builder, &object->m_relativeTo, value);
    }
    value = Attr(object, "relativeKey");
    if (value) {
        object->m_anchorFlags |= 8;
        XMLObjectAddString(object, builder, &object->m_relativeKey, value);
    }
    value = Attr(object, "point");
    if (value && StringToFramePoint(value, &object->m_point)) {
        object->m_anchorFlags |= 1;
    }
    value = Attr(object, "relativePoint");
    if (value && StringToFramePoint(value, &object->m_relativePoint)) {
        object->m_anchorFlags |= 2;
    }
    object->m_size = SizeOf(object, builder);
    XMLParseDimension(object, builder);
    XMLObjectParseChildren(object, XMLParseDimensionChild, builder);
    return XMLObjectParseChildren(object, [](XMLOBJECT* child, XMLBUILDER* b, const char* name) -> bool {
        // ref: FUN_14066a4c0
        if (strcmp(name, "offset") != 0) {
            return false;
        }
        child->m_type = XMLTYPE_OFFSET;
        b->m_cursor = reinterpret_cast<char*>(child) + sizeof(XMLDIMENSION);
        child->m_size = SizeOf(child, b);
        XMLParseDimension(static_cast<XMLDIMENSION*>(child), b);
        return XMLObjectParseChildren(child, XMLParseDimensionChild, b);
    }, builder);
}

// <AbsDimension> / <RelDimension> under a dimension element.
// ref: FUN_14066b2c0
bool XMLParseDimensionChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "absdimension") == 0) {
        object->m_type = XMLTYPE_ABSDIMENSION;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLDIMENSION);
        object->m_size = SizeOf(object, builder);
        XMLParseDimension(static_cast<XMLDIMENSION*>(object), builder);
        return true;
    }
    if (strcmp(name, "reldimension") == 0) {
        object->m_type = XMLTYPE_RELDIMENSION;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLDIMENSION);
        return XMLParseDimension(static_cast<XMLDIMENSION*>(object), builder);
    }
    return false;
}

// <AbsInset> / <RelInset> under an inset element.
// ref: FUN_14066bad0
bool XMLParseInsetChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "absinset") == 0) {
        object->m_type = XMLTYPE_ABSINSET;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLINSET);
        object->m_size = SizeOf(object, builder);
        XMLParseInset(static_cast<XMLINSET*>(object), builder);
        return true;
    }
    if (strcmp(name, "relinset") == 0) {
        object->m_type = XMLTYPE_RELINSET;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLINSET);
        return XMLParseInset(static_cast<XMLINSET*>(object), builder);
    }
    return false;
}

// <AbsValue> / <RelValue> under a value element.
// ref: FUN_14066d360
bool XMLParseValueChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "absvalue") == 0) {
        object->m_type = XMLTYPE_ABSVALUE;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLVALUE);
        return XMLParseValue(static_cast<XMLVALUE*>(object), builder);
    }
    if (strcmp(name, "relvalue") == 0) {
        auto value = static_cast<XMLVALUE*>(object);
        object->m_type = XMLTYPE_RELVALUE;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLVALUE);
        const char* text = Attr(object, "val");
        if (text && *text) {
            float f = SStrToFloat(text);
            value->m_valueFlags |= 1;
            value->m_value = f;
        }
        object->m_size = SizeOf(object, builder);
        XMLParseHeader(object, builder);
        return true;
    }
    return false;
}

// ref: FUN_140669190
bool XMLParseOffsetBody(XMLDIMENSION* object, XMLBUILDER* builder) {
    object->m_type = XMLTYPE_OFFSET;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLDIMENSION);
    object->m_size = SizeOf(object, builder);
    XMLParseDimension(object, builder);
    return XMLObjectParseChildren(object, XMLParseDimensionChild, builder);
}

// ref: FUN_140669370
bool XMLParseViewInsets(XMLINSET* object, XMLBUILDER* builder) {
    object->m_type = XMLTYPE_VIEWINSETS;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLINSET);
    object->m_size = SizeOf(object, builder);
    XMLParseInset(object, builder);
    return XMLObjectParseChildren(object, XMLParseInsetChild, builder);
}

// <Color> under a font's <Shadow>, or the shadow's <Offset>.
// LAB_14066cea0
bool XMLParseShadowChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "color") == 0) {
        object->m_type = XMLTYPE_COLOR;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLCOLOR);
        return XMLParseColor(static_cast<XMLCOLOR*>(object), builder);
    }
    if (strcmp(name, "offset") == 0) {
        return XMLParseOffsetBody(static_cast<XMLDIMENSION*>(object), builder);
    }
    return false;
}

// LAB_14066b270
bool XMLParseColorChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "color") != 0) {
        return false;
    }
    object->m_type = XMLTYPE_COLOR;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLCOLOR);
    return XMLParseColor(static_cast<XMLCOLOR*>(object), builder);
}

// <Color>, <Shadow>, <FontHeight> under a font.
// ref: FUN_14066b4a0
bool XMLParseFontChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "color") == 0) {
        object->m_type = XMLTYPE_COLOR;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLCOLOR);
        return XMLParseColor(static_cast<XMLCOLOR*>(object), builder);
    }
    if (strcmp(name, "shadow") == 0) {
        object->m_type = XMLTYPE_SHADOW;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLDIMENSION);
        object->m_size = SizeOf(object, builder);
        XMLParseDimension(static_cast<XMLDIMENSION*>(object), builder);
        return XMLObjectParseChildren(object, XMLParseShadowChild, builder);
    }
    if (strcmp(name, "fontheight") == 0) {
        object->m_type = XMLTYPE_FONTHEIGHT;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLVALUE);
        object->m_size = SizeOf(object, builder);
        XMLParseValue(static_cast<XMLVALUE*>(object), builder);
        return XMLObjectParseChildren(object, XMLParseValueChild, builder);
    }
    return false;
}

// ref: FUN_14066f730
bool XMLParseFont(XMLFONT* object, XMLBUILDER* builder) {
    object->m_flags |= 0x100000;
    const char* value = Attr(object, "font");
    if (value) {
        object->m_fontFlags |= 0x80;
        XMLObjectAddString(object, builder, &object->m_font, value);
    }
    value = Attr(object, "height");
    if (value) {
        object->m_fontFlags |= 0x800;
        float height = 120.0f;
        if (SStrToFloat(value) < 120.0f) {
            height = SStrToFloat(value);
        }
        object->m_height = height;
    }
    value = Attr(object, "justifyV");
    if (value) {
        object->m_fontFlags |= 1;
        XMLObjectAddString(object, builder, &object->m_justifyV, value);
    }
    value = Attr(object, "justifyH");
    if (value) {
        object->m_fontFlags |= 2;
        XMLObjectAddString(object, builder, &object->m_justifyH, value);
    }
    value = Attr(object, "outline");
    if (value) {
        object->m_fontFlags |= 0x100;
        XMLObjectAddString(object, builder, &object->m_outline, value);
    }
    value = Attr(object, "monochrome");
    if (value && *value) {
        SetBoolFlag(&object->m_fontFlags, 0x200, 10, value);
    }
    value = Attr(object, "indented");
    if (value && *value) {
        SetBoolFlag(&object->m_fontFlags, 8, 4, value);
    }
    value = Attr(object, "nonspacewrap");
    if (value && *value) {
        SetBoolFlag(&object->m_fontFlags, 4, 6, value);
    }
    value = Attr(object, "spacing");
    if (value && *value) {
        object->m_fontFlags |= 0x20;
        object->m_spacing = SStrToFloat(value);
    }
    value = Attr(object, "filter");
    if (value && *value) {
        SetBoolFlag(&object->m_fontFlags, 0x1000, 13, value);
    }
    value = Attr(object, "fixedsize");
    if (value && *value) {
        SetBoolFlag(&object->m_fontFlags, 0x4000, 15, value);
    }
    object->m_size = SizeOf(object, builder);
    XMLParseHeader(object, builder);
    return XMLObjectParseChildren(object, XMLParseFontChild, builder);
}

// <Size>, <Anchors>, <Animations>, <KeyValues>, <Scripts> under a region.
// ref: FUN_14066c510
bool XMLParseRegionChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "size") == 0) {
        object->m_type = XMLTYPE_SIZE;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLDIMENSION);
        object->m_size = SizeOf(object, builder);
        XMLParseDimension(static_cast<XMLDIMENSION*>(object), builder);
        return XMLObjectParseChildren(object, XMLParseDimensionChild, builder);
    }
    if (strcmp(name, "anchors") == 0) {
        object->m_type = XMLTYPE_ANCHORS;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLOBJECT);
        object->m_size = SizeOf(object, builder);
        XMLParseHeader(object, builder);
        return XMLObjectParseChildren(object, [](XMLOBJECT* child, XMLBUILDER* b, const char* n) -> bool {
            // LAB_14066a550
            if (strcmp(n, "anchor") != 0) {
                return false;
            }
            child->m_type = XMLTYPE_ANCHOR;
            b->m_cursor = reinterpret_cast<char*>(child) + sizeof(XMLANCHOR);
            return XMLParseAnchor(static_cast<XMLANCHOR*>(child), b);
        }, builder);
    }
    if (strcmp(name, "animations") == 0) {
        return XMLParseAnimations(object, builder);
    }
    if (strcmp(name, "keyvalues") == 0) {
        return XMLParseKeyValues(object, builder);
    }
    if (strcmp(name, "scripts") == 0) {
        return XMLParseScripts(object, builder);
    }
    return false;
}

// ref: FUN_140670d80
bool XMLParseRegion(XMLREGION* object, XMLBUILDER* builder) {
    object->m_flags |= 0x200;
    uint32_t* flags = &object->m_regionFlags;
    const char* value = Attr(object, "alpha");
    if (value) {
        *flags |= 1;
        object->m_alpha = SStrToFloat(value);
    }
    value = Attr(object, "hidden");
    if (value) {
        *flags |= 2;
        bool hidden = _stricmp(value, "true") == 0;
        *flags &= ~0x10u;
        *flags |= hidden ? 0x10 : 0;
    }
    value = Attr(object, "parentKey");
    if (value && *value) {
        *flags |= 4;
        XMLObjectAddString(object, builder, &object->m_parentKey, value);
    }
    value = Attr(object, "parentArray");
    if (value && *value) {
        *flags |= 8;
        XMLObjectAddString(object, builder, &object->m_parentArray, value);
    }
    value = Attr(object, "ignoreParentAlpha");
    if (value && *value) {
        SetBoolFlag(flags, 0x20, 6, value);
    }
    value = Attr(object, "ignoreParentScale");
    if (value && *value) {
        bool b = StringToBOOL(value);
        *flags = (*flags & ~0x100u) | uint32_t(b) << 8 | 0x80;
    }
    value = Attr(object, "scale");
    if (value && *value) {
        *flags |= 0x200;
        object->m_scale = SStrToFloat(value);
    }
    value = Attr(object, "enableMouse");
    if (value && *value) {
        SetBoolFlag(flags, 0x400, 11, value);
    }
    value = Attr(object, "enableMouseClicks");
    if (value && *value) {
        SetBoolFlag(flags, 0x1000, 13, value);
    }
    value = Attr(object, "enableMouseMotion");
    if (value && *value) {
        SetBoolFlag(flags, 0x4000, 15, value);
    }
    value = Attr(object, "registerForDrag");
    if (value && *value) {
        *flags |= 0x10000;
        XMLObjectAddString(object, builder, &object->m_registerForDrag, value);
    }
    value = Attr(object, "passThroughButtons");
    if (value && *value) {
        *flags |= 0x20000;
        XMLObjectAddString(object, builder, &object->m_passThroughButtons, value);
    }
    value = Attr(object, "retainClickThroughOverride");
    if (value && *value) {
        SetBoolFlag(flags, 0x40000, 19, value);
    }
    value = Attr(object, "protected");
    if (value && *value) {
        SetBoolFlag(flags, 0x100000, 21, value);
    }
    object->m_size = SizeOf(object, builder);
    value = Attr(object, "setAllPoints");
    if (value && *value) {
        bool b = StringToBOOL(value);
        object->m_setAllPoints = (object->m_setAllPoints & ~2u) | uint32_t(b) * 2 | 1;
    }
    object->m_size = SizeOf(object, builder);
    XMLParseFont(object, builder);
    return XMLObjectParseChildren(object, XMLParseRegionChild, builder);
}

// <MaskedTexture> under <MaskedTextures>.
// ref: FUN_14066c090
bool XMLParseMaskedTexturesChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "maskedtexture") != 0) {
        return false;
    }
    auto masked = static_cast<XMLMASKEDTEXTURE*>(object);
    object->m_type = XMLTYPE_MASKEDTEXTURE;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLMASKEDTEXTURE);
    const char* value = Attr(object, "target");
    if (value) {
        masked->m_maskFlags |= 1;
        XMLObjectAddString(object, builder, &masked->m_target, value);
    }
    value = Attr(object, "childKey");
    if (value) {
        masked->m_maskFlags |= 2;
        XMLObjectAddString(object, builder, &masked->m_childKey, value);
    }
    object->m_size = SizeOf(object, builder);
    XMLParseHeader(object, builder);
    return true;
}

// ref: FUN_140668ef0
bool XMLParseMaskedTextures(XMLOBJECT* object, XMLBUILDER* builder) {
    object->m_type = XMLTYPE_MASKEDTEXTURES;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLOBJECT);
    object->m_size = SizeOf(object, builder);
    XMLParseHeader(object, builder);
    return XMLObjectParseChildren(object, XMLParseMaskedTexturesChild, builder);
}

// <MinColor> / <MaxColor> under <Gradient>.
// ref: FUN_14066ba40
bool XMLParseGradientChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "mincolor") == 0) {
        object->m_type = XMLTYPE_MINCOLOR;
    } else if (strcmp(name, "maxcolor") == 0) {
        object->m_type = XMLTYPE_MAXCOLOR;
    } else {
        return false;
    }
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLCOLOR);
    object->m_size = SizeOf(object, builder);
    return XMLParseColor(static_cast<XMLCOLOR*>(object), builder);
}

// <TexCoords>, <Gradient>, <MaskedTextures> under a texture.
// ref: FUN_14066d080
bool XMLParseTextureChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "texcoords") == 0) {
        object->m_type = XMLTYPE_TEXCOORDS;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLTEXCOORDS);
        return XMLParseTexCoords(static_cast<XMLTEXCOORDS*>(object), builder);
    }
    if (strcmp(name, "gradient") == 0) {
        auto gradient = static_cast<XMLGRADIENT*>(object);
        object->m_type = XMLTYPE_GRADIENT;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLGRADIENT);
        gradient->m_orientation = 0;
        const char* value = Attr(object, "orientation");
        if (value && *value && StringToOrientation(value, &gradient->m_orientation)) {
            gradient->m_gradientFlags |= 1;
        }
        object->m_size = SizeOf(object, builder);
        XMLParseHeader(object, builder);
        return XMLObjectParseChildren(object, XMLParseGradientChild, builder);
    }
    if (strcmp(name, "maskedtextures") == 0) {
        return XMLParseMaskedTextures(object, builder);
    }
    return false;
}

// ref: FUN_1406716d0
bool XMLParseTexture(XMLTEXTURE* object, XMLBUILDER* builder) {
    object->m_flags |= 0x200000;
    uint32_t* flags = &object->m_textureFlags;
    const char* value = Attr(object, "desaturated");
    if (value) {
        SetBoolFlag(flags, 0x1000000, 25, value);
    }
    value = Attr(object, "useAtlasSize");
    if (value) {
        bool b = StringToBOOL(value);
        *flags &= ~0x8000000u;
        *flags |= uint32_t(b) << 27;
    }
    value = Attr(object, "noanimalpha");
    if (value && *value) {
        SetBoolFlag(flags, 0x400000, 23, value);
    }
    value = Attr(object, "nounload");
    if (value && *value) {
        SetBoolFlag(flags, 0x40000, 20, value);
    }
    value = Attr(object, "nolazyload");
    if (value && *value) {
        SetBoolFlag(flags, 0x80000, 21, value);
    }
    value = Attr(object, "horizTile");
    if (value && *value) {
        *flags |= 0x200;
        *flags &= ~0x100u;
        bool b = StringToBOOL(value);
        *flags &= 0xffffe3ff;
        *flags |= uint32_t(b) << 10;
    }
    value = Attr(object, "hWrapMode");
    if (value && *value) {
        *flags &= ~0x200u;
        *flags |= 0x100;
        uint8_t mode = 0;
        StringToWrapMode(value, &mode);
        *flags ^= (uint32_t(mode) << 10 ^ *flags) & 0x1c00;
    }
    value = Attr(object, "vertTile");
    if (value && *value) {
        *flags |= 0x2000;
        *flags &= ~0x4000u;
        bool b = StringToBOOL(value);
        *flags &= 0xfffc7fff;
        *flags |= uint32_t(b) << 15;
    }
    value = Attr(object, "vWrapMode");
    if (value && *value) {
        *flags &= ~0x2000u;
        *flags |= 0x4000;
        uint8_t mode = 0;
        StringToWrapMode(value, &mode);
        *flags ^= (uint32_t(mode) << 15 ^ *flags) & 0x38000;
    }
    value = Attr(object, "nonBlocking");
    if (value && *value) {
        SetBoolFlag(flags, 0x40, 7, value);
    }
    value = Attr(object, "hidden");
    if (value && *value) {
        SetBoolFlag(flags, 0x10, 5, value);
    }
    value = Attr(object, "rotation");
    if (value && *value) {
        *flags |= 0x10000000;
        object->m_rotation = SStrToFloat(value) * DEG_TO_RAD;
    }
    value = Attr(object, "snapToPixelGrid");
    if (value && *value) {
        SetBoolFlag(flags, 0x20000000, 30, value);
    }
    value = Attr(object, "texelSnappingBias");
    if (value && *value) {
        *flags |= 0x4000000;
        object->m_texelSnappingBias = SStrToFloat(value);
    }
    value = Attr(object, "atlas");
    if (value) {
        *flags |= 4;
        XMLObjectAddString(object, builder, &object->m_atlas, value);
    }
    value = Attr(object, "file");
    if (value) {
        *flags |= 2;
        XMLObjectAddString(object, builder, &object->m_file, value);
    }
    value = Attr(object, "mask");
    if (value) {
        *flags |= 1;
        XMLObjectAddString(object, builder, &object->m_mask, value);
    }
    object->m_alphaMode = 2;
    value = Attr(object, "alphaMode");
    if (value) {
        *flags |= 8;
        if (*value) {
            StringToBlendMode(value, &object->m_alphaMode);
        }
    }
    object->m_size = SizeOf(object, builder);
    XMLParseRegion(object, builder);
    return XMLObjectParseChildren(object, XMLParseTextureChild, builder);
}

// <StartAnchor> / <EndAnchor> under a line.
// ref: FUN_14066bf70
bool XMLParseLineChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "startanchor") == 0) {
        object->m_type = XMLTYPE_STARTANCHOR;
    } else if (strcmp(name, "endanchor") == 0) {
        object->m_type = XMLTYPE_ENDANCHOR;
    } else {
        return false;
    }
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLANCHOR);
    object->m_size = SizeOf(object, builder);
    return XMLParseAnchor(static_cast<XMLANCHOR*>(object), builder);
}

// ref: FUN_140670550
bool XMLParseLine(XMLLINE* object, XMLBUILDER* builder) {
    object->m_flags |= 0x400000;
    const char* value = Attr(object, "thickness");
    if (value) {
        object->m_lineFlags |= 1;
        object->m_thickness = SStrToFloat(value);
    }
    object->m_size = SizeOf(object, builder);
    XMLParseTexture(object, builder);
    return XMLObjectParseChildren(object, XMLParseLineChild, builder);
}

// ref: FUN_14066fac0
bool XMLParseFontString(XMLFONTSTRING* object, XMLBUILDER* builder) {
    object->m_flags |= 0x800000;
    uint32_t* flags = &object->m_fontStringFlags;
    const char* value = Attr(object, "degrees");
    if (value && *value) {
        *flags |= 0x20;
        object->m_rotation = SStrToFloat(value) * DEG_TO_RAD;
    }
    value = Attr(object, "radians");
    if (value && *value) {
        *flags |= 0x20;
        object->m_rotation = SStrToFloat(value);
    }
    value = Attr(object, "scale");
    if (value && *value) {
        *flags |= 0x40;
        object->m_textScale = SStrToFloat(value);
    }
    value = Attr(object, "wordwrap");
    if (value && *value) {
        SetBoolFlag(flags, 8, 4, value);
    }
    value = Attr(object, "text");
    if (value && *value) {
        *flags |= 4;
        XMLObjectAddString(object, builder, &object->m_text, value);
    }
    value = Attr(object, "bytes");
    if (value) {
        *flags |= 1;
        object->m_bytes = strtol(value, nullptr, IntRadix(value));
    }
    value = Attr(object, "maxLines");
    if (value && *value) {
        int32_t lines = strtol(value, nullptr, IntRadix(value));
        *flags |= 2;
        object->m_maxLines = lines;
    }
    object->m_size = SizeOf(object, builder);
    return XMLParseRegion(object, builder);
}

// <MaskedTextures> under a <MaskTexture>.
// ref: FUN_14066c010
bool XMLParseMaskTextureChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "maskedtextures") != 0) {
        return false;
    }
    object->m_type = XMLTYPE_MASKEDTEXTURES;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLOBJECT);
    object->m_size = SizeOf(object, builder);
    XMLParseHeader(object, builder);
    return XMLObjectParseChildren(object, XMLParseMaskedTexturesChild, builder);
}

// What a <Layer> holds: font strings, textures, lines, mask textures.
// ref: FUN_14066bcf0
bool XMLParseLayerChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (XMLParseFontStringChild(object, builder, name)) {
        return true;
    }
    if (strcmp(name, "texture") == 0) {
        object->m_type = XMLTYPE_TEXTURE;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLTEXTURE);
        return XMLParseTexture(static_cast<XMLTEXTURE*>(object), builder);
    }
    if (strcmp(name, "line") == 0) {
        object->m_type = XMLTYPE_LINE;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLLINE);
        return XMLParseLine(static_cast<XMLLINE*>(object), builder);
    }
    if (strcmp(name, "masktexture") == 0) {
        object->m_type = XMLTYPE_MASKTEXTURE;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLTEXTURE);
        object->m_size = SizeOf(object, builder);
        XMLParseTexture(static_cast<XMLTEXTURE*>(object), builder);
        return XMLObjectParseChildren(object, XMLParseMaskTextureChild, builder);
    }
    return false;
}

// <Layer> under <Layers>.
// ref: FUN_14066be30
bool XMLParseLayersChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "layer") != 0) {
        return false;
    }
    auto layer = static_cast<XMLLAYER*>(object);
    object->m_type = XMLTYPE_LAYER;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLLAYER);
    layer->m_level = DRAWLAYER_ARTWORK;
    layer->m_subLevel = 0;
    const char* value = Attr(object, "level");
    if (value) {
        layer->m_layerFlags |= 1;
        StringToDrawLayer(value, &layer->m_level);
    }
    value = Attr(object, "textureSubLevel");
    if (value) {
        int32_t level = strtol(value, nullptr, IntRadix(value));
        int8_t clamped;
        if (level > 7) {
            clamped = 7;
        } else {
            clamped = int8_t(level < -8 ? -8 : level);
        }
        layer->m_layerFlags |= 2;
        layer->m_subLevel = clamped;
    }
    object->m_size = SizeOf(object, builder);
    XMLParseHeader(object, builder);
    return XMLObjectParseChildren(object, XMLParseLayerChild, builder);
}

// <Attribute> under <Attributes>.
// ref: FUN_14066abb0
bool XMLParseAttributesChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "attribute") != 0) {
        return false;
    }
    auto attribute = static_cast<XMLATTRIBUTE*>(object);
    object->m_type = XMLTYPE_ATTRIBUTE;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLATTRIBUTE);
    object->m_size = SizeOf(object, builder);
    const char* value = Attr(object, "type");
    if (value) {
        attribute->m_attributeFlags |= 2;
        XMLObjectAddString(object, builder, &attribute->m_valueType, value);
    }
    value = Attr(object, "value");
    if (value) {
        attribute->m_attributeFlags |= 1;
        XMLObjectAddString(object, builder, &attribute->m_value, value);
    }
    XMLParseHeader(object, builder);
    return true;
}

// <KeyValue> under <KeyValues>.
// ref: FUN_14066bb90
bool XMLParseKeyValuesChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "keyvalue") != 0) {
        return false;
    }
    auto keyValue = static_cast<XMLKEYVALUE*>(object);
    object->m_type = XMLTYPE_KEYVALUE;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLKEYVALUE);
    object->m_size = SizeOf(object, builder);
    const char* value = Attr(object, "key");
    if (value) {
        keyValue->m_keyValueFlags |= 1;
        XMLObjectAddString(object, builder, &keyValue->m_key, value);
    }
    value = Attr(object, "keyType");
    if (value) {
        keyValue->m_keyValueFlags |= 2;
        XMLObjectAddString(object, builder, &keyValue->m_keyType, value);
    }
    value = Attr(object, "type");
    if (value) {
        keyValue->m_keyValueFlags |= 8;
        XMLObjectAddString(object, builder, &keyValue->m_valueType, value);
    }
    value = Attr(object, "value");
    if (value) {
        keyValue->m_keyValueFlags |= 4;
        XMLObjectAddString(object, builder, &keyValue->m_value, value);
    }
    XMLParseHeader(object, builder);
    return true;
}

// ref: FUN_140668ea0
bool XMLParseKeyValues(XMLOBJECT* object, XMLBUILDER* builder) {
    object->m_type = XMLTYPE_KEYVALUES;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLOBJECT);
    object->m_size = SizeOf(object, builder);
    XMLParseHeader(object, builder);
    return XMLObjectParseChildren(object, XMLParseKeyValuesChild, builder);
}

// ref: FUN_1406691f0
bool XMLParseScripts(XMLOBJECT* object, XMLBUILDER* builder) {
    object->m_type = XMLTYPE_SCRIPTS;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLOBJECT);
    object->m_size = SizeOf(object, builder);
    XMLParseHeader(object, builder);
    return XMLObjectParseChildren(object, XMLParseScriptsChild, builder);
}

// ref: FUN_140670390
bool XMLParseScriptHandler(XMLSCRIPTHANDLER* object, XMLBUILDER* builder) {
    object->m_flags |= 0x400;
    object->m_handlerFlags |= 0x200;
    const char* value = Attr(object, "function");
    if (value && *value) {
        object->m_handlerFlags |= 1;
        XMLObjectAddString(object, builder, &object->m_function, value);
    }
    value = Attr(object, "method");
    if (value && *value) {
        object->m_handlerFlags |= 2;
        XMLObjectAddString(object, builder, &object->m_method, value);
    }
    value = Attr(object, "inherit");
    if (value) {
        if (_stricmp(value, "prepend") == 0) {
            object->m_handlerFlags |= 0x800;
        } else if (_stricmp(value, "append") == 0) {
            object->m_handlerFlags |= 0x400;
        }
    }
    value = Attr(object, "intrinsicOrder");
    if (value && _stricmp(value, "postcall") == 0) {
        object->m_handlerFlags |= 0x1000;
    }
    value = Attr(object, "autoEnableInput");
    if (value) {
        bool b = StringToBOOL(value);
        object->m_handlerFlags &= ~0x200u;
        object->m_handlerFlags |= uint32_t(b) << 9;
    }
    object->m_size = SizeOf(object, builder);
    if (object->m_node->m_body != nullptr) {
        object->m_bodyFlags |= 1;
        XMLObjectAddString(object, builder, &object->m_body, object->m_node->m_body);
    }
    XMLParseHeader(object, builder);
    return true;
}

// A handler under <Scripts>: its type is the handler's index, mapped to an element type,
// and some handlers carry an argument kind.
// ref: FUN_14066c760
bool XMLParseScriptsChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    // The element type of each handler index, and the extra handler flag it carries.
    struct HANDLERTYPE {
        uint32_t m_type;
        uint32_t m_flags;
    };
    static const HANDLERTYPE s_types[FRAMESCRIPT_NUM_SCRIPTS] = {
        { 0x1d, 0 }, { 0x1e, 0 }, { 0x1f, 0 }, { 0x20, 0 }, { 0x36, 0 }, { 0x02, 0 }, { 0x03, 0 },
        { 0x04, 0 }, { 0x05, 0 }, { 0x37, 0 }, { 0x38, 0 }, { 0x38, 0 }, { 0x39, 0 }, { 0x3a, 0 },
        { 0x3b, 0x80 }, { 0x3c, 0x80 }, { 0x3d, 0x40 }, { 0x3e, 0x40 }, { 0x9e, 0x100 },
        { 0x3f, 0x40 }, { 0x40, 0 }, { 0x41, 0 }, { 0x42, 4 }, { 0x43, 0 }, { 0x44, 0 }, { 0x45, 0 },
        { 0x46, 0 }, { 0x47, 8 }, { 0x48, 8 }, { 0x49, 0x10 }, { 0x4a, 0x10 }, { 0x4b, 0x20 },
        { 0x4c, 0 }, { 0x4d, 0 }, { 0x4e, 0 }, { 0x99, 0 }, { 0x9a, 0 }, { 0x9b, 0 }, { 0x9c, 0 },
        { 0x9d, 0 }, { 0x9f, 0 }, { 0x2d, 0 }, { 0x2e, 0 }, { 0x2f, 0 }, { 0x30, 0 }, { 0x31, 0 },
        { 0x33, 0 }, { 0x32, 0 }, { 0x34, 0 }, { 0x1a, 0 }, { 0x1b, 0 }, { 0x1c, 0 }, { 0x07, 0 },
        { 0x08, 0 }, { 0x09, 0 }, { 0x0a, 0 }, { 0x0b, 0 }, { 0x0c, 0 }, { 0x0d, 0 }, { 0x0e, 0 },
        { 0x0f, 0 }, { 0x10, 0 }, { 0x11, 0 }, { 0x12, 0 }, { 0x13, 0 }, { 0x14, 0 }, { 0x15, 0 },
        { 0x16, 0 }, { 0x17, 0 }, { 0x18, 0 }, { 0x06, 0 }, { 0xc3, 0 }, { 0xc4, 0 }, { 0xc6, 0 },
        { 0xc7, 0 }, { 0xce, 0 }, { 0xcf, 0 }, { 0xd8, 0 }, { 0xda, 0 },
    };
    uint32_t index = FrameScript_GetScriptIndex(name);
    if (index >= FRAMESCRIPT_NUM_SCRIPTS) {
        return false;
    }
    auto handler = static_cast<XMLSCRIPTHANDLER*>(object);
    object->m_type = s_types[index].m_type;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLSCRIPTHANDLER);
    handler->m_handlerFlags |= s_types[index].m_flags;
    object->m_size = SizeOf(object, builder);
    if (s_types[index].m_flags != 0) {
        XMLParseScriptHandler(handler, builder);
        return true;
    }
    return XMLParseScriptHandler(handler, builder);
}

// ref: FUN_14066e560
bool XMLParseAnimation(XMLANIMATION* object, XMLBUILDER* builder) {
    object->m_flags |= 0x80000;
    const char* value = Attr(object, "target");
    if (value) {
        object->m_targetFlags |= 2;
        XMLObjectAddString(object, builder, &object->m_target, value);
    }
    value = Attr(object, "childKey");
    if (value) {
        object->m_targetFlags |= 4;
        XMLObjectAddString(object, builder, &object->m_childKey, value);
    }
    value = Attr(object, "targetKey");
    if (value) {
        object->m_targetFlags |= 8;
        XMLObjectAddString(object, builder, &object->m_targetKey, value);
    }
    value = Attr(object, "parentKey");
    if (value && *value) {
        object->m_animFlags |= 1;
        XMLObjectAddString(object, builder, &object->m_parentKey, value);
    }
    value = Attr(object, "startDelay");
    if (value && *value) {
        object->m_animFlags |= 2;
        object->m_startDelay = SStrToFloat(value);
    }
    value = Attr(object, "endDelay");
    if (value && *value) {
        object->m_animFlags |= 4;
        object->m_endDelay = SStrToFloat(value);
    }
    value = Attr(object, "duration");
    if (value && *value) {
        object->m_animFlags |= 8;
        object->m_duration = SStrToFloat(value);
    }
    value = Attr(object, "order");
    if (value && *value) {
        object->m_targetFlags |= 1;
        object->m_order = strtol(value, nullptr, IntRadix(value));
    }
    value = Attr(object, "smoothing");
    if (value && *value) {
        object->m_hasSmoothing = 1;
        XMLObjectAddString(object, builder, &object->m_smoothing, value);
    }
    object->m_size = SizeOf(object, builder);
    XMLParseHeader(object, builder);
    return XMLObjectParseChildren(object, [](XMLOBJECT* child, XMLBUILDER* b, const char* name) -> bool {
        // ref: FUN_14066a5a0
        if (strcmp(name, "scripts") == 0) {
            child->m_type = XMLTYPE_SCRIPTS;
            b->m_cursor = reinterpret_cast<char*>(child) + sizeof(XMLOBJECT);
            child->m_size = SizeOf(child, b);
            XMLParseHeader(child, b);
            return XMLObjectParseChildren(child, XMLParseScriptsChild, b);
        }
        if (strcmp(name, "origin") == 0) {
            auto origin = static_cast<XMLORIGIN*>(child);
            child->m_type = XMLTYPE_ORIGIN;
            b->m_cursor = reinterpret_cast<char*>(child) + sizeof(XMLORIGIN);
            origin->m_point = FRAMEPOINT_CENTER;
            const char* point = Attr(child, "point");
            if (point && StringToFramePoint(point, &origin->m_point)) {
                origin->m_originFlags |= 1;
            }
            child->m_size = SizeOf(child, b);
            XMLParseDimension(origin, b);
            return XMLObjectParseChildren(child, XMLParseDimensionChild, b);
        }
        if (strcmp(name, "keyvalues") == 0) {
            return XMLParseKeyValues(child, b);
        }
        return false;
    }, builder);
}

// ref: FUN_140671c20
bool XMLParseTranslation(XMLANIMATIONPAIR* object, XMLBUILDER* builder) {
    const char* value = Attr(object, "offsetX");
    if (value) {
        object->m_pairFlags |= 1;
        object->m_first = SStrToFloat(value);
    }
    value = Attr(object, "offsetY");
    if (value) {
        object->m_pairFlags |= 2;
        object->m_second = SStrToFloat(value);
    }
    object->m_size = SizeOf(object, builder);
    return XMLParseAnimation(object, builder);
}

// ref: FUN_1406713b0
bool XMLParseScale(XMLANIMATIONSCALE* object, XMLBUILDER* builder) {
    const char* const names[] = { "scaleX", "scaleY", "fromScaleX", "fromScaleY", "toScaleX", "toScaleY" };
    float* const fields[] = { &object->m_scaleX, &object->m_scaleY, &object->m_fromScaleX,
        &object->m_fromScaleY, &object->m_toScaleX, &object->m_toScaleY };
    for (uint32_t i = 0; i < 6; i++) {
        const char* value = Attr(object, names[i]);
        if (value) {
            object->m_scaleFlags |= 1u << i;
            *fields[i] = SStrToFloat(value);
        }
    }
    object->m_size = SizeOf(object, builder);
    return XMLParseAnimation(object, builder);
}

// ref: FUN_14066f540
bool XMLParseFlipBook(XMLANIMATIONFLIPBOOK* object, XMLBUILDER* builder) {
    const char* const names[] = { "flipBookRows", "flipBookColumns", "flipBookFrames", "flipBookFrameWidth",
        "flipBookFrameHeight" };
    uint32_t* const fields[] = { &object->m_rows, &object->m_columns, &object->m_frames, &object->m_frameWidth,
        &object->m_frameHeight };
    for (uint32_t i = 0; i < 5; i++) {
        const char* value = Attr(object, names[i]);
        if (value) {
            object->m_flipBookFlags |= 1u << i;
            *fields[i] = strtoul(value, nullptr, IntRadix(value));
        }
    }
    object->m_size = SizeOf(object, builder);
    return XMLParseAnimation(object, builder);
}

// <ControlPoint> under <ControlPoints>.
// ref: FUN_14066b090
bool XMLParseControlPointsChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "controlpoint") != 0) {
        return false;
    }
    auto point = static_cast<XMLCONTROLPOINT*>(object);
    object->m_type = XMLTYPE_CONTROLPOINT;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLCONTROLPOINT);
    const char* value = Attr(object, "parentKey");
    if (value && *value) {
        point->m_pointFlags |= 4;
        XMLObjectAddString(object, builder, &point->m_parentKey, value);
    }
    value = Attr(object, "offsetX");
    if (value) {
        point->m_pointFlags |= 1;
        point->m_offsetX = SStrToFloat(value);
    }
    value = Attr(object, "offsetY");
    if (value) {
        point->m_pointFlags |= 2;
        point->m_offsetY = SStrToFloat(value);
    }
    object->m_size = SizeOf(object, builder);
    return XMLParseHeader(object, builder);
}

// <ControlPoints> under a <Path>.
// ref: FUN_14066c490
bool XMLParsePathChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "controlpoints") != 0) {
        return false;
    }
    object->m_type = XMLTYPE_CONTROLPOINTS;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLOBJECT);
    object->m_size = SizeOf(object, builder);
    XMLParseHeader(object, builder);
    return XMLObjectParseChildren(object, XMLParseControlPointsChild, builder);
}

// What an <AnimationGroup> holds.
// ref: FUN_14066a710
bool XMLParseAnimationGroupChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    char* base = reinterpret_cast<char*>(object);
    if (strcmp(name, "animation") == 0) {
        object->m_type = XMLTYPE_ANIMATION;
        builder->m_cursor = base + sizeof(XMLANIMATION);
        return XMLParseAnimation(static_cast<XMLANIMATION*>(object), builder);
    }
    if (strcmp(name, "scripts") == 0) {
        object->m_type = XMLTYPE_SCRIPTS;
        builder->m_cursor = base + sizeof(XMLOBJECT);
        object->m_size = SizeOf(object, builder);
        XMLParseHeader(object, builder);
        return XMLObjectParseChildren(object, XMLParseScriptsChild, builder);
    }
    auto pair = static_cast<XMLANIMATIONPAIR*>(object);
    const char* second;
    if (strcmp(name, "alpha") == 0) {
        object->m_type = XMLTYPE_ALPHA;
        builder->m_cursor = base + sizeof(XMLANIMATIONPAIR);
        const char* value = Attr(object, "fromAlpha");
        if (value) {
            pair->m_pairFlags |= 1;
            pair->m_first = SStrToFloat(value);
        }
        second = "toAlpha";
    } else if (strcmp(name, "translation") == 0) {
        object->m_type = XMLTYPE_TRANSLATION;
        builder->m_cursor = base + sizeof(XMLANIMATIONPAIR);
        return XMLParseTranslation(pair, builder);
    } else if (strcmp(name, "linetranslation") == 0) {
        object->m_type = XMLTYPE_LINETRANSLATION;
        builder->m_cursor = base + sizeof(XMLANIMATIONPAIR);
        object->m_size = SizeOf(object, builder);
        return XMLParseTranslation(pair, builder);
    } else if (strcmp(name, "texturecoordtranslation") == 0) {
        object->m_type = XMLTYPE_TEXCOORDTRANSLATION;
        builder->m_cursor = base + sizeof(XMLANIMATIONPAIR);
        const char* value = Attr(object, "offsetU");
        if (value) {
            pair->m_pairFlags |= 1;
            pair->m_first = SStrToFloat(value);
        }
        second = "offsetV";
    } else if (strcmp(name, "rotation") == 0) {
        object->m_type = XMLTYPE_ROTATION;
        builder->m_cursor = base + sizeof(XMLANIMATIONPAIR);
        const char* value = Attr(object, "degrees");
        if (value) {
            pair->m_pairFlags |= 1;
            pair->m_first = SStrToFloat(value);
        }
        second = "radians";
    } else if (strcmp(name, "scale") == 0) {
        object->m_type = XMLTYPE_SCALE;
        builder->m_cursor = base + sizeof(XMLANIMATIONSCALE);
        return XMLParseScale(static_cast<XMLANIMATIONSCALE*>(object), builder);
    } else if (strcmp(name, "linescale") == 0) {
        object->m_type = XMLTYPE_LINESCALE;
        builder->m_cursor = base + sizeof(XMLANIMATIONSCALE);
        object->m_size = SizeOf(object, builder);
        return XMLParseScale(static_cast<XMLANIMATIONSCALE*>(object), builder);
    } else if (strcmp(name, "path") == 0) {
        auto path = static_cast<XMLANIMATIONPATH*>(object);
        object->m_type = XMLTYPE_PATH;
        builder->m_cursor = base + sizeof(XMLANIMATIONPATH);
        const char* value = Attr(object, "curve");
        if (value && *value) {
            path->m_pathFlags |= 1;
            XMLObjectAddString(object, builder, &path->m_curve, value);
        }
        object->m_size = SizeOf(object, builder);
        if (!XMLParseAnimation(path, builder) || !XMLObjectParseChildren(object, XMLParsePathChild, builder)) {
            return false;
        }
        return true;
    } else if (strcmp(name, "flipbook") == 0) {
        object->m_type = XMLTYPE_FLIPBOOK;
        builder->m_cursor = base + sizeof(XMLANIMATIONFLIPBOOK);
        return XMLParseFlipBook(static_cast<XMLANIMATIONFLIPBOOK*>(object), builder);
    } else if (strcmp(name, "keyvalues") == 0) {
        return XMLParseKeyValues(object, builder);
    } else {
        return false;
    }
    const char* value = Attr(object, second);
    if (value) {
        pair->m_pairFlags |= 2;
        pair->m_second = SStrToFloat(value);
    }
    object->m_size = SizeOf(object, builder);
    return XMLParseAnimation(pair, builder);
}

// ref: FUN_140668b40
bool XMLParseAnimationGroup(XMLANIMATIONGROUP* object, XMLBUILDER* builder) {
    object->m_type = XMLTYPE_ANIMATIONGROUP;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLANIMATIONGROUP);
    const char* value = Attr(object, "parentKey");
    if (value && *value) {
        object->m_groupFlags |= 1;
        XMLObjectAddString(object, builder, &object->m_parentKey, value);
    }
    value = Attr(object, "parentArray");
    if (value && *value) {
        object->m_groupFlags |= 2;
        XMLObjectAddString(object, builder, &object->m_parentArray, value);
    }
    value = Attr(object, "looping");
    if (value && *value) {
        object->m_groupFlags |= 4;
        XMLObjectAddString(object, builder, &object->m_looping, value);
    }
    value = Attr(object, "setToFinalAlpha");
    if (value) {
        SetBoolFlag(&object->m_groupFlags, 8, 4, value);
    }
    value = Attr(object, "animationSpeedMultiplier");
    if (value) {
        object->m_groupFlags |= 0x20;
        object->m_speedMultiplier = SStrToFloat(value);
    }
    object->m_size = SizeOf(object, builder);
    XMLParseHeader(object, builder);
    return XMLObjectParseChildren(object, XMLParseAnimationGroupChild, builder);
}

// ref: FUN_140668cb0
bool XMLParseAnimations(XMLOBJECT* object, XMLBUILDER* builder) {
    object->m_type = XMLTYPE_ANIMATIONS;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLOBJECT);
    object->m_size = SizeOf(object, builder);
    XMLParseHeader(object, builder);
    return XMLObjectParseChildren(object, [](XMLOBJECT* child, XMLBUILDER* b, const char* name) -> bool {
        // ref: FUN_14066ab60
        if (strcmp(name, "animationgroup") == 0) {
            return XMLParseAnimationGroup(static_cast<XMLANIMATIONGROUP*>(child), b);
        }
        return false;
    }, builder);
}

// <Scripts>, <KeyValues>, <Animations> under an <Actor>.
// ref: FUN_14066c320
bool XMLParseActorChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "scripts") == 0) {
        object->m_type = XMLTYPE_SCRIPTS;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLOBJECT);
        object->m_size = SizeOf(object, builder);
        XMLParseHeader(object, builder);
        return XMLObjectParseChildren(object, XMLParseScriptsChild, builder);
    }
    if (strcmp(name, "keyvalues") == 0) {
        object->m_type = XMLTYPE_KEYVALUES;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLOBJECT);
        object->m_size = SizeOf(object, builder);
        XMLParseHeader(object, builder);
        return XMLObjectParseChildren(object, XMLParseKeyValuesChild, builder);
    }
    if (strcmp(name, "animations") == 0) {
        return XMLParseAnimations(object, builder);
    }
    return false;
}

// ref: FUN_1406709b0
bool XMLParseActor(XMLACTOR* object, XMLBUILDER* builder) {
    object->m_flags |= 0x8000;
    const char* value = Attr(object, "parentKey");
    if (value && *value) {
        object->m_actorFlags |= 1;
        XMLObjectAddString(object, builder, &object->m_parentKey, value);
    }
    value = Attr(object, "parentArray");
    if (value && *value) {
        object->m_actorFlags |= 2;
        XMLObjectAddString(object, builder, &object->m_parentArray, value);
    }
    value = Attr(object, "file");
    if (value) {
        object->m_actorFlags |= 4;
        XMLObjectAddString(object, builder, &object->m_file, value);
    }
    value = Attr(object, "fileID");
    if (value) {
        object->m_actorFlags |= 8;
        object->m_fileID = strtol(value, nullptr, IntRadix(value));
    }
    value = Attr(object, "scale");
    if (value) {
        object->m_actorFlags |= 0x10;
        object->m_scale = SStrToFloat(value);
    }
    value = Attr(object, "alpha");
    if (value) {
        object->m_actorFlags |= 0x20;
        object->m_alpha = SStrToFloat(value);
    }
    value = Attr(object, "desaturation");
    if (value) {
        object->m_actorFlags |= 0x40;
        object->m_desaturation = SStrToFloat(value);
    }
    object->m_size = SizeOf(object, builder);
    XMLParseHeader(object, builder);
    return XMLObjectParseChildren(object, XMLParseActorChild, builder);
}

// What a <Frames> or <ScrollChild> holds: frames of any kind.
// LAB_14066ba20
bool XMLParseFramesChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    object->m_type = XMLTYPE_FRAMEDEF;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLREGION);
    return XMLParseFrameType(object, builder, name);
}

// <ResizeBounds>, <HitRectInsets>, <Layers>, <Attributes>, <Scripts>, <Frames> under a frame.
// ref: FUN_14066b7e0
bool XMLParseFrameChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "resizebounds") == 0) {
        object->m_type = XMLTYPE_RESIZEBOUNDS;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLOBJECT);
        object->m_size = SizeOf(object, builder);
        XMLParseHeader(object, builder);
        return XMLObjectParseChildren(object, [](XMLOBJECT* child, XMLBUILDER* b, const char* n) -> bool {
            // ref: FUN_14066c690
            if (strcmp(n, "minresize") == 0) {
                child->m_type = XMLTYPE_MINRESIZE;
            } else if (strcmp(n, "maxresize") == 0) {
                child->m_type = XMLTYPE_MAXRESIZE;
            } else {
                return false;
            }
            b->m_cursor = reinterpret_cast<char*>(child) + sizeof(XMLDIMENSION);
            child->m_size = SizeOf(child, b);
            XMLParseDimension(static_cast<XMLDIMENSION*>(child), b);
            return XMLObjectParseChildren(child, XMLParseDimensionChild, b);
        }, builder);
    }
    if (strcmp(name, "hitrectinsets") == 0) {
        object->m_type = XMLTYPE_HITRECTINSETS;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLINSET);
        object->m_size = SizeOf(object, builder);
        XMLParseInset(static_cast<XMLINSET*>(object), builder);
        return XMLObjectParseChildren(object, XMLParseInsetChild, builder);
    }
    if (strcmp(name, "layers") == 0) {
        object->m_type = XMLTYPE_LAYERS;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLOBJECT);
        object->m_size = SizeOf(object, builder);
        XMLParseHeader(object, builder);
        return XMLObjectParseChildren(object, XMLParseLayersChild, builder);
    }
    if (strcmp(name, "attributes") == 0) {
        object->m_type = XMLTYPE_ATTRIBUTES;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLOBJECT);
        object->m_size = SizeOf(object, builder);
        XMLParseHeader(object, builder);
        return XMLObjectParseChildren(object, XMLParseAttributesChild, builder);
    }
    if (strcmp(name, "scripts") == 0) {
        return XMLParseScripts(object, builder);
    }
    if (strcmp(name, "frames") == 0) {
        object->m_type = XMLTYPE_FRAMES;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLOBJECT);
        object->m_size = SizeOf(object, builder);
        XMLParseHeader(object, builder);
        return XMLObjectParseChildren(object, XMLParseFramesChild, builder);
    }
    return false;
}

// ref: FUN_14066fcc0
bool XMLParseFrame(XMLFRAME* object, XMLBUILDER* builder) {
    object->m_flags |= 0x100;
    uint32_t* flags = &object->m_frameFlags;
    uint32_t* flags2 = &object->m_frameFlags2;
    const char* value = Attr(object, "parent");
    if (value && *value) {
        *flags |= 0x40000;
        XMLObjectAddString(object, builder, &object->m_parent, value);
    }
    object->m_id = -1;
    value = Attr(object, "id");
    if (value) {
        object->m_id = strtol(value, nullptr, IntRadix(value));
        *flags |= 0x80000;
    }
    value = Attr(object, "drawLayer");
    if (value && *value && StringToDrawLayer(value, &object->m_drawLayer)) {
        *flags |= 0x100000;
    }
    value = Attr(object, "toplevel");
    if (value && *value) {
        SetBoolFlag(flags, 0x100, 9, value);
    }
    value = Attr(object, "useParentLevel");
    if (value && *value) {
        SetBoolFlag(flags, 0x200000, 22, value);
    }
    value = Attr(object, "movable");
    if (value && *value) {
        SetBoolFlag(flags, 0x400, 11, value);
    }
    value = Attr(object, "resizable");
    if (value && *value) {
        SetBoolFlag(flags, 0x1000, 13, value);
    }
    value = Attr(object, "frameStrata");
    if (value && StringToFrameStrata(value, &object->m_strata)) {
        *flags |= 0x4000;
    }
    value = Attr(object, "frameLevel");
    if (value) {
        *flags |= 1;
        object->m_frameLevel = int16_t(strtol(value, nullptr, IntRadix(value)));
    }
    value = Attr(object, "fixedFrameStrata");
    if (value && *value) {
        SetBoolFlag(flags2, 0x80, 8, value);
    }
    value = Attr(object, "fixedFrameLevel");
    if (value && *value) {
        SetBoolFlag(flags2, 0x200, 10, value);
    }
    value = Attr(object, "enableKeyboard");
    if (value && *value) {
        SetBoolFlag(flags, 0x8000, 16, value);
    }
    value = Attr(object, "clampedToScreen");
    if (value && *value) {
        SetBoolFlag(flags, 2, 2, value);
    }
    value = Attr(object, "jumpNavigateEnabled");
    if (value && *value) {
        SetBoolFlag(flags, 8, 4, value);
    }
    value = Attr(object, "jumpNavigateStart");
    if (value && *value) {
        SetBoolFlag(flags, 0x20, 6, value);
    }
    value = Attr(object, "depth");
    if (value) {
        *flags |= 0x80;
        object->m_depth = SStrToFloat(value);
    }
    value = Attr(object, "dontSavePosition");
    if (value && *value) {
        SetBoolFlag(flags, 0x800000, 24, value);
    }
    value = Attr(object, "propagateKeyboardInput");
    if (value && *value) {
        SetBoolFlag(flags, 0x2000000, 26, value);
    }
    value = Attr(object, "ignoreParentAlpha");
    if (value && *value) {
        SetBoolFlag(flags, 0x8000000, 28, value);
    }
    value = Attr(object, "ignoreParentScale");
    if (value && *value) {
        SetBoolFlag(flags2, 0x20, 6, value);
    }
    value = Attr(object, "flattenRenderLayers");
    if (value && *value) {
        SetBoolFlag(flags, 0x20000000, 30, value);
    }
    value = Attr(object, "clipChildren");
    if (value && *value) {
        *flags |= 0x80000000;
        bool b = StringToBOOL(value);
        *flags2 = (*flags2 & ~1u) | uint32_t(b);
    }
    value = Attr(object, "hyperlinksEnabled");
    if (value && *value) {
        SetBoolFlag(flags2, 2, 2, value);
    }
    value = Attr(object, "propagateHyperlinksToParent");
    if (value && *value) {
        SetBoolFlag(flags2, 8, 4, value);
    }
    value = Attr(object, "frameBuffer");
    if (value && *value) {
        SetBoolFlag(flags2, 0x800, 12, value);
    }
    object->m_size = SizeOf(object, builder);
    XMLParseRegion(object, builder);
    return XMLObjectParseChildren(object, XMLParseFrameChild, builder);
}

// <FontStringHeader1..3> and <FontString> under a frame that lays out text.
// ref: FUN_14066b700
bool XMLParseFontStringChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "fontstringheader1") == 0) {
        object->m_type = XMLTYPE_FONTSTRINGHEADER1;
    } else if (strcmp(name, "fontstringheader2") == 0) {
        object->m_type = XMLTYPE_FONTSTRINGHEADER2;
    } else if (strcmp(name, "fontstringheader3") == 0) {
        object->m_type = XMLTYPE_FONTSTRINGHEADER3;
    } else if (strcmp(name, "fontstring") == 0) {
        object->m_type = XMLTYPE_FONTSTRING;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLFONTSTRING);
        return XMLParseFontString(static_cast<XMLFONTSTRING*>(object), builder);
    } else {
        return false;
    }
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLFONTSTRING);
    object->m_size = SizeOf(object, builder);
    return XMLParseFontString(static_cast<XMLFONTSTRING*>(object), builder);
}

// ref: FUN_14066fa50
bool XMLParseButtonFont(XMLFILE* object, XMLBUILDER* builder) {
    const char* value = Attr(object, "style");
    if (value && *value) {
        object->m_fileFlags |= 1;
        XMLObjectAddString(object, builder, &object->m_file, value);
    }
    object->m_size = SizeOf(object, builder);
    XMLParseHeader(object, builder);
    return true;
}

// The textures, fonts, colours and text of a button.
// ref: FUN_14066ac90
bool XMLParseButtonChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    char* base = reinterpret_cast<char*>(object);
    if (strcmp(name, "normaltexture") == 0) {
        object->m_type = XMLTYPE_NORMALTEXTURE;
    } else if (strcmp(name, "pushedtexture") == 0) {
        object->m_type = XMLTYPE_PUSHEDTEXTURE;
    } else if (strcmp(name, "disabledtexture") == 0) {
        object->m_type = XMLTYPE_DISABLEDTEXTURE;
    } else if (strcmp(name, "highlighttexture") == 0) {
        object->m_type = XMLTYPE_HIGHLIGHTTEXTURE;
    } else if (strcmp(name, "buttontext") == 0) {
        object->m_type = XMLTYPE_BUTTONTEXT;
        builder->m_cursor = base + sizeof(XMLFONTSTRING);
        object->m_size = SizeOf(object, builder);
        return XMLParseFontString(static_cast<XMLFONTSTRING*>(object), builder);
    } else {
        if (strcmp(name, "disabledfont") == 0) {
            object->m_type = XMLTYPE_DISABLEDFONT;
        } else if (strcmp(name, "normalfont") == 0) {
            object->m_type = XMLTYPE_NORMALFONT;
        } else if (strcmp(name, "highlightfont") == 0) {
            object->m_type = XMLTYPE_HIGHLIGHTFONT;
        } else {
            if (strcmp(name, "normalcolor") == 0) {
                object->m_type = XMLTYPE_NORMALCOLOR;
            } else if (strcmp(name, "disabledcolor") == 0) {
                object->m_type = XMLTYPE_DISABLEDCOLOR;
            } else if (strcmp(name, "highlightcolor") == 0) {
                object->m_type = XMLTYPE_HIGHLIGHTCOLOR;
            } else if (strcmp(name, "pushedtextoffset") == 0) {
                object->m_type = XMLTYPE_PUSHEDTEXTOFFSET;
                builder->m_cursor = base + sizeof(XMLDIMENSION);
                object->m_size = SizeOf(object, builder);
                XMLParseDimension(static_cast<XMLDIMENSION*>(object), builder);
                return XMLObjectParseChildren(object, XMLParseDimensionChild, builder);
            } else {
                return false;
            }
            builder->m_cursor = base + sizeof(XMLCOLOR);
            object->m_size = SizeOf(object, builder);
            return XMLParseColor(static_cast<XMLCOLOR*>(object), builder);
        }
        builder->m_cursor = base + sizeof(XMLFILE);
        object->m_size = SizeOf(object, builder);
        return XMLParseButtonFont(static_cast<XMLFILE*>(object), builder);
    }
    builder->m_cursor = base + sizeof(XMLTEXTURE);
    object->m_size = SizeOf(object, builder);
    return XMLParseTexture(static_cast<XMLTEXTURE*>(object), builder);
}

// ref: FUN_14066e990
bool XMLParseButton(XMLBUTTON* object, XMLBUILDER* builder) {
    object->m_flags |= 0x80;
    const char* value = Attr(object, "text");
    if (value && *value) {
        object->m_buttonFlags |= 1;
        XMLObjectAddString(object, builder, &object->m_text, value);
    }
    value = Attr(object, "registerForClicks");
    if (value && *value) {
        object->m_buttonFlags |= 2;
        XMLObjectAddString(object, builder, &object->m_registerForClicks, value);
    }
    value = Attr(object, "motionScriptsWhileDisabled");
    if (value) {
        SetBoolFlag(&object->m_buttonFlags, 4, 3, value);
    }
    object->m_size = SizeOf(object, builder);
    XMLParseFrame(object, builder);
    return XMLObjectParseChildren(object, XMLParseButtonChild, builder);
}

// ref: FUN_14066e7e0
bool XMLParseRange(XMLRANGE* object, XMLBUILDER* builder) {
    const char* value = Attr(object, "minValue");
    if (value && *value) {
        object->m_rangeFlags |= 2;
        object->m_minValue = SStrToFloat(value);
    }
    value = Attr(object, "maxValue");
    if (value && *value) {
        object->m_rangeFlags |= 4;
        object->m_maxValue = SStrToFloat(value);
    }
    value = Attr(object, "defaultValue");
    if (value && *value) {
        object->m_rangeFlags |= 8;
        object->m_defaultValue = SStrToFloat(value);
    }
    value = Attr(object, "orientation");
    if (value && *value && StringToOrientation(value, &object->m_orientation)) {
        object->m_rangeFlags |= 1;
    }
    object->m_size = SizeOf(object, builder);
    return XMLParseFrame(object, builder);
}

// ref: FUN_14066e8e0
bool XMLParseBlobFrame(XMLBLOBFRAME* object, XMLBUILDER* builder) {
    object->m_flags |= 0x2000000;
    const char* value = Attr(object, "filltexture");
    if (value && *value) {
        object->m_blobFlags |= 1;
        XMLObjectAddString(object, builder, &object->m_fillTexture, value);
    }
    value = Attr(object, "bordertexture");
    if (value && *value) {
        object->m_blobFlags |= 2;
        XMLObjectAddString(object, builder, &object->m_borderTexture, value);
    }
    object->m_size = SizeOf(object, builder);
    XMLParseFrame(object, builder);
    return true;
}

// ref: FUN_14066ef50
bool XMLParseCooldownTexture(XMLFILE* object, XMLBUILDER* builder) {
    const char* value = Attr(object, "file");
    if (value) {
        object->m_fileFlags |= 1;
        XMLObjectAddString(object, builder, &object->m_file, value);
    }
    object->m_size = SizeOf(object, builder);
    XMLParseHeader(object, builder);
    return XMLObjectParseChildren(object, XMLParseColorChild, builder);
}

// ref: FUN_14066ed30
bool XMLParseCooldown(XMLCOOLDOWN* object, XMLBUILDER* builder) {
    object->m_flags |= 0x40000000;
    uint32_t* flags = &object->m_cooldownFlags;
    *flags &= ~0x200u;
    const char* value = Attr(object, "reverse");
    if (value && *value) {
        SetBoolFlag(flags, 1, 1, value);
    }
    value = Attr(object, "drawSwipe");
    if (value && *value) {
        SetBoolFlag(flags, 4, 3, value);
    }
    value = Attr(object, "drawEdge");
    if (value && *value) {
        SetBoolFlag(flags, 0x10, 5, value);
    }
    value = Attr(object, "drawBling");
    if (value && *value) {
        SetBoolFlag(flags, 0x40, 7, value);
    }
    value = Attr(object, "hideCountdownNumbers");
    if (value && *value) {
        SetBoolFlag(flags, 0x100, 9, value);
    }
    value = Attr(object, "useCircularEdge");
    if (value && *value) {
        SetBoolFlag(flags, 0x400, 11, value);
    }
    value = Attr(object, "rotation");
    if (value && *value) {
        *flags |= 0x1000;
        object->m_rotation = SStrToFloat(value) * DEG_TO_RAD;
    }
    object->m_size = SizeOf(object, builder);
    XMLParseFrame(object, builder);
    return XMLObjectParseChildren(object, [](XMLOBJECT* child, XMLBUILDER* b, const char* name) -> bool {
        // ref: FUN_14066b1c0
        if (strcmp(name, "swipetexture") == 0) {
            child->m_type = XMLTYPE_SWIPETEXTURE;
        } else if (strcmp(name, "edgetexture") == 0) {
            child->m_type = XMLTYPE_EDGETEXTURE;
        } else if (strcmp(name, "blingtexture") == 0) {
            child->m_type = XMLTYPE_BLINGTEXTURE;
        } else {
            return false;
        }
        b->m_cursor = reinterpret_cast<char*>(child) + sizeof(XMLFILE);
        child->m_size = SizeOf(child, b);
        return XMLParseCooldownTexture(static_cast<XMLFILE*>(child), b);
    }, builder);
}

// <TextInsets>, <HighlightColor>, <FontString> under an edit box.
// ref: FUN_14066b380
bool XMLParseEditBoxChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "textinsets") == 0) {
        object->m_type = XMLTYPE_TEXTINSETS;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLINSET);
        object->m_size = SizeOf(object, builder);
        XMLParseInset(static_cast<XMLINSET*>(object), builder);
        return XMLObjectParseChildren(object, XMLParseInsetChild, builder);
    }
    if (strcmp(name, "highlightcolor") == 0) {
        object->m_type = XMLTYPE_HIGHLIGHTCOLOR;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLCOLOR);
        object->m_size = SizeOf(object, builder);
        return XMLParseColor(static_cast<XMLCOLOR*>(object), builder);
    }
    if (strcmp(name, "fontstring") == 0) {
        object->m_type = XMLTYPE_FONTSTRING;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLFONTSTRING);
        return XMLParseFontString(static_cast<XMLFONTSTRING*>(object), builder);
    }
    return false;
}

// ref: FUN_14066efd0
bool XMLParseEditBox(XMLEDITBOX* object, XMLBUILDER* builder) {
    object->m_flags |= 0x800;
    uint32_t* flags = &object->m_editFlags;
    const char* value = Attr(object, "bytes");
    if (value) {
        *flags |= 1;
        object->m_bytes = strtol(value, nullptr, IntRadix(value));
    }
    value = Attr(object, "visibleBytes");
    if (value) {
        *flags |= 2;
        object->m_visibleBytes = strtol(value, nullptr, IntRadix(value));
    }
    value = Attr(object, "letters");
    if (value) {
        *flags |= 4;
        object->m_letters = strtol(value, nullptr, IntRadix(value));
    }
    value = Attr(object, "countInvisibleLetters");
    if (value && *value) {
        SetBoolFlag(flags, 8, 5, value);
    }
    value = Attr(object, "invisibleBytes");
    if (value && *value) {
        *flags |= 0x10;
        object->m_invisibleBytes = strtol(value, nullptr, IntRadix(value));
    }
    value = Attr(object, "blinkSpeed");
    if (value && *value) {
        *flags |= 0x40;
        float speed = 0.0f;
        if (0.0f < SStrToFloat(value)) {
            speed = SStrToFloat(value);
        }
        object->m_blinkSpeed = speed;
    }
    value = Attr(object, "autoFocus");
    if (value && *value) {
        SetBoolFlag(flags, 0x80, 8, value);
    }
    value = Attr(object, "multiLine");
    if (value && *value) {
        SetBoolFlag(flags, 0x200, 10, value);
    }
    value = Attr(object, "numeric");
    if (value && *value) {
        SetBoolFlag(flags, 0x800, 12, value);
    }
    value = Attr(object, "numericfullrange");
    if (value && *value) {
        SetBoolFlag(flags, 0x2000, 14, value);
    }
    value = Attr(object, "alphabeticOnly");
    if (value && *value) {
        SetBoolFlag(flags, 0x8000, 16, value);
    }
    value = Attr(object, "password");
    if (value && *value) {
        SetBoolFlag(flags, 0x20000, 18, value);
    }
    value = Attr(object, "ignoreArrows");
    if (value && *value) {
        SetBoolFlag(flags, 0x80000, 20, value);
    }
    value = Attr(object, "historyLines");
    if (value) {
        *flags |= 0x200000;
        int32_t lines = strtol(value, nullptr, IntRadix(value));
        if (lines < 1) {
            lines = 0;
        } else {
            lines = strtol(value, nullptr, IntRadix(value));
        }
        object->m_historyLines = lines;
    }
    value = Attr(object, "securityDisableSetText");
    if (value) {
        SetBoolFlag(flags, 0x800000, 22, value);
    }
    value = Attr(object, "securityDisablePaste");
    if (value) {
        SetBoolFlag(flags, 0x2000000, 24, value);
    }
    object->m_size = SizeOf(object, builder);
    XMLParseFrame(object, builder);
    // LAB_14066b6f0
    XMLObjectParseChildren(object, XMLParseFontStringChild, builder);
    return XMLObjectParseChildren(object, XMLParseEditBoxChild, builder);
}

// <FogColor>, <ViewInsets> under a model.
// ref: FUN_14066c170
bool XMLParseModelChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "fogcolor") == 0) {
        object->m_type = XMLTYPE_FOGCOLOR;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLCOLOR);
        object->m_size = SizeOf(object, builder);
        return XMLParseColor(static_cast<XMLCOLOR*>(object), builder);
    }
    if (strcmp(name, "viewinsets") == 0) {
        return XMLParseViewInsets(static_cast<XMLINSET*>(object), builder);
    }
    return false;
}

// ref: FUN_140670880
bool XMLParseModel(XMLMODEL* object, XMLBUILDER* builder) {
    object->m_flags |= 0x2000;
    const char* value = Attr(object, "file");
    if (value) {
        object->m_modelFlags |= 1;
        XMLObjectAddString(object, builder, &object->m_file, value);
    }
    value = Attr(object, "modelScale");
    if (value) {
        object->m_modelFlags |= 2;
        object->m_modelScale = SStrToFloat(value);
    }
    value = Attr(object, "fogNear");
    if (value) {
        object->m_modelFlags |= 4;
        object->m_fogNear = SStrToFloat(value);
    }
    value = Attr(object, "fogFar");
    if (value) {
        object->m_modelFlags |= 8;
        object->m_fogFar = SStrToFloat(value);
    }
    object->m_size = SizeOf(object, builder);
    XMLParseFrame(object, builder);
    return XMLObjectParseChildren(object, XMLParseModelChild, builder);
}

// <FogColor>, <Actors>, <ViewInsets> under a model scene.
// ref: FUN_14066c210
bool XMLParseModelSceneChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "fogcolor") == 0) {
        object->m_type = XMLTYPE_FOGCOLOR;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLCOLOR);
        object->m_size = SizeOf(object, builder);
        return XMLParseColor(static_cast<XMLCOLOR*>(object), builder);
    }
    if (strcmp(name, "actors") == 0) {
        object->m_type = XMLTYPE_ACTORS;
        builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLOBJECT);
        object->m_size = SizeOf(object, builder);
        XMLParseHeader(object, builder);
        return XMLObjectParseChildren(object, [](XMLOBJECT* child, XMLBUILDER* b, const char* n) -> bool {
            // LAB_14066c440
            if (strcmp(n, "actor") != 0) {
                return false;
            }
            child->m_type = XMLTYPE_ACTOR;
            b->m_cursor = reinterpret_cast<char*>(child) + sizeof(XMLACTOR);
            return XMLParseActor(static_cast<XMLACTOR*>(child), b);
        }, builder);
    }
    if (strcmp(name, "viewinsets") == 0) {
        return XMLParseViewInsets(static_cast<XMLINSET*>(object), builder);
    }
    return false;
}

// ref: FUN_1406690c0
bool XMLParseModelScene(XMLMODELSCENE* object, XMLBUILDER* builder) {
    object->m_frameType = XMLTYPE_MODELSCENE;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLMODELSCENE);
    object->m_flags |= 0x4000;
    const char* value = Attr(object, "fogNear");
    if (value) {
        object->m_sceneFlags |= 1;
        object->m_fogNear = SStrToFloat(value);
    }
    value = Attr(object, "fogFar");
    if (value) {
        object->m_sceneFlags |= 2;
        object->m_fogFar = SStrToFloat(value);
    }
    object->m_size = SizeOf(object, builder);
    XMLParseFrame(object, builder);
    return XMLObjectParseChildren(object, XMLParseModelSceneChild, builder);
}

// ref: FUN_140668f40
bool XMLParseMessageFrame(XMLMESSAGEFRAME* object, XMLBUILDER* builder) {
    object->m_frameType = XMLTYPE_MESSAGEFRAME;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLMESSAGEFRAME);
    object->m_flags |= 0x10000;
    const char* value = Attr(object, "fade");
    if (value && *value) {
        SetBoolFlag(&object->m_messageFlags, 1, 1, value);
    }
    value = Attr(object, "displayDuration");
    if (value && *value) {
        object->m_messageFlags |= 4;
        object->m_displayDuration = SStrToFloat(value);
    }
    value = Attr(object, "fadeDuration");
    if (value && *value) {
        object->m_messageFlags |= 8;
        object->m_fadeDuration = SStrToFloat(value);
    }
    value = Attr(object, "fadePower");
    if (value && *value) {
        object->m_messageFlags |= 0x10;
        object->m_fadePower = SStrToFloat(value);
    }
    value = Attr(object, "insertMode");
    if (value && *value) {
        object->m_insertMode = _stricmp(value, "BOTTOM") == 0;
        object->m_messageFlags |= 0x20;
    }
    object->m_size = SizeOf(object, builder);
    XMLParseFrame(object, builder);
    // LAB_14066b6f0
    return XMLObjectParseChildren(object, XMLParseFontStringChild, builder);
}

// ref: FUN_140669240
bool XMLParseSimpleHTML(XMLSIMPLEHTML* object, XMLBUILDER* builder) {
    object->m_frameType = XMLTYPE_SIMPLEHTML;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLSIMPLEHTML);
    object->m_flags |= 0x40000;
    const char* value = Attr(object, "file");
    if (value) {
        object->m_htmlFlags |= 4;
        XMLObjectAddString(object, builder, &object->m_file, value);
    }
    value = Attr(object, "hyperlinkFormat");
    if (value) {
        object->m_htmlFlags |= 8;
        XMLObjectAddString(object, builder, &object->m_hyperlinkFormat, value);
    }
    value = Attr(object, "resizeToFitContents");
    if (value) {
        SetBoolFlag(&object->m_htmlFlags, 1, 1, value);
    }
    object->m_size = SizeOf(object, builder);
    XMLParseFrame(object, builder);
    // LAB_14066b6f0
    return XMLObjectParseChildren(object, XMLParseFontStringChild, builder);
}

// Browser (0x80000000 in the parser flags) and Checkout (4 in the attribute flags).
// ref: FUN_140668d00
bool XMLParseBrowser(XMLBROWSER* object, XMLBUILDER* builder) {
    object->m_frameType = XMLTYPE_BROWSER;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLBROWSER);
    object->m_flags |= 0x80000000;
    const char* value = Attr(object, "IMEFont");
    if (value && *value) {
        object->m_browserFlags |= 1;
        XMLObjectAddString(object, builder, &object->m_imeFont, value);
    }
    object->m_size = SizeOf(object, builder);
    XMLParseFrame(object, builder);
    return XMLObjectParseChildren(object, [](XMLOBJECT*, XMLBUILDER*, const char*) { return false; }, builder);
}

// ref: FUN_140668da0
bool XMLParseCheckout(XMLBROWSER* object, XMLBUILDER* builder) {
    object->m_frameType = XMLTYPE_CHECKOUT;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLBROWSER);
    object->m_attributes |= 4;
    const char* value = Attr(object, "IMEFont");
    if (value && *value) {
        object->m_browserFlags |= 1;
        XMLObjectAddString(object, builder, &object->m_imeFont, value);
    }
    object->m_size = SizeOf(object, builder);
    XMLParseFrame(object, builder);
    return XMLObjectParseChildren(object, [](XMLOBJECT*, XMLBUILDER*, const char*) { return false; }, builder);
}

// ref: FUN_140668e40
bool XMLParseFogOfWarFrame(XMLFRAME* object, XMLBUILDER* builder) {
    object->m_frameType = XMLTYPE_FOGOFWARFRAME;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLFRAME);
    object->m_attributes |= 8;
    object->m_size = SizeOf(object, builder);
    XMLParseFrame(object, builder);
    return XMLObjectParseChildren(object, [](XMLOBJECT*, XMLBUILDER*, const char*) { return false; }, builder);
}

// ref: FUN_1406705d0
bool XMLParseMinimap(XMLMINIMAP* object, XMLBUILDER* builder) {
    struct TEXTUREATTR {
        const char* m_name;
        uint32_t m_flag;
        uint32_t m_offset;
    };
    // The arch* textures all land in the questBlobRing slot, as in the reference.
    static const TEXTUREATTR s_textures[] = {
        { "questBlobInsideTexture", 0x1, 0x190 },
        { "questBlobOutsideTexture", 0x2, 0x1a0 },
        { "questBlobOutsideSelectedTexture", 0x4, 0x1b0 },
        { "questBlobRingTexture", 0x8, 0x1c0 },
        { "taskBlobInsideTexture", 0x10, 0x1d0 },
        { "taskBlobOutsideTexture", 0x20, 0x1e0 },
        { "taskBlobOutsideSelectedTexture", 0x40, 0x1f0 },
        { "taskBlobRingTexture", 0x80, 0x200 },
        { "archBlobInsideTexture", 0x100, 0x1c0 },
        { "archBlobOutsideTexture", 0x200, 0x1c0 },
        { "archBlobRingTexture", 0x400, 0x1c0 },
    };
    for (const auto& texture : s_textures) {
        const char* value = Attr(object, texture.m_name);
        if (value) {
            object->m_minimapFlags |= texture.m_flag;
            auto slot = reinterpret_cast<XMLSTRING*>(reinterpret_cast<char*>(object) + texture.m_offset);
            XMLObjectAddString(object, builder, slot, value);
        }
    }
    object->m_size = SizeOf(object, builder);
    XMLParseFrame(object, builder);
    return true;
}

// <ScrollChild> under a scroll frame.
// ref: FUN_14066ce20
bool XMLParseScrollFrameChild(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    if (strcmp(name, "scrollchild") != 0) {
        return false;
    }
    object->m_type = XMLTYPE_SCROLLCHILD;
    builder->m_cursor = reinterpret_cast<char*>(object) + sizeof(XMLOBJECT);
    object->m_size = SizeOf(object, builder);
    XMLParseHeader(object, builder);
    return XMLObjectParseChildren(object, XMLParseFramesChild, builder);
}

}  // namespace

// The element types a frame definition can be, by element name; anything else is looked up
// as an intrinsic and parsed as the element type that intrinsic was defined with.
// ref: FUN_14066d450
bool XMLParseFrameType(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    auto frame = static_cast<XMLFRAME*>(object);
    char* base = reinterpret_cast<char*>(object);
    if (strcmp(name, "button") == 0) {
        frame->m_frameType = XMLTYPE_BUTTON;
        builder->m_cursor = base + sizeof(XMLBUTTON);
        return XMLParseButton(static_cast<XMLBUTTON*>(object), builder);
    }
    if (strcmp(name, "checkbutton") == 0) {
        auto check = static_cast<XMLCHECKBUTTON*>(object);
        frame->m_frameType = XMLTYPE_CHECKBUTTON;
        builder->m_cursor = base + sizeof(XMLCHECKBUTTON);
        object->m_flags |= 0x40;
        const char* value = Attr(object, "checked");
        if (value && *value) {
            SetBoolFlag(&check->m_checkFlags, 1, 1, value);
        }
        object->m_size = SizeOf(object, builder);
        XMLParseButton(check, builder);
        return XMLObjectParseChildren(object, [](XMLOBJECT* child, XMLBUILDER* b, const char* n) -> bool {
            // ref: FUN_14066af20
            if (strcmp(n, "checkedtexture") == 0) {
                child->m_type = XMLTYPE_CHECKEDTEXTURE;
            } else if (strcmp(n, "disabledcheckedtexture") == 0) {
                child->m_type = XMLTYPE_DISABLEDCHECKEDTEXTURE;
            } else {
                return false;
            }
            b->m_cursor = reinterpret_cast<char*>(child) + sizeof(XMLTEXTURE);
            child->m_size = SizeOf(child, b);
            return XMLParseTexture(static_cast<XMLTEXTURE*>(child), b);
        }, builder);
    }
    if (strcmp(name, "slider") == 0) {
        auto slider = static_cast<XMLSLIDER*>(object);
        frame->m_frameType = XMLTYPE_SLIDER;
        builder->m_cursor = base + sizeof(XMLSLIDER);
        object->m_flags |= 0x20000;
        const char* value = Attr(object, "valueStep");
        if (value && *value) {
            slider->m_rangeFlags |= 0x10;
            slider->m_valueStep = SStrToFloat(value);
        }
        value = Attr(object, "stepsPerPage");
        if (value && *value) {
            slider->m_rangeFlags |= 0x20;
            slider->m_stepsPerPage = strtol(value, nullptr, IntRadix(value));
        }
        value = Attr(object, "obeyStepOnDrag");
        if (value && *value) {
            slider->m_sliderFlags |= 1;
            slider->m_obeyStepOnDrag = StringToBOOL(value);
        }
        frame->m_drawLayer = DRAWLAYER_OVERLAY;
        object->m_size = SizeOf(object, builder);
        XMLParseRange(slider, builder);
        return XMLObjectParseChildren(object, [](XMLOBJECT* child, XMLBUILDER* b, const char* n) -> bool {
            // ref: FUN_14066cf10
            if (strcmp(n, "thumbtexture") != 0) {
                return false;
            }
            child->m_type = XMLTYPE_THUMBTEXTURE;
            b->m_cursor = reinterpret_cast<char*>(child) + sizeof(XMLTEXTURE);
            child->m_size = SizeOf(child, b);
            return XMLParseTexture(static_cast<XMLTEXTURE*>(child), b);
        }, builder);
    }
    if (strcmp(name, "scrollframe") == 0) {
        frame->m_frameType = XMLTYPE_SCROLLFRAME;
        builder->m_cursor = base + sizeof(XMLFRAME);
        object->m_size = SizeOf(object, builder);
        XMLParseFrame(frame, builder);
        return XMLObjectParseChildren(object, XMLParseScrollFrameChild, builder);
    }
    if (strcmp(name, "texture") == 0) {
        frame->m_frameType = XMLTYPE_TEXTURE;
        builder->m_cursor = base + sizeof(XMLTEXTURE);
        return XMLParseTexture(static_cast<XMLTEXTURE*>(object), builder);
    }
    if (strcmp(name, "line") == 0) {
        frame->m_frameType = XMLTYPE_LINE;
        builder->m_cursor = base + sizeof(XMLLINE);
        return XMLParseLine(static_cast<XMLLINE*>(object), builder);
    }
    if (strcmp(name, "model") == 0) {
        frame->m_frameType = XMLTYPE_MODEL;
        builder->m_cursor = base + sizeof(XMLMODEL);
        return XMLParseModel(static_cast<XMLMODEL*>(object), builder);
    }
    if (strcmp(name, "movieframe") == 0) {
        frame->m_frameType = XMLTYPE_MOVIEFRAME;
    } else if (strcmp(name, "modelffx") == 0) {
        auto model = static_cast<XMLMODELEX*>(object);
        frame->m_frameType = XMLTYPE_MODELFFX;
        builder->m_cursor = base + sizeof(XMLMODELEX);
        object->m_flags |= 0x1000000;
        const char* value = Attr(object, "glow");
        if (value) {
            model->m_modelExFlags |= 1;
            model->m_glow = SStrToFloat(value);
        }
        object->m_size = SizeOf(object, builder);
        XMLParseModel(model, builder);
        return true;
    } else if (strcmp(name, "messageframe") == 0) {
        return XMLParseMessageFrame(static_cast<XMLMESSAGEFRAME*>(object), builder);
    } else if (strcmp(name, "worldframe") == 0) {
        frame->m_frameType = XMLTYPE_WORLDFRAME;
    } else if (strcmp(name, "statusbar") == 0) {
        auto bar = static_cast<XMLRANGE*>(object);
        frame->m_frameType = XMLTYPE_STATUSBAR;
        builder->m_cursor = base + sizeof(XMLRANGE);
        object->m_flags |= 0x1000;
        frame->m_drawLayer = DRAWLAYER_ARTWORK;
        const char* value = Attr(object, "rotatesTexture");
        if (value && *value) {
            SetBoolFlag(&bar->m_rangeFlags, 0x40, 9, value);
        }
        value = Attr(object, "reverseFill");
        if (value && *value) {
            SetBoolFlag(&bar->m_rangeFlags, 0x80, 10, value);
        }
        value = Attr(object, "fillStyle");
        if (value && *value && StringToFillStyle(value, &bar->m_fillStyle)) {
            bar->m_rangeFlags |= 0x100;
        }
        object->m_size = SizeOf(object, builder);
        XMLParseRange(bar, builder);
        return XMLObjectParseChildren(object, [](XMLOBJECT* child, XMLBUILDER* b, const char* n) -> bool {
            // ref: FUN_14066cf70
            if (strcmp(n, "bartexture") == 0) {
                child->m_type = XMLTYPE_BARTEXTURE;
                b->m_cursor = reinterpret_cast<char*>(child) + sizeof(XMLTEXTURE);
                child->m_size = SizeOf(child, b);
                return XMLParseTexture(static_cast<XMLTEXTURE*>(child), b);
            }
            if (strcmp(n, "barcolor") == 0) {
                child->m_type = XMLTYPE_BARCOLOR;
                b->m_cursor = reinterpret_cast<char*>(child) + sizeof(XMLCOLOR);
                child->m_size = SizeOf(child, b);
                return XMLParseColor(static_cast<XMLCOLOR*>(child), b);
            }
            return false;
        }, builder);
    } else if (strcmp(name, "cooldown") == 0) {
        frame->m_frameType = XMLTYPE_COOLDOWN;
        builder->m_cursor = base + sizeof(XMLCOOLDOWN);
        return XMLParseCooldown(static_cast<XMLCOOLDOWN*>(object), builder);
    } else if (strcmp(name, "editbox") == 0) {
        frame->m_frameType = XMLTYPE_EDITBOX;
        builder->m_cursor = base + sizeof(XMLEDITBOX);
        return XMLParseEditBox(static_cast<XMLEDITBOX*>(object), builder);
    } else if (strcmp(name, "gametooltip") == 0) {
        frame->m_frameType = XMLTYPE_GAMETOOLTIP;
    } else {
        if (strcmp(name, "blobframe") == 0) {
            frame->m_frameType = XMLTYPE_BLOBFRAME;
            builder->m_cursor = base + sizeof(XMLBLOBFRAME);
            return XMLParseBlobFrame(static_cast<XMLBLOBFRAME*>(object), builder);
        }
        if (strcmp(name, "questpoiframe") == 0) {
            frame->m_frameType = XMLTYPE_QUESTPOIFRAME;
            builder->m_cursor = base + sizeof(XMLBLOBFRAME);
            object->m_flags |= 0x4000000;
        } else if (strcmp(name, "archaeologydigsiteframe") == 0) {
            frame->m_frameType = XMLTYPE_ARCHAEOLOGYDIGSITEFRAME;
            builder->m_cursor = base + sizeof(XMLBLOBFRAME);
            object->m_flags |= 0x8000000;
        } else if (strcmp(name, "scenariopoiframe") == 0) {
            frame->m_frameType = XMLTYPE_SCENARIOPOIFRAME;
            builder->m_cursor = base + sizeof(XMLBLOBFRAME);
            object->m_flags |= 0x10000000;
        } else {
            if (strcmp(name, "minimap") == 0) {
                frame->m_frameType = XMLTYPE_MINIMAP;
                builder->m_cursor = base + sizeof(XMLMINIMAP);
                return XMLParseMinimap(static_cast<XMLMINIMAP*>(object), builder);
            }
            if (strcmp(name, "playermodel") == 0) {
                frame->m_frameType = XMLTYPE_PLAYERMODEL;
                builder->m_cursor = base + sizeof(XMLMODEL);
            } else if (strcmp(name, "cinematicmodel") == 0) {
                auto model = static_cast<XMLMODELEX*>(object);
                frame->m_frameType = XMLTYPE_CINEMATICMODEL;
                builder->m_cursor = base + sizeof(XMLMODELEX);
                const char* value = Attr(object, "facingLeft");
                if (value) {
                    SetBoolFlag(&model->m_modelExFlags, 1, 1, value);
                }
                object->m_size = SizeOf(object, builder);
            } else {
                if (strcmp(name, "dressupmodel") == 0) {
                    frame->m_frameType = XMLTYPE_DRESSUPMODEL;
                } else if (strcmp(name, "tabardmodel") == 0) {
                    frame->m_frameType = XMLTYPE_TABARDMODEL;
                } else if (strcmp(name, "uicamera") == 0) {
                    frame->m_frameType = XMLTYPE_UICAMERA;
                } else if (strcmp(name, "fontstring") == 0) {
                    frame->m_frameType = XMLTYPE_FONTSTRING;
                    builder->m_cursor = base + sizeof(XMLFONTSTRING);
                    return XMLParseFontString(static_cast<XMLFONTSTRING*>(object), builder);
                } else if (strcmp(name, "colorselect") == 0) {
                    frame->m_frameType = XMLTYPE_COLORSELECT;
                    builder->m_cursor = base + sizeof(XMLFRAME);
                    object->m_flags |= 0x20000000;
                    object->m_size = SizeOf(object, builder);
                    XMLParseFrame(frame, builder);
                    return XMLObjectParseChildren(object, [](XMLOBJECT* child, XMLBUILDER* b, const char* n) -> bool {
                        // ref: FUN_14066afb0
                        if (strcmp(n, "colorwheeltexture") == 0) {
                            child->m_type = XMLTYPE_COLORWHEELTEXTURE;
                        } else if (strcmp(n, "colorwheelthumbtexture") == 0) {
                            child->m_type = XMLTYPE_COLORWHEELTHUMBTEXTURE;
                        } else if (strcmp(n, "colorvaluetexture") == 0) {
                            child->m_type = XMLTYPE_COLORVALUETEXTURE;
                        } else if (strcmp(n, "colorvaluethumbtexture") == 0) {
                            child->m_type = XMLTYPE_COLORVALUETHUMBTEXTURE;
                        } else {
                            return false;
                        }
                        b->m_cursor = reinterpret_cast<char*>(child) + sizeof(XMLTEXTURE);
                        child->m_size = SizeOf(child, b);
                        return XMLParseTexture(static_cast<XMLTEXTURE*>(child), b);
                    }, builder);
                } else if (strcmp(name, "simplehtml") == 0) {
                    return XMLParseSimpleHTML(static_cast<XMLSIMPLEHTML*>(object), builder);
                } else if (strcmp(name, "frame") == 0) {
                    frame->m_frameType = XMLTYPE_FRAME;
                    builder->m_cursor = base + sizeof(XMLFRAME);
                    return XMLParseFrame(frame, builder);
                } else if (strcmp(name, "browser") == 0) {
                    return XMLParseBrowser(static_cast<XMLBROWSER*>(object), builder);
                } else if (strcmp(name, "offscreenframe") == 0) {
                    frame->m_frameType = XMLTYPE_OFFSCREENFRAME;
                    goto plainFrame;
                } else if (strcmp(name, "unitpositionframe") == 0) {
                    frame->m_frameType = XMLTYPE_UNITPOSITIONFRAME;
                    goto plainFrame;
                } else if (strcmp(name, "modelscene") == 0) {
                    return XMLParseModelScene(static_cast<XMLMODELSCENE*>(object), builder);
                } else if (strcmp(name, "checkout") == 0) {
                    return XMLParseCheckout(static_cast<XMLBROWSER*>(object), builder);
                } else if (strcmp(name, "fogofwarframe") == 0) {
                    return XMLParseFogOfWarFrame(frame, builder);
                } else {
                    // An intrinsic used as an element: parse it as what the intrinsic is.
                    char intrinsic[0x80] = {};
                    LowerName(intrinsic, name);
                    bool recursive = true;
                    const char* taint = nullptr;
                    XMLOBJECT* found = nullptr;
                    XMLOBJECT* definition = FrameXML_AcquireIntrinsic(intrinsic, &taint, &recursive);
                    if (definition) {
                        found = definition;
                    }
                    bool result;
                    if (found == nullptr || recursive) {
                        result = false;
                    } else {
                        char tag[0x80];
                        LowerName(tag, found->m_tag.m_text);
                        _strlwr(tag);
                        result = XMLParseFrameType(object, builder, tag);
                    }
                    if (found != nullptr && !recursive) {
                        FrameXML_ReleaseIntrinsic(intrinsic);
                    }
                    return result;
                }
                builder->m_cursor = base + sizeof(XMLMODEL);
                object->m_size = SizeOf(object, builder);
            }
            object->m_size = SizeOf(object, builder);
            return XMLParseModel(static_cast<XMLMODEL*>(object), builder);
        }
        object->m_size = SizeOf(object, builder);
        XMLParseBlobFrame(static_cast<XMLBLOBFRAME*>(object), builder);
        return true;
    }
plainFrame:
    builder->m_cursor = base + sizeof(XMLFRAME);
    object->m_size = SizeOf(object, builder);
    return XMLParseFrame(frame, builder);
}

// What a document (or a scoped modifier) holds at the top level.
// ref: FUN_140669950
bool XMLParseTopLevel(XMLOBJECT* object, XMLBUILDER* builder, const char* name) {
    char* base = reinterpret_cast<char*>(object);
    if (strcmp(name, "include") == 0) {
        auto include = static_cast<XMLFILE*>(object);
        object->m_type = XMLTYPE_INCLUDE;
        builder->m_cursor = base + sizeof(XMLFILE);
        const char* value = Attr(object, "file");
        if (value) {
            include->m_fileFlags |= 1;
            XMLObjectAddString(object, builder, &include->m_file, value);
        }
        object->m_size = SizeOf(object, builder);
        XMLParseHeader(object, builder);
        return true;
    }
    if (strcmp(name, "script") == 0) {
        auto script = static_cast<XMLSCRIPT*>(object);
        object->m_type = XMLTYPE_SCRIPT;
        builder->m_cursor = base + sizeof(XMLSCRIPT);
        const char* value = Attr(object, "file");
        if (value) {
            script->m_scriptFlags |= 1;
            XMLObjectAddString(object, builder, &script->m_file, value);
        }
        object->m_size = SizeOf(object, builder);
        if (object->m_node->m_body != nullptr) {
            script->m_bodyFlags |= 1;
            XMLObjectAddString(object, builder, &script->m_body, object->m_node->m_body);
        }
        XMLParseHeader(object, builder);
        return true;
    }
    if (strcmp(name, "font") == 0) {
        object->m_type = XMLTYPE_FONT;
        builder->m_cursor = base + sizeof(XMLFONT);
        return XMLParseFont(static_cast<XMLFONT*>(object), builder);
    }
    if (strcmp(name, "fontfamily") == 0) {
        object->m_type = XMLTYPE_FONTFAMILY;
        builder->m_cursor = base + sizeof(XMLOBJECT);
        object->m_attributes |= 1;
        object->m_size = SizeOf(object, builder);
        XMLParseHeader(object, builder);
        return XMLObjectParseChildren(object, [](XMLOBJECT* child, XMLBUILDER* b, const char* n) -> bool {
            // ref: FUN_14066b5d0
            if (strcmp(n, "member") != 0) {
                return false;
            }
            auto member = static_cast<XMLFILE*>(child);
            child->m_type = XMLTYPE_MEMBER;
            b->m_cursor = reinterpret_cast<char*>(child) + sizeof(XMLFILE);
            child->m_attributes |= 2;
            const char* alphabet = Attr(child, "alphabet");
            if (alphabet == nullptr) {
                return false;
            }
            member->m_fileFlags |= 1;
            XMLObjectAddString(child, b, &member->m_file, alphabet);
            child->m_size = SizeOf(child, b);
            XMLParseHeader(child, b);
            return XMLObjectParseChildren(child, [](XMLOBJECT* font, XMLBUILDER* fb, const char* fn) -> bool {
                // LAB_14066b6a0
                if (strcmp(fn, "font") != 0) {
                    return false;
                }
                font->m_type = XMLTYPE_FONT;
                fb->m_cursor = reinterpret_cast<char*>(font) + sizeof(XMLFONT);
                return XMLParseFont(static_cast<XMLFONT*>(font), fb);
            }, b);
        }, builder);
    }
    if (strcmp(name, "animationgroup") == 0) {
        return XMLParseAnimationGroup(static_cast<XMLANIMATIONGROUP*>(object), builder);
    }
    if (strcmp(name, "actor") == 0) {
        object->m_type = XMLTYPE_ACTOR;
        builder->m_cursor = base + sizeof(XMLACTOR);
        return XMLParseActor(static_cast<XMLACTOR*>(object), builder);
    }
    object->m_type = XMLTYPE_FRAMEDEF;
    builder->m_cursor = base + sizeof(XMLREGION);
    return XMLParseFrameType(object, builder, name);
}
