#ifndef UI_BINARY_XML_HPP
#define UI_BINARY_XML_HPP

// BinaryXML (Engine/Source/FrameXML/BinaryXML.cpp): an XMLTree turned into typed element
// objects. Each element becomes a fixed-layout record (a common header, then the fields of
// its type), built in a bump arena of 32 KiB blocks with its strings in separate blocks.
// A finished tree can be compacted into one allocation: the strings follow each record, and
// every string is reached through an offset chain from its record so the records can move.

#include <cstdint>

#include "framescript/FrameScript.hpp"

class CStatus;
class XMLNode;

// A string an element owns. The `m_prev` offsets (from the element) chain every string the
// element holds, newest first, starting at XMLOBJECT::m_strings.
struct XMLSTRING {
    int32_t m_prev;             // 0x00
    char* m_text;               // 0x08
};

// The element types (XMLOBJECT::m_type; frames also carry theirs in XMLREGION::m_frameType).
enum XMLTYPE : uint32_t {
    XMLTYPE_UI = 0x01,
    // 0x02..0x20, 0x2d..0x4e, 0x99..0x9f, 0xc3..0xda: script handlers (XMLSCRIPTHANDLER)
    XMLTYPE_ATTRIBUTE = 0x21,
    XMLTYPE_ATTRIBUTES = 0x22,
    XMLTYPE_RESIZEBOUNDS = 0x23,
    XMLTYPE_HITRECTINSETS = 0x24,
    XMLTYPE_LAYERS = 0x25,
    XMLTYPE_LAYER = 0x26,
    XMLTYPE_SCRIPTS = 0x27,
    XMLTYPE_FRAMES = 0x2a,
    XMLTYPE_SCROLLCHILD = 0x2b,
    XMLTYPE_SIMPLEHTML = 0x2c,
    XMLTYPE_INCLUDE = 0x4f,
    XMLTYPE_SCRIPT = 0x50,
    XMLTYPE_FONT = 0x51,
    XMLTYPE_FRAME = 0x52,
    XMLTYPE_NORMALTEXTURE = 0x54,
    XMLTYPE_PUSHEDTEXTURE = 0x55,
    XMLTYPE_DISABLEDTEXTURE = 0x56,
    XMLTYPE_HIGHLIGHTTEXTURE = 0x57,
    XMLTYPE_BUTTONTEXT = 0x58,
    XMLTYPE_DISABLEDFONT = 0x59,
    XMLTYPE_NORMALFONT = 0x5a,
    XMLTYPE_HIGHLIGHTFONT = 0x5b,
    XMLTYPE_NORMALCOLOR = 0x5c,
    XMLTYPE_DISABLEDCOLOR = 0x5d,
    XMLTYPE_HIGHLIGHTCOLOR = 0x5e,
    XMLTYPE_BARCOLOR = 0x60,
    XMLTYPE_PUSHEDTEXTOFFSET = 0x61,
    XMLTYPE_CHECKEDTEXTURE = 0x62,
    XMLTYPE_DISABLEDCHECKEDTEXTURE = 0x63,
    XMLTYPE_FONTHEIGHT = 0x64,
    XMLTYPE_BUTTON = 0x65,
    XMLTYPE_CHECKBUTTON = 0x66,
    XMLTYPE_COLORSELECT = 0x67,
    XMLTYPE_COOLDOWN = 0x68,
    XMLTYPE_DRESSUPMODEL = 0x69,
    XMLTYPE_EDITBOX = 0x6a,
    XMLTYPE_FONTSTRING = 0x6b,
    XMLTYPE_GAMETOOLTIP = 0x6c,
    XMLTYPE_MESSAGEFRAME = 0x6d,
    XMLTYPE_MINIMAP = 0x6e,
    XMLTYPE_MODEL = 0x6f,
    XMLTYPE_MODELSCENE = 0x70,
    XMLTYPE_ACTOR = 0x71,
    XMLTYPE_ACTORS = 0x72,
    XMLTYPE_VIEWINSETS = 0x73,
    XMLTYPE_MODELFFX = 0x74,
    XMLTYPE_MOVIEFRAME = 0x76,
    XMLTYPE_PLAYERMODEL = 0x77,
    XMLTYPE_BLOBFRAME = 0x78,
    XMLTYPE_QUESTPOIFRAME = 0x79,
    XMLTYPE_ARCHAEOLOGYDIGSITEFRAME = 0x7a,
    XMLTYPE_SCENARIOPOIFRAME = 0x7b,
    XMLTYPE_SCROLLFRAME = 0x7c,
    XMLTYPE_SLIDER = 0x7d,
    XMLTYPE_STATUSBAR = 0x7e,
    XMLTYPE_TABARDMODEL = 0x7f,
    XMLTYPE_CINEMATICMODEL = 0x80,
    XMLTYPE_UICAMERA = 0x81,
    XMLTYPE_TEXTURE = 0x82,
    XMLTYPE_MASKTEXTURE = 0x83,
    XMLTYPE_WORLDFRAME = 0x84,
    XMLTYPE_COLORWHEELTEXTURE = 0x85,
    XMLTYPE_COLORWHEELTHUMBTEXTURE = 0x86,
    XMLTYPE_COLORVALUETEXTURE = 0x87,
    XMLTYPE_COLORVALUETHUMBTEXTURE = 0x88,
    XMLTYPE_SIZE = 0x89,
    XMLTYPE_ANCHORS = 0x8a,
    XMLTYPE_STARTANCHOR = 0x8b,
    XMLTYPE_ENDANCHOR = 0x8c,
    XMLTYPE_ANCHOR = 0x8d,
    XMLTYPE_ANIMATIONS = 0x8f,
    XMLTYPE_COLOR = 0x90,
    XMLTYPE_GRADIENT = 0x91,
    XMLTYPE_TEXCOORDS = 0x92,
    XMLTYPE_FRAMEDEF = 0x93,     // an element parsed as a frame; its kind is m_frameType
    XMLTYPE_MINCOLOR = 0x95,
    XMLTYPE_MAXCOLOR = 0x96,
    XMLTYPE_OFFSET = 0x97,
    XMLTYPE_SHADOW = 0x98,
    XMLTYPE_FONTSTRINGHEADER1 = 0xa0,
    XMLTYPE_FONTSTRINGHEADER2 = 0xa1,
    XMLTYPE_FONTSTRINGHEADER3 = 0xa2,
    XMLTYPE_THUMBTEXTURE = 0xa3,
    XMLTYPE_BARTEXTURE = 0xa4,
    XMLTYPE_ANIMATIONGROUP = 0xa5,
    XMLTYPE_ANIMATION = 0xa6,
    XMLTYPE_ALPHA = 0xa7,
    XMLTYPE_FLIPBOOK = 0xa8,
    XMLTYPE_TRANSLATION = 0xa9,
    XMLTYPE_LINETRANSLATION = 0xaa,
    XMLTYPE_TEXCOORDTRANSLATION = 0xab,
    XMLTYPE_FOGCOLOR = 0xac,
    XMLTYPE_TEXTINSETS = 0xad,
    XMLTYPE_ABSINSET = 0xaf,
    XMLTYPE_RELINSET = 0xb0,
    XMLTYPE_MINRESIZE = 0xb1,
    XMLTYPE_MAXRESIZE = 0xb2,
    XMLTYPE_ABSDIMENSION = 0xb3,
    XMLTYPE_RELDIMENSION = 0xb4,
    XMLTYPE_ABSVALUE = 0xb6,
    XMLTYPE_RELVALUE = 0xb7,
    XMLTYPE_ROTATION = 0xb8,
    XMLTYPE_PATH = 0xb9,
    XMLTYPE_SCALE = 0xba,
    XMLTYPE_LINESCALE = 0xbb,
    XMLTYPE_ORIGIN = 0xbd,
    XMLTYPE_CONTROLPOINT = 0xbf,
    XMLTYPE_CONTROLPOINTS = 0xc0,
    XMLTYPE_RECT = 0xc1,
    XMLTYPE_BROWSER = 0xc2,
    XMLTYPE_SCOPEDMODIFIER = 0xc5,
    XMLTYPE_SWIPETEXTURE = 0xc9,
    XMLTYPE_EDGETEXTURE = 0xca,
    XMLTYPE_BLINGTEXTURE = 0xcb,
    XMLTYPE_KEYVALUES = 0xcc,
    XMLTYPE_KEYVALUE = 0xcd,
    XMLTYPE_FONTFAMILY = 0xd0,
    XMLTYPE_MEMBER = 0xd1,
    XMLTYPE_OFFSCREENFRAME = 0xd2,
    XMLTYPE_LINE = 0xd3,
    XMLTYPE_UNITPOSITIONFRAME = 0xd4,
    XMLTYPE_MASKEDTEXTURES = 0xd5,
    XMLTYPE_MASKEDTEXTURE = 0xd6,
    XMLTYPE_CHECKOUT = 0xd7,
    XMLTYPE_FOGOFWARFRAME = 0xd9,
};

// The header every element starts with (0xa8).
struct XMLOBJECT {
    int32_t m_strings;              // 0x00 offset of the newest owned string, 0 for none
    void* m_block;                  // 0x08 the compacted block the tree lives in (root only)
    XMLSTRING m_tag;                // 0x10 the element name as written
    XMLSTRING m_inherits;           // 0x20
    XMLSTRING m_name;               // 0x30
    XMLSTRING m_mixin;              // 0x40
    XMLSTRING m_secureMixin;        // 0x50
    XMLSTRING m_secureReferenceKey; // 0x60
    SCRIPTLOCATION m_location;      // 0x70
    int32_t m_size;                 // 0x78 bytes of the record itself
    uint32_t m_scriptState;         // 0x7C taint state captured when a tree is loaded
    XMLNode* m_node;                // 0x80 the source node while building
    XMLOBJECT* m_child;             // 0x88
    XMLOBJECT* m_next;              // 0x90
    // Low 5 bits: the scoped-modifier state it was loaded under. 0x20 deferred (owned by the
    // virtual/intrinsic registry), and one bit per parser that ran: 0x80 button, 0x100
    // frame, 0x200 region, 0x400 script handler, 0x80000 animation, 0x100000 font, ...
    uint32_t m_flags;               // 0x98
    // 0x10/0x200 virtual, 0x20/0x400 intrinsic, 0x40 named, 0x80 secureReferenceKey,
    // 0x100 inherits, 0x800 mixin, 0x1000 secureMixin; 1 font family, 2 font member.
    uint32_t m_attributes;          // 0x9C
    uint32_t m_type;                // 0xA0
};
static_assert(sizeof(XMLOBJECT) == 0xa8, "XMLOBJECT is 0xa8 bytes");

// x/y (0xb8): Size, Offset, AbsDimension, RelDimension, Shadow, PushedTextOffset, ...
struct XMLDIMENSION : XMLOBJECT {
    float m_x;                      // 0xA8
    float m_y;                      // 0xAC
    uint32_t m_dimensionFlags;      // 0xB0 1 x, 2 y
};
static_assert(sizeof(XMLDIMENSION) == 0xb8, "XMLDIMENSION is 0xb8 bytes");

struct XMLORIGIN : XMLDIMENSION {
    uint32_t m_originFlags;         // 0xB8 1 point
    uint32_t m_point;               // 0xBC
};
static_assert(sizeof(XMLORIGIN) == 0xc0, "XMLORIGIN is 0xc0 bytes");

struct XMLANCHOR : XMLDIMENSION {
    uint32_t m_point;               // 0xB8
    uint32_t m_relativePoint;       // 0xBC
    XMLSTRING m_relativeTo;         // 0xC0
    XMLSTRING m_relativeKey;        // 0xD0
    uint32_t m_anchorFlags;         // 0xE0 1 point, 2 relativePoint, 4 relativeTo, 8 relativeKey
};
static_assert(sizeof(XMLANCHOR) == 0xe8, "XMLANCHOR is 0xe8 bytes");

// left/right/top/bottom (0xc0): HitRectInsets, TextInsets, ViewInsets, AbsInset, RelInset.
struct XMLINSET : XMLOBJECT {
    float m_left;                   // 0xA8
    float m_right;                  // 0xAC
    float m_top;                    // 0xB0
    float m_bottom;                 // 0xB4
    uint32_t m_insetFlags;          // 0xB8 1 left, 2 right, 4 top, 8 bottom
};
static_assert(sizeof(XMLINSET) == 0xc0, "XMLINSET is 0xc0 bytes");

// FontHeight, AbsValue, RelValue.
struct XMLVALUE : XMLOBJECT {
    float m_value;                  // 0xA8
    uint32_t m_valueFlags;          // 0xAC 1 val
};
static_assert(sizeof(XMLVALUE) == 0xb0, "XMLVALUE is 0xb0 bytes");

struct XMLCOLOR : XMLOBJECT {
    uint32_t m_colorFlags;          // 0xA8 1 set
    float m_r;                      // 0xAC
    float m_g;                      // 0xB0
    float m_b;                      // 0xB4
    float m_a;                      // 0xB8
};
static_assert(sizeof(XMLCOLOR) == 0xc0, "XMLCOLOR is 0xc0 bytes");

struct XMLGRADIENT : XMLOBJECT {
    uint32_t m_gradientFlags;       // 0xA8 1 orientation
    uint32_t m_orientation;         // 0xAC
};
static_assert(sizeof(XMLGRADIENT) == 0xb0, "XMLGRADIENT is 0xb0 bytes");

struct XMLTEXCOORDS : XMLOBJECT {
    // 1 left, 2 right, 4 top, 8 bottom, 0x10 degrees, 0x20 radians, 0x40/0x80 flip
    uint32_t m_texFlags;            // 0xA8
    float m_left;                   // 0xAC
    float m_right;                  // 0xB0
    float m_top;                    // 0xB4
    float m_bottom;                 // 0xB8
    float m_radians;                // 0xBC
    float m_degrees;                // 0xC0
};
static_assert(sizeof(XMLTEXCOORDS) == 0xc8, "XMLTEXCOORDS is 0xc8 bytes");

struct XMLRECT : XMLOBJECT {
    uint32_t m_rectFlags;           // 0xA8 one bit per corner value
    float m_ulx;                    // 0xAC
    float m_uly;                    // 0xB0
    float m_urx;                    // 0xB4
    float m_ury;                    // 0xB8
    float m_llx;                    // 0xBC
    float m_lly;                    // 0xC0
    float m_lrx;                    // 0xC4
    float m_lry;                    // 0xC8
};
static_assert(sizeof(XMLRECT) == 0xd0, "XMLRECT is 0xd0 bytes");

// An element holding one string: Include and the cooldown textures (file), Member
// (alphabet), the button fonts (style).
struct XMLFILE : XMLOBJECT {
    uint32_t m_fileFlags;           // 0xA8 1 set
    XMLSTRING m_file;               // 0xB0
};
static_assert(sizeof(XMLFILE) == 0xc0, "XMLFILE is 0xc0 bytes");

struct XMLSCRIPT : XMLOBJECT {
    uint32_t m_bodyFlags;           // 0xA8 1 body
    XMLSTRING m_body;               // 0xB0
    uint32_t m_scriptFlags;         // 0xC0 1 file
    XMLSTRING m_file;               // 0xC8
};
static_assert(sizeof(XMLSCRIPT) == 0xd8, "XMLSCRIPT is 0xd8 bytes");

// One handler under <Scripts>: OnLoad, OnClick, ...
struct XMLSCRIPTHANDLER : XMLOBJECT {
    uint32_t m_bodyFlags;           // 0xA8 1 body
    XMLSTRING m_body;               // 0xB0
    // 1 function, 2 method, 4..0x100 handler argument kinds, 0x200 autoEnableInput,
    // 0x400 inherit append, 0x800 inherit prepend, 0x1000 intrinsicOrder postcall
    uint32_t m_handlerFlags;        // 0xC0
    XMLSTRING m_function;           // 0xC8
    XMLSTRING m_method;             // 0xD8
    int32_t* m_ref;                 // 0xE8 the compiled function's registry reference
};
static_assert(sizeof(XMLSCRIPTHANDLER) == 0xf0, "XMLSCRIPTHANDLER is 0xf0 bytes");

struct XMLLAYER : XMLOBJECT {
    uint32_t m_layerFlags;          // 0xA8 1 level, 2 textureSubLevel
    uint32_t m_level;               // 0xAC draw layer, ARTWORK by default
    int8_t m_subLevel;              // 0xB0 -8..7
};
static_assert(sizeof(XMLLAYER) == 0xb8, "XMLLAYER is 0xb8 bytes");

struct XMLATTRIBUTE : XMLOBJECT {
    XMLSTRING m_value;              // 0xA8
    XMLSTRING m_valueType;          // 0xB8
    uint32_t m_attributeFlags;      // 0xC8 1 value, 2 type
};
static_assert(sizeof(XMLATTRIBUTE) == 0xd0, "XMLATTRIBUTE is 0xd0 bytes");

struct XMLKEYVALUE : XMLOBJECT {
    XMLSTRING m_key;                // 0xA8
    XMLSTRING m_keyType;            // 0xB8
    XMLSTRING m_value;              // 0xC8
    XMLSTRING m_valueType;          // 0xD8
    uint32_t m_keyValueFlags;       // 0xE8 1 key, 2 keyType, 4 value, 8 type
};
static_assert(sizeof(XMLKEYVALUE) == 0xf0, "XMLKEYVALUE is 0xf0 bytes");

struct XMLMASKEDTEXTURE : XMLOBJECT {
    uint32_t m_maskFlags;           // 0xA8 1 target, 2 childKey
    XMLSTRING m_target;             // 0xB0
    XMLSTRING m_childKey;           // 0xC0
};
static_assert(sizeof(XMLMASKEDTEXTURE) == 0xd0, "XMLMASKEDTEXTURE is 0xd0 bytes");

struct XMLSCOPEDMODIFIER : XMLOBJECT {
    // 1/2 forbidden, 4/8 scriptsUseGivenEnv, 0x10/0x20 fullLockdown, 0x40/0x80 hideFromGlobalEnv
    uint32_t m_modifierFlags;       // 0xA8
};
static_assert(sizeof(XMLSCOPEDMODIFIER) == 0xb0, "XMLSCOPEDMODIFIER is 0xb0 bytes");

struct XMLANIMATIONGROUP : XMLOBJECT {
    // 1 parentKey, 2 parentArray, 4 looping, 8/0x10 setToFinalAlpha, 0x20 speed
    uint32_t m_groupFlags;          // 0xA8
    float m_speedMultiplier;        // 0xAC
    XMLSTRING m_looping;            // 0xB0
    XMLSTRING m_parentKey;          // 0xC0
    XMLSTRING m_parentArray;        // 0xD0
};
static_assert(sizeof(XMLANIMATIONGROUP) == 0xe0, "XMLANIMATIONGROUP is 0xe0 bytes");

struct XMLANIMATION : XMLOBJECT {
    uint32_t m_animFlags;           // 0xA8 1 parentKey, 2 startDelay, 4 endDelay, 8 duration
    uint32_t m_hasSmoothing;        // 0xAC
    uint32_t m_targetFlags;         // 0xB0 1 order, 2 target, 4 childKey, 8 targetKey
    int32_t m_order;                // 0xB4
    float m_startDelay;             // 0xB8
    float m_endDelay;               // 0xBC
    float m_duration;               // 0xC0
    XMLSTRING m_smoothing;          // 0xC8
    XMLSTRING m_parentKey;          // 0xD8
    XMLSTRING m_target;             // 0xE8
    XMLSTRING m_childKey;           // 0xF8
    XMLSTRING m_targetKey;          // 0x108
};
static_assert(sizeof(XMLANIMATION) == 0x118, "XMLANIMATION is 0x118 bytes");

// Alpha (from/to), Translation (offsetX/Y), TexCoordTranslation (offsetU/V),
// Rotation (degrees/radians).
struct XMLANIMATIONPAIR : XMLANIMATION {
    uint32_t m_pairFlags;           // 0x118 1 first, 2 second
    float m_first;                  // 0x11C
    float m_second;                 // 0x120
};
static_assert(sizeof(XMLANIMATIONPAIR) == 0x128, "XMLANIMATIONPAIR is 0x128 bytes");

struct XMLANIMATIONSCALE : XMLANIMATION {
    uint32_t m_scaleFlags;          // 0x118
    float m_scaleX;                 // 0x11C
    float m_scaleY;                 // 0x120
    float m_fromScaleX;             // 0x124
    float m_fromScaleY;             // 0x128
    float m_toScaleX;               // 0x12C
    float m_toScaleY;               // 0x130
};
static_assert(sizeof(XMLANIMATIONSCALE) == 0x138, "XMLANIMATIONSCALE is 0x138 bytes");

struct XMLANIMATIONFLIPBOOK : XMLANIMATION {
    uint32_t m_flipBookFlags;       // 0x118
    uint32_t m_rows;                // 0x11C
    uint32_t m_columns;             // 0x120
    uint32_t m_frames;              // 0x124
    uint32_t m_frameWidth;          // 0x128
    uint32_t m_frameHeight;         // 0x12C
};
static_assert(sizeof(XMLANIMATIONFLIPBOOK) == 0x130, "XMLANIMATIONFLIPBOOK is 0x130 bytes");

struct XMLANIMATIONPATH : XMLANIMATION {
    uint32_t m_pathFlags;           // 0x118 1 curve
    XMLSTRING m_curve;              // 0x120
};
static_assert(sizeof(XMLANIMATIONPATH) == 0x130, "XMLANIMATIONPATH is 0x130 bytes");

struct XMLCONTROLPOINT : XMLOBJECT {
    uint32_t m_pointFlags;          // 0xA8 1 offsetX, 2 offsetY, 4 parentKey
    XMLSTRING m_parentKey;          // 0xB0
    float m_offsetX;                // 0xC0
    float m_offsetY;                // 0xC4
};
static_assert(sizeof(XMLCONTROLPOINT) == 0xc8, "XMLCONTROLPOINT is 0xc8 bytes");

struct XMLACTOR : XMLOBJECT {
    // 1 parentKey, 2 parentArray, 4 file, 8 fileID, 0x10 scale, 0x20 alpha, 0x40 desaturation
    uint32_t m_actorFlags;          // 0xA8
    float m_scale;                  // 0xAC
    float m_alpha;                  // 0xB0
    float m_desaturation;           // 0xB4
    int32_t m_fileID;               // 0xB8
    XMLSTRING m_parentKey;          // 0xC0
    XMLSTRING m_parentArray;        // 0xD0
    XMLSTRING m_file;               // 0xE0
};
static_assert(sizeof(XMLACTOR) == 0xf0, "XMLACTOR is 0xf0 bytes");

// The font attributes. Every region parses them too, into the same place.
struct XMLFONT : XMLOBJECT {
    // 1 justifyV, 2 justifyH, 4/0x40 nonspacewrap, 8/0x10 indented, 0x20 spacing, 0x80 font,
    // 0x100 outline, 0x200/0x400 monochrome, 0x800 height, 0x1000/0x2000 filter,
    // 0x4000/0x8000 fixedsize
    uint32_t m_fontFlags;           // 0xA8
    XMLSTRING m_font;               // 0xB0
    float m_height;                 // 0xC0
    float m_spacing;                // 0xC4
    XMLSTRING m_justifyV;           // 0xC8
    XMLSTRING m_justifyH;           // 0xD8
    XMLSTRING m_outline;            // 0xE8
};
static_assert(sizeof(XMLFONT) == 0xf8, "XMLFONT is 0xf8 bytes");

struct XMLREGION : XMLFONT {
    uint32_t m_setAllPoints;        // 0xF8 1 set, 2 value
    uint32_t m_padFC;
    // 1 alpha, 2/0x10 hidden, 4 parentKey, 8 parentArray, 0x20/0x40 ignoreParentAlpha,
    // 0x80/0x100 ignoreParentScale, 0x200 scale, 0x400/0x800 enableMouse,
    // 0x1000/0x2000 enableMouseClicks, 0x4000/0x8000 enableMouseMotion, 0x10000
    // registerForDrag, 0x20000 passThroughButtons, 0x40000/0x80000
    // retainClickThroughOverride, 0x100000/0x200000 protected
    uint32_t m_regionFlags;         // 0x100
    XMLSTRING m_parentKey;          // 0x108
    XMLSTRING m_parentArray;        // 0x118
    XMLSTRING m_registerForDrag;    // 0x128
    XMLSTRING m_passThroughButtons; // 0x138
    float m_alpha;                  // 0x148
    float m_scale;                  // 0x14C
    uint32_t m_frameType;           // 0x150 for XMLTYPE_FRAMEDEF elements
};
static_assert(sizeof(XMLREGION) == 0x158, "XMLREGION is 0x158 bytes");

struct XMLFRAME : XMLREGION {
    uint32_t m_frameFlags;          // 0x158
    uint32_t m_frameFlags2;         // 0x15C
    uint32_t m_drawLayer;           // 0x160
    int32_t m_id;                   // 0x164
    XMLSTRING m_parent;             // 0x168
    uint32_t m_strata;              // 0x178
    float m_depth;                  // 0x17C
    int16_t m_frameLevel;           // 0x180
};
static_assert(sizeof(XMLFRAME) == 0x188, "XMLFRAME is 0x188 bytes");

struct XMLTEXTURE : XMLREGION {
    uint32_t m_textureFlags;        // 0x158
    float m_rotation;               // 0x15C
    float m_texelSnappingBias;      // 0x160
    uint8_t m_alphaMode;            // 0x164 BLEND by default
    XMLSTRING m_file;               // 0x168
    XMLSTRING m_atlas;              // 0x178
    XMLSTRING m_mask;               // 0x188
};
static_assert(sizeof(XMLTEXTURE) == 0x198, "XMLTEXTURE is 0x198 bytes");

struct XMLLINE : XMLTEXTURE {
    uint32_t m_lineFlags;           // 0x198 1 thickness
    float m_thickness;              // 0x19C
};
static_assert(sizeof(XMLLINE) == 0x1a0, "XMLLINE is 0x1a0 bytes");

struct XMLFONTSTRING : XMLREGION {
    // 1 bytes, 2 maxLines, 4 text, 8/0x10 wordwrap, 0x20 rotation, 0x40 scale
    uint32_t m_fontStringFlags;     // 0x158
    XMLSTRING m_text;               // 0x160
    int32_t m_bytes;                // 0x170
    int32_t m_maxLines;             // 0x174
    float m_textScale;              // 0x178
    float m_rotation;               // 0x17C
};
static_assert(sizeof(XMLFONTSTRING) == 0x180, "XMLFONTSTRING is 0x180 bytes");

struct XMLBUTTON : XMLFRAME {
    uint32_t m_buttonFlags;         // 0x188 1 text, 2 registerForClicks, 4/8 motionScriptsWhileDisabled
    XMLSTRING m_text;               // 0x190
    XMLSTRING m_registerForClicks;  // 0x1A0
};
static_assert(sizeof(XMLBUTTON) == 0x1b0, "XMLBUTTON is 0x1b0 bytes");

struct XMLCHECKBUTTON : XMLBUTTON {
    uint32_t m_checkFlags;          // 0x1B0 1/2 checked
};
static_assert(sizeof(XMLCHECKBUTTON) == 0x1b8, "XMLCHECKBUTTON is 0x1b8 bytes");

// The value range Slider and StatusBar share.
struct XMLRANGE : XMLFRAME {
    // 1 orientation, 2 minValue, 4 maxValue, 8 defaultValue; Slider: 0x10 valueStep,
    // 0x20 stepsPerPage; StatusBar: 0x40/0x200 rotatesTexture, 0x80/0x400 reverseFill,
    // 0x100 fillStyle
    uint32_t m_rangeFlags;          // 0x188
    uint32_t m_orientation;         // 0x18C
    uint8_t m_fillStyle;            // 0x190 StatusBar
    float m_minValue;               // 0x194
    float m_maxValue;               // 0x198
    float m_defaultValue;           // 0x19C
};
static_assert(sizeof(XMLRANGE) == 0x1a0, "XMLRANGE is 0x1a0 bytes");

struct XMLSLIDER : XMLRANGE {
    uint32_t m_sliderFlags;         // 0x1A0 1 obeyStepOnDrag
    float m_valueStep;              // 0x1A4
    int32_t m_stepsPerPage;         // 0x1A8
    bool m_obeyStepOnDrag;          // 0x1AC
};
static_assert(sizeof(XMLSLIDER) == 0x1b0, "XMLSLIDER is 0x1b0 bytes");

struct XMLMODEL : XMLFRAME {
    uint32_t m_modelFlags;          // 0x188 1 file, 2 modelScale, 4 fogNear, 8 fogFar
    float m_modelScale;             // 0x18C
    float m_fogNear;                // 0x190
    float m_fogFar;                 // 0x194
    XMLSTRING m_file;               // 0x198
};
static_assert(sizeof(XMLMODEL) == 0x1a8, "XMLMODEL is 0x1a8 bytes");

// ModelFFX (1 glow) and CinematicModel (1/2 facingLeft).
struct XMLMODELEX : XMLMODEL {
    uint32_t m_modelExFlags;        // 0x1A8
    float m_glow;                   // 0x1AC
};
static_assert(sizeof(XMLMODELEX) == 0x1b0, "XMLMODELEX is 0x1b0 bytes");

struct XMLMESSAGEFRAME : XMLFRAME {
    uint32_t m_insertMode;          // 0x188 1 BOTTOM
    // 1/2 fade, 4 displayDuration, 8 fadeDuration, 0x10 fadePower, 0x20 insertMode
    uint32_t m_messageFlags;        // 0x18C
    float m_displayDuration;        // 0x190
    float m_fadeDuration;           // 0x194
    float m_fadePower;              // 0x198
};
static_assert(sizeof(XMLMESSAGEFRAME) == 0x1a0, "XMLMESSAGEFRAME is 0x1a0 bytes");

struct XMLMODELSCENE : XMLFRAME {
    uint32_t m_sceneFlags;          // 0x188 1 fogNear, 2 fogFar
    float m_fogNear;                // 0x18C
    float m_fogFar;                 // 0x190
};
static_assert(sizeof(XMLMODELSCENE) == 0x198, "XMLMODELSCENE is 0x198 bytes");

struct XMLCOOLDOWN : XMLFRAME {
    // 1/2 reverse, 4/8 drawSwipe, 0x10/0x20 drawEdge, 0x40/0x80 drawBling,
    // 0x100/0x200 hideCountdownNumbers, 0x400/0x800 useCircularEdge, 0x1000 rotation
    uint32_t m_cooldownFlags;       // 0x188
    float m_rotation;               // 0x18C
};
static_assert(sizeof(XMLCOOLDOWN) == 0x190, "XMLCOOLDOWN is 0x190 bytes");

struct XMLEDITBOX : XMLFRAME {
    uint32_t m_editFlags;           // 0x188
    float m_blinkSpeed;             // 0x18C
    int32_t m_historyLines;         // 0x190
    int32_t m_letters;              // 0x194
    int32_t m_invisibleBytes;       // 0x198
    int32_t m_bytes;                // 0x19C
    int32_t m_visibleBytes;         // 0x1A0
};
static_assert(sizeof(XMLEDITBOX) == 0x1a8, "XMLEDITBOX is 0x1a8 bytes");

// BlobFrame, QuestPOIFrame, ArchaeologyDigSiteFrame, ScenarioPOIFrame.
struct XMLBLOBFRAME : XMLFRAME {
    uint32_t m_blobFlags;           // 0x188 1 filltexture, 2 bordertexture
    XMLSTRING m_fillTexture;        // 0x190
    XMLSTRING m_borderTexture;      // 0x1A0
};
static_assert(sizeof(XMLBLOBFRAME) == 0x1b0, "XMLBLOBFRAME is 0x1b0 bytes");

struct XMLMINIMAP : XMLFRAME {
    uint32_t m_minimapFlags;        // 0x188 one bit per texture
    XMLSTRING m_questBlobInside;            // 0x190
    XMLSTRING m_questBlobOutside;           // 0x1A0
    XMLSTRING m_questBlobOutsideSelected;   // 0x1B0
    XMLSTRING m_questBlobRing;              // 0x1C0 also every archBlob* texture
    XMLSTRING m_taskBlobInside;             // 0x1D0
    XMLSTRING m_taskBlobOutside;            // 0x1E0
    XMLSTRING m_taskBlobOutsideSelected;    // 0x1F0
    XMLSTRING m_taskBlobRing;               // 0x200
    uint8_t m_unknown210[0x30];             // 0x210
};
static_assert(sizeof(XMLMINIMAP) == 0x240, "XMLMINIMAP is 0x240 bytes");

struct XMLSIMPLEHTML : XMLFRAME {
    uint32_t m_htmlFlags;           // 0x188 1/2 resizeToFitContents, 4 file, 8 hyperlinkFormat
    XMLSTRING m_hyperlinkFormat;    // 0x190
    XMLSTRING m_file;               // 0x1A0
};
static_assert(sizeof(XMLSIMPLEHTML) == 0x1b0, "XMLSIMPLEHTML is 0x1b0 bytes");

// Browser and Checkout.
struct XMLBROWSER : XMLFRAME {
    uint32_t m_browserFlags;        // 0x188 1 IMEFont
    XMLSTRING m_imeFont;            // 0x190
};
static_assert(sizeof(XMLBROWSER) == 0x1a0, "XMLBROWSER is 0x1a0 bytes");

// The arena elements are built in (0x48): 32 KiB blocks for the records, others for strings.
struct XMLBUILDER {
    char* m_block;                  // 0x00 the record block in use
    char* m_cursor;                 // 0x08 where the next record goes
    char* m_end;                    // 0x10
    char* m_strings;                // 0x18 where the next string goes
    char* m_stringsEnd;             // 0x20
    // TSGrowableArray<void*, 32>: every block, freed together
    uint32_t m_blockCount;          // 0x28
    void** m_blocks;                // 0x30
    uint32_t m_blockCapacity;       // 0x38
    uint32_t m_pad3C;
    uint32_t m_blockGranularity;    // 0x40
};
static_assert(sizeof(XMLBUILDER) == 0x48, "XMLBUILDER is 0x48 bytes");

// Parses one element of a known kind; true if it was recognised.
typedef bool (*XMLCHILDPARSER)(XMLOBJECT* object, XMLBUILDER* builder, const char* name);

// Adds `count` blocks to the builder's block list.
void XMLBuilderAddBlocks(XMLBUILDER* builder, uint32_t count, void* const* blocks);
// A new block of `bytes`, owned by the builder.
char* XMLBuilderAllocBlock(XMLBUILDER* builder, uint64_t bytes);
// Makes sure the record block has room for one more element.
bool XMLBuilderReserve(XMLBUILDER* builder);

// Copies `text` into the builder's strings and makes `slot` (a field of `object`) own it.
bool XMLObjectAddString(XMLOBJECT* object, XMLBUILDER* builder, XMLSTRING* slot, const char* text);
// Builds a record for each child node `parser` recognises and appends it to the children.
bool XMLObjectParseChildren(XMLOBJECT* object, XMLCHILDPARSER parser, XMLBUILDER* builder);
// The first child of a type, or null.
XMLOBJECT* XMLObjectFindChild(XMLOBJECT* object, uint32_t type);

// The bytes the tree needs compacted.
uint64_t XMLObjectGetCompactSize(XMLOBJECT* object);
// Moves the tree to *dest, each record followed by its strings; advances *dest.
bool XMLObjectCompact(XMLOBJECT* object, char** dest);
// Compacts a tree into one new block; null for an empty tree.
XMLOBJECT* XMLObjectCompile(XMLOBJECT* object, uint64_t* size);
// Releases the script handlers' function references (skipped for registry-owned trees
// unless `force`).
void XMLObjectReleaseScripts(XMLOBJECT* object, bool force);
// Releases and frees a compiled tree.
void XMLObjectFree(XMLOBJECT* object, bool force);

// A <Ui> document root, built into `builder`.
XMLOBJECT* XMLObjectCreateDocument(XMLNode* root, XMLBUILDER* builder);
// A single node parsed as a frame, compiled into its own block.
XMLOBJECT* XMLObjectCreateFrame(XMLNode* node);

// Element parsers shared with the loader.
bool XMLParseTopLevel(XMLOBJECT* object, XMLBUILDER* builder, const char* name);
bool XMLParseFrameType(XMLOBJECT* object, XMLBUILDER* builder, const char* name);

// The element getters the frame loaders read through. Positions are in UI units (the
// reference's 1024-unit tall screen, scaled to normalized coordinates); "rel" values are
// fractions and are not scaled.
bool XMLInsetGetAbs(XMLINSET* inset, float* left, float* right, float* top, float* bottom);
bool XMLInsetGet(XMLINSET* inset, float* left, float* right, float* top, float* bottom, CStatus* status);
bool XMLAnchorGet(XMLANCHOR* anchor, uint32_t* point, uint32_t* relativePoint, float* x, float* y, CStatus* status);
bool XMLAnchorGetCentered(XMLANCHOR* anchor, uint32_t* point, uint32_t* relativePoint, float* x, float* y, CStatus* status);
bool XMLColorGet(XMLCOLOR* color, uint32_t* argb);
bool XMLDimensionGet(XMLDIMENSION* dimension, float* x, float* y, CStatus* status);
// An element's own x/y unscaled, unless it has an <Offset> child: then the inverse of
// XMLDimensionGet on that child (as the reference returns it).
bool XMLOffsetGet(XMLDIMENSION* dimension, float* x, float* y, CStatus* status);
// The packed colour of an XML colour, set or not.
uint32_t XmlColorPack(const XMLCOLOR* color);
bool XMLGradientGet(XMLGRADIENT* gradient, uint32_t* orientation, uint32_t* minColor, uint32_t* maxColor);
bool XMLValueGet(XMLVALUE* value, float* result, CStatus* status);

#endif
