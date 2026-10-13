#include "ui/CSimpleTexture.hpp"
#include "base/Coordinate.hpp"

#include <cmath>
#include <cstring>
#include <new>

#include "base/DataAllocator.hpp"
#include "base/Status.hpp"
#include "cas/CAS.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"
extern "C" {
#include "lauxlib.h"
#include "lua.h"
}
#include "os/Os.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "texture/Texture.hpp"
#include "ui/BinaryXML.hpp"
#include "ui/CRenderBatch.hpp"
#include "ui/CSimpleFrame.hpp"
#include "ui/CSimpleTop.hpp"
#include "ui/FrameXML.hpp"
#include "ui/TextureAtlas.hpp"
#include "util/Unimplemented.hpp"

const char s_maskedTextureTag[] = ".PEAVCSimpleTexture@@";
const char s_maskTextureTag[] = ".PEAVCSimpleMaskTexture@@";
const char s_textureListTag[] = ".PEAVCTexture@@";

int64_t CSimpleTexture::s_count;
int32_t CSimpleTexture::s_metaTable = -2;
int32_t CSimpleTexture::s_restrictedMetaTable = -2;
uint8_t CSimpleTexture::s_drawWhileLoading;

void CSimpleTexture_FillScriptMethods(lua_State* L);
void CSimpleMaskTexture_FillScriptMethods(lua_State* L);
void CSimpleMaskTexture_AddMasked(CSimpleMaskTexture* mask, CSimpleTexture* texture);
void CSimpleMaskTexture_RemoveMasked(CSimpleMaskTexture* mask, CSimpleTexture* texture);

namespace {

const char s_extensionTag[] = ".?AUCTextureExtension@@";
const char s_argbArrayTag[] = ".?AV?$TSGrowableArray@VCArgb@@$07@@";

// The pools: textures (0x142e99d70) and their extensions (0x142e99d30).
CDataAllocator s_textureHeap(0x2a0, 100);
CDataAllocator s_extensionHeap(0xf8, 100);

// No masks (DAT_142e99d58).
blz::vector<CSimpleMaskTexture*, s_maskTextureTag> s_noMasks;

// A texture that cannot be loaded shows green.
const uint32_t ERROR_COLOR = 0xff00ff00;

// The texels across the screen at scale 1 (DAT_142ccb9fc * DAT_14261a9c4), against the UI's
// width (DAT_142ccb9f4).
float TexelsPerScreen() {
    return g_coordinateSpace.m_aspectCompensation * 1024.0f;
}
const float& UI_WIDTH = g_coordinateSpace.m_diagonalWidth;

const float DEFAULT_TEX_COORDS[8] = { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f };

// The quad's two triangles (DAT_142cf54c0).
const uint16_t s_quadIndices[6] = { 0, 1, 2, 2, 1, 3 };

}  // namespace

// A path's FileDataID, tried again with ".blp" (FUN_1406380e0).
// ref: FUN_1406380e0
int32_t* TextureFileId(int32_t* id, const char* name) {
    if (name == nullptr) {
        *id = 0;
        return id;
    }
    int32_t found = cas::CASFileGetIdByName(name, 1);
    if (found == 0) {
        char path[0x30c];
        OsPathChangeExtension(path, sizeof(path), name, "blp");
        found = cas::CASFileGetIdByName(path, 1);
    }
    *id = found;
    return id;
}

namespace {

uint32_t FileDataIdOf(const char* name) {
    int32_t id = 0;
    TextureFileId(&id, name);
    return static_cast<uint32_t>(id);
}

// Textures of hidden frames are let go (FUN_14026bea0, which is always true here).
bool ReleasesHiddenTextures() {
    return true;
}

// The bounded copy the reference inlines.
void CopyBounded(char* dest, const char* source, uint64_t size) {
    uint64_t i = 0;
    for (; i + 1 < size && source[i] != '\0'; i++) {
        dest[i] = source[i];
    }
    dest[i] = '\0';
}

// The bounded append the reference inlines.
void Append(char* dest, const char* source, uint64_t size) {
    uint64_t length = std::strlen(dest);
    while (*source != '\0' && length + 1 < size) {
        dest[length++] = *source++;
    }
    dest[length] = '\0';
}

// Room for `count` pixels (FUN_140634630): chunks of the highest power of two not above the
// count, 64 at most.
// ref: FUN_140634630
void ArgbArrayReserve(ARGBARRAY* array, uint32_t count) {
    if (count <= array->m_capacity) {
        return;
    }
    uint32_t chunk = array->m_chunk;
    if (chunk == 0) {
        if (count < 0x40) {
            chunk = count;
            for (uint32_t bits = (count - 1) & count; bits != 0; bits = (bits - 1) & bits) {
                chunk = bits;
            }
            if (chunk == 0) {
                chunk = 1;
            }
        } else {
            chunk = 0x40;
            array->m_chunk = 0x40;
        }
    }
    uint32_t rest = count % chunk;
    if (rest != 0) {
        count += chunk - rest;
    }
    uint32_t* old = array->m_data;
    array->m_capacity = count;
    array->m_data = nullptr;
    if (count != 0) {
        array->m_data = static_cast<uint32_t*>(SMemAlloc(static_cast<uint64_t>(count) * 4, 8, s_argbArrayTag, 0, -7, 0));
    }
    if (old != nullptr) {
        uint32_t keep = array->m_count <= count ? array->m_count : count;
        std::memcpy(array->m_data, old, static_cast<uint64_t>(keep) * 4);
        SMemFree(old, s_argbArrayTag, -2, 0);
    }
}

// The raw pixels' writer: a pitch of width * 4, the array's data (0x1406373c0).
void PixelFill(uint32_t width, uint32_t height, uint32_t slice, uint32_t mip, uint32_t* pitch, const void** data,
    void* storage) {
    (void)height;
    (void)slice;
    (void)mip;
    *pitch = width * 4;
    *data = (*static_cast<ARGBARRAY**>(storage))->m_data;
}

void TextureMakePixelFill(blz::function_storage* fill, ARGBARRAY* pixels) {
    fill->m_invoke = reinterpret_cast<void*>(&PixelFill);
    fill->m_manager = &blz::function_trivial_manager;
    std::memcpy(fill->m_storage, &pixels, sizeof(pixels));
}

// The sine and cosine of an angle (FUN_14240ad20).
void SinCos(float radians, float* sine, float* cosine) {
    *sine = std::sin(radians);
    *cosine = std::cos(radians);
}

// A point of the corners rotated about `center`: x' = x cos - y sin (FUN_140637e30).
// ref: FUN_140637e30
void RotatePoint(float* out, const float* point, float sine, float cosine, const float* center) {
    float z = point[2];
    float dy = point[1] - center[1];
    float dx = point[0] - center[0];
    out[0] = (dx * cosine - dy * sine) + center[0];
    out[1] = dy * cosine + dx * sine + center[1];
    out[2] = z;
}

// Rotates the four corners by `radians` about the origin point (FUN_140637990).
// ref: FUN_140637990
void RotatePositions(float* positions, int32_t point, const float* offset, float radians) {
    float origin[2] = { 0.0f, 0.0f };
    TextureAnimOrigin(origin, point, offset, positions);
    float sine;
    float cosine;
    SinCos(radians, &sine, &cosine);
    for (uint32_t i = 0; i < 4; i++) {
        float x = positions[i * 3] - origin[0];
        float y = positions[i * 3 + 1] - origin[1];
        positions[i * 3] = (x * cosine - y * sine) + origin[0];
        positions[i * 3 + 1] = x * sine + y * cosine + origin[1];
    }
}

// Scales the corners about the origin point, clamped to +-1e10 (FUN_140637b30).
// ref: FUN_140637b30
void ScalePositions(float* positions, int32_t point, const float* offset, const float* scale) {
    float origin[2] = { 0.0f, 0.0f };
    TextureAnimOrigin(origin, point, offset, positions);
    for (uint32_t i = 0; i < 4; i++) {
        positions[i * 3] = (origin[0] - positions[i * 3]) * scale[0] + positions[i * 3];
        positions[i * 3 + 1] = (origin[1] - positions[i * 3 + 1]) * scale[1] + positions[i * 3 + 1];
    }
    for (uint32_t i = 0; i < 4; i++) {
        for (uint32_t axis = 0; axis < 2; axis++) {
            float value = positions[i * 3 + axis];
            if (value < -1e10f) {
                value = -1e10f;
            } else if (1e10f <= value) {
                value = 1e10f;
            }
            positions[i * 3 + axis] = value;
        }
    }
}

// Moves the corners (FUN_140637db0).
// ref: FUN_140637db0
void TranslatePositions(float* positions, const float* move) {
    for (uint32_t i = 0; i < 4; i++) {
        positions[i * 3] += move[0];
        positions[i * 3 + 1] += move[1];
    }
}

// The mask's coordinates for corners given relative to the mask (FUN_140632450): divided by
// the mask's extent when it has one, the v axis flipped.
// ref: FUN_140632450
void MaskTexCoords(float* out, const float* corners, const float* extent) {
    bool scaled = std::fabs(extent[0]) > 1.17549435e-38f * 0x100000 && std::fabs(extent[1]) > 1.17549435e-38f * 0x100000;
    scaled = !(std::fabs(extent[0]) <= 6.0e-36f || std::fabs(extent[1]) <= 6.0e-36f);
    for (uint32_t i = 0; i < 4; i++) {
        if (scaled) {
            out[i * 2] = corners[i * 2] / extent[0];
            out[i * 2 + 1] = 1.0f - corners[i * 2 + 1] / extent[1];
        } else {
            out[i * 2] = corners[i * 2];
            out[i * 2 + 1] = 1.0f - corners[i * 2 + 1];
        }
    }
}

// The Gx flags and create flags a texture's settings make.
uint64_t TextureGxFlags(uint32_t flags) {
    return ((flags & 0x1c00) | 0x20800000) >> 10 | (flags & 0x3f0);
}

uint32_t TextureCreateFlags(uint32_t flags) {
    return ((flags & 0x10000) | 0x3800) >> 0xb;
}

}

// ref: FUN_140637eb0
void TextureAnimOrigin(float* origin, int32_t point, const float* offset, const float* positions) {
    switch (point) {
    case 0:
        origin[0] = positions[0];
        origin[1] = positions[1];
        break;
    case 1:
        origin[0] = (positions[6] - positions[0]) * 0.5f + positions[0];
        origin[1] = (positions[7] - positions[1]) * 0.5f + positions[1];
        break;
    case 2:
        origin[0] = positions[6];
        origin[1] = positions[7];
        break;
    case 3:
        origin[0] = (positions[3] - positions[0]) * 0.5f + positions[0];
        origin[1] = (positions[4] - positions[1]) * 0.5f + positions[1];
        break;
    case 4:
        origin[0] = (positions[9] - positions[0]) * 0.5f + positions[0];
        origin[1] = (positions[10] - positions[1]) * 0.5f + positions[1];
        break;
    case 5:
        origin[0] = (positions[9] - positions[6]) * 0.5f + positions[6];
        origin[1] = (positions[10] - positions[7]) * 0.5f + positions[7];
        break;
    case 6:
        origin[0] = positions[3];
        origin[1] = positions[4];
        break;
    case 7:
        origin[0] = (positions[9] - positions[3]) * 0.5f + positions[3];
        origin[1] = (positions[10] - positions[4]) * 0.5f + positions[4];
        break;
    case 8:
        origin[0] = positions[9];
        origin[1] = positions[10];
        break;
    default:
        origin[0] = 0.0f;
        origin[1] = 0.0f;
        break;
    }
    float x = offset[0];
    float y = offset[1];
    if (1.1920929e-07f < x * x + y * y) {
        float axisX = positions[9] - positions[3];
        float axisY = positions[10] - positions[4];
        float length = axisY * axisY + axisX * axisX;
        if (2.3841858e-07f < length) {
            float inverse = 1.0f / std::sqrt(length);
            axisX *= inverse;
            axisY *= inverse;
        }
        origin[0] = (x * axisX - y * axisY) + origin[0];
        origin[1] = x * axisY + y * axisX + origin[1];
    }
}

// ---- lifetime ------------------------------------------------------------------------------------

CSimpleTexture::CSimpleTexture(CSimpleFrame* frame, uint8_t drawLayer, int8_t subLevel, bool show)
    : CSimpleRegion(frame, drawLayer, subLevel, show) {
    this->m_blendMode = 2;
    this->m_extension = nullptr;
    this->m_texture = nullptr;
    this->m_atlas = 0;
    this->m_fileDataId = 0;
    std::memset(this->m_positions, 0, sizeof(this->m_positions));
    this->m_desaturation = 0.0f;
    this->m_snappingBias = 0.3f;    // DAT_142618d5c
    this->m_rotation = 0.0f;
    this->m_rotationCenter[0] = 0.0f;
    this->m_rotationCenter[1] = 0.0f;
    std::memset(this->m_texCoords, 0, sizeof(this->m_texCoords));
    this->m_textureFlags = (this->m_textureFlags & 0xfffe0400) | 0x20400;
    s_count++;
    if (this->m_atlas == 0) {
        this->StoreTexCoords(DEFAULT_TEX_COORDS);
    } else {
        float texCoords[8] = {};
        TextureAtlasMapTexCoords(&this->m_atlas, texCoords, DEFAULT_TEX_COORDS);
        this->StoreTexCoords(texCoords);
    }
}

// ref: FUN_140631c70
CSimpleTexture::~CSimpleTexture() {
    if (this->m_texture != nullptr) {
        UiReleaseTexture(&this->m_texture);
        CSimpleTop::s_instance->RemoveLayoutWaiting(this);
        this->MarkFrameBatchDirty();
    }
    if (this->m_extension != nullptr) {
        // FUN_140631d80: the pixel texture, the mask lists and the pixels.
        CTextureExtension* extension = this->m_extension;
        if (extension->m_pixelTexture != nullptr) {
            TextureFree(&extension->m_pixelTexture);
        }
        extension->m_masks.~vector();
        extension->m_masked.~vector();
        if (extension->m_pixels.m_data != nullptr) {
            SMemFree(extension->m_pixels.m_data, s_argbArrayTag, -2, 0);
        }
        s_extensionHeap.PutData(extension);
    }
    s_count--;
}

void* CSimpleTexture::operator new(size_t size) {
    (void)size;
    return s_textureHeap.GetData(false, ".?AVCSimpleTexture@@", -2, 0x1f);
}

void CSimpleTexture::operator delete(void* texture) {
    if (texture != nullptr) {
        s_textureHeap.PutData(texture);
    }
}

// ref: FUN_1406325e0
void CSimpleTexture::CreateScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    s_metaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleTexture_FillScriptMethods, false);
    s_restrictedMetaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleTexture_FillScriptMethods, true);
}

// ref: FUN_140632630
void CSimpleTexture::DestroyScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    luaL_unref(L, LUA_REGISTRYINDEX, s_metaTable);
    luaL_unref(L, LUA_REGISTRYINDEX, s_restrictedMetaTable);
    s_metaTable = -2;
    s_restrictedMetaTable = -2;
}

// ---- FrameScript_Object and CScriptRegion ---------------------------------------------------------

// ref: FUN_140632de0
int32_t CSimpleTexture::GetScriptMetaTable() {
    if ((this->FrameScript_Object::m_flags & 1) == 0 && (this->FrameScript_Object::m_flags & 4) == 0) {
        return s_metaTable;
    }
    return s_restrictedMetaTable;
}

// ref: FUN_140633210
bool CSimpleTexture::IsObjectTypeName(const char* name) {
    return _stricmp(name, "Texture") == 0 || _stricmp(name, "Region") == 0 || _stricmp(name, "Object") == 0
        || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_140616420
// The reference's type id and its magic-static guard (CSimpleTexture), held here so translated code
// and the port number the class once, alike.
int32_t g_cSimpleTextureType;  // DAT_142e97b60
int32_t g_cSimpleTextureTypeGuard;  // DAT_142e97b64

int32_t CSimpleTexture::GetObjectType() {
    return FrameScript_TypeId(&g_cSimpleTextureType, &g_cSimpleTextureTypeGuard);
}

// ref: FUN_140633110
bool CSimpleTexture::IsObjectType(int32_t type) {
    return type == CSimpleTexture::GetObjectType() || type == CScriptRegion::GetObjectType() || this->CScriptObject::IsObjectType(type);
}

// ref: FUN_140632d90
const char* CSimpleTexture::GetObjectTypeName() {
    return "Texture";
}

// ref: FUN_140632d60
uint32_t CSimpleTexture::GetObjectSize() {
    return 0x2a0;
}

// "Texture <name> " with the source: + and the path, - and the file, or ?.
// ref: FUN_1406326c0
void CSimpleTexture::DumpHierarchy(uint32_t indent, HIERARCHYDUMPFUNC dump, void* context) {
    char line[0x118];
    if (indent != 0) {
        std::memset(line, '.', indent);
    }
    line[indent] = '\0';
    alignas(8) uint8_t storage[sizeof(blz::string)];
    blz::string* name = this->GetDebugName(reinterpret_cast<blz::string*>(storage), false);
    SStrPrintf(line + indent, sizeof(line) - indent, "Texture %s ", name->m_data);
    name->~string();
    char path[0x110];
    if (this->m_texture != nullptr) {
        Append(line, "+", sizeof(line));
        Append(line, TextureGetName(this->m_texture, path, 0x104), sizeof(line));
    } else if (this->m_fileDataId == 0) {
        Append(line, "?", sizeof(line));
    } else {
        Append(line, "-", sizeof(line));
        char file[0x110];
        if (this->GetTextureName(file, 0x104)) {
            Append(line, file, sizeof(line));
        } else {
            Append(line, "?", sizeof(line));
        }
    }
    Append(line, "\n", sizeof(line));
    dump(line, static_cast<int32_t>(std::strlen(line)), context);
}

// The texture joins a list of textures (blz::vector<CTexture*>).
// ref: FUN_140632a80
void CSimpleTexture::Unknown98(void* argument) {
    if (this->m_texture == nullptr) {
        return;
    }
    auto list = static_cast<blz::vector<CTexture*, s_textureListTag>*>(argument);
    list->push_back(this->m_texture);
}

// The region's settings, then the texture's: tiling, wrap, filter, colour, gradient, coordinates,
// the mask file, the file or atlas, the blend mode, alpha, desaturation, snapping.
// ref: FUN_140633400
void CSimpleTexture::LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) {
    auto xml = static_cast<XMLTEXTURE*>(node);
    if ((node->m_attributes & 0x100) != 0) {
        char inherits[0x80];
        CopyBounded(inherits, node->m_inherits.m_text, sizeof(inherits));
        const char* taint = nullptr;
        bool busy = true;
        XMLOBJECT* parent = FrameXML_AcquireVirtual(inherits, &taint, &busy);
        if (parent == nullptr) {
            status->Add(1, "Couldn't find inherited node: %s", inherits);
        } else if (busy) {
            status->Add(1, "Recursively inherited node: %s", inherits);
        } else {
            this->LoadXML(parent, status, nullptr);
        }
        if (parent != nullptr && !busy) {
            FrameXML_ReleaseVirtual(inherits);
        }
    }
    this->CSimpleRegion::LoadXML(node, status, intrinsic);
    if ((node->m_flags & 0x200000) == 0) {
        return;
    }
    uint32_t xmlFlags = xml->m_textureFlags;
    uint32_t& flags = this->m_textureFlags;
    uint8_t wrapH = 0;
    uint8_t wrapV = 0;
    int32_t color = 0;
    if ((xmlFlags >> 0x16) & 1) {
        flags ^= ((xmlFlags >> 8) ^ flags) & 0x8000;
    }
    if ((xmlFlags >> 0x12) & 1) {
        flags ^= ((xmlFlags >> 7) ^ flags) & 0x2000;
    }
    if ((xmlFlags >> 0x13) & 1) {
        flags ^= ((xmlFlags >> 7) ^ flags) & 0x4000;
    }
    if ((xmlFlags >> 9) & 1) {
        uint32_t tile = (xmlFlags & 0x1c00) == 0x400 ? 4 : 0;
        wrapH = static_cast<uint8_t>((xmlFlags >> 10) & 7);
        flags = (static_cast<uint32_t>(wrapH) << 4) | tile | (flags & 0xffffff8b);
    }
    if ((xmlFlags & 0x100) != 0) {
        wrapH = static_cast<uint8_t>((xmlFlags >> 10) & 7);
        flags = (static_cast<uint32_t>(wrapH) << 4) | (flags & 0xffffff8b);
    }
    if ((xmlFlags >> 0xd) & 1) {
        uint32_t tile = (xmlFlags & 0x38000) == 0x8000 ? 8 : 0;
        wrapV = static_cast<uint8_t>((xmlFlags >> 0xf) & 7);
        flags = (static_cast<uint32_t>(wrapV) << 7) | tile | (flags & 0xfffffc77);
    }
    if ((xmlFlags & 0x4000) != 0) {
        wrapV = static_cast<uint8_t>((xmlFlags >> 0xf) & 7);
        flags = (static_cast<uint32_t>(wrapV) << 7) | (flags & 0xfffffc77);
    }
    if ((xml->m_regionFlags & 0x20) != 0) {
        uint8_t ignore = static_cast<uint8_t>(xml->m_regionFlags >> 6);
        if (((this->m_regionFlags >> 1 ^ ignore) & 1) != 0) {
            this->m_regionFlags = static_cast<uint8_t>(((ignore * 2) ^ this->m_regionFlags) & 2 ^ this->m_regionFlags);
            this->UpdateEffectiveAlpha();
        }
    }
    if ((xmlFlags & 0x10) != 0) {
        if ((xmlFlags & 0x20) == 0) {
            this->Show();
        } else {
            this->Hide();
        }
    }
    if ((xmlFlags & 0x40) != 0) {
        flags = (static_cast<uint32_t>(static_cast<uint8_t>(~static_cast<uint8_t>(xmlFlags >> 7))) ^ flags) & 1 ^ flags;
    }
    if ((xmlFlags & 0x10000000) != 0
        && (xml->m_rotation != this->m_rotation || this->m_rotationCenter[0] != 0.5f || this->m_rotationCenter[1] != 0.5f)) {
        this->m_rotation = xml->m_rotation;
        this->m_rotationCenter[0] = 0.5f;
        this->m_rotationCenter[1] = 0.5f;
        if ((this->CLayoutFrame::m_flags & 2) == 0) {
            this->UpdatePositions(&this->m_rect);
        }
    }
    if ((xml->m_regionFlags & 0x200) != 0) {
        this->SetScale(xml->m_scale);
    }
    bool clampU = false;
    bool clampV = false;
    for (XMLOBJECT* child = node->m_child; child != nullptr; child = child->m_next) {
        if (child->m_type == XMLTYPE_COLOR) {
            color = static_cast<int32_t>(XmlColorPack(static_cast<XMLCOLOR*>(child)));
            if ((node->m_attributes & 0x100) == 0 || this->m_texture == nullptr) {
                if ((xmlFlags & 6) == 0) {
                    uint32_t argb = static_cast<uint32_t>(color);
                    this->SetColorTexture(&argb);
                }
            } else {
                uint32_t argb = static_cast<uint32_t>(color);
                this->CSimpleRegion::SetVertexColor(&argb);
            }
        } else if (child->m_type == XMLTYPE_GRADIENT) {
            uint32_t orientation = 0;
            uint32_t minColor = 0;
            uint32_t maxColor = 0;
            XMLGradientGet(static_cast<XMLGRADIENT*>(child), &orientation, &minColor, &maxColor);
            this->SetGradient(static_cast<int32_t>(orientation), &minColor, &maxColor);
        } else if (child->m_type == XMLTYPE_TEXCOORDS) {
            auto coords = static_cast<XMLTEXCOORDS*>(child);
            float ulx = 0.0f;
            float uly = 0.0f;
            float llx = 0.0f;
            float lly = 1.0f;
            float urx = 1.0f;
            float ury = 0.0f;
            float lrx = 1.0f;
            float lry = 1.0f;
            bool hTileBad = false;
            bool vTileBad = false;
            auto rect = static_cast<XMLRECT*>(XMLObjectFindChild(child, XMLTYPE_RECT));
            if (rect != nullptr) {
                uint32_t rectFlags = rect->m_rectFlags;
                if (rectFlags & 1) {
                    ulx = rect->m_ulx;
                }
                if (rectFlags & 2) {
                    uly = rect->m_uly;
                }
                if (rectFlags & 0x10) {
                    llx = rect->m_llx;
                }
                if (rectFlags & 0x20) {
                    lly = rect->m_lly;
                }
                if (rectFlags & 4) {
                    urx = rect->m_urx;
                }
                if (rectFlags & 8) {
                    ury = rect->m_ury;
                }
                if (rectFlags & 0x40) {
                    lrx = rect->m_lrx;
                }
                if (rectFlags & 0x80) {
                    lry = rect->m_lry;
                }
            }
            uint32_t texFlags = coords->m_texFlags;
            if (texFlags & 1) {
                hTileBad = (flags & 4) != 0 && coords->m_left != 0.0f;
                llx = coords->m_left;
                ulx = llx;
            }
            if (texFlags & 2) {
                if ((flags & 4) != 0 && coords->m_right != 1.0f) {
                    hTileBad = true;
                }
                lrx = coords->m_right;
                urx = lrx;
            }
            if (texFlags & 4) {
                vTileBad = (flags & 8) != 0 && coords->m_top != 0.0f;
                ury = coords->m_top;
                uly = ury;
            }
            if (texFlags & 8) {
                if ((flags & 8) != 0 && coords->m_bottom != 1.0f) {
                    vTileBad = true;
                }
                lry = coords->m_bottom;
                lly = lry;
            }
            const float values[8] = { ulx, uly, llx, lly, urx, ury, lrx, lry };
            bool inRange = true;
            for (float value : values) {
                if (value < -10000.0f || 10000.0f < value) {
                    inRange = false;
                }
            }
            const char* name = this->GetName();
            if (name == nullptr) {
                name = this->GetObjectTypeName();
            }
            if (hTileBad) {
                status->Add(2, "Texture %s: Invalid TexCoords value (horizTile is on)", name);
            } else if (vTileBad) {
                status->Add(2, "Texture %s: Invalid TexCoords value (vertTile is on)", name);
            } else if (inRange) {
                const float texCoords[8] = { ulx, uly, llx, lly, urx, ury, lrx, lry };
                if (this->m_atlas == 0) {
                    this->StoreTexCoords(texCoords);
                } else {
                    float mapped[8] = {};
                    TextureAtlasMapTexCoords(&this->m_atlas, mapped, texCoords);
                    this->StoreTexCoords(mapped);
                }
                if (ulx < 0.0f || 1.0f < ulx || llx < 0.0f || 1.0f < llx || urx < 0.0f || 1.0f < urx || lrx < 0.0f
                    || 1.0f < lrx) {
                    clampU = true;
                }
                if (uly < 0.0f || 1.0f < uly || lly < 0.0f || 1.0f < lly || ury < 0.0f || 1.0f < ury || lry < 0.0f
                    || 1.0f < lry) {
                    clampV = true;
                }
            } else {
                status->Add(2, "Texture %s: Invalid rect value (out of range)", name);
            }
        }
    }
    uint8_t wrapU = clampU ? 1 : wrapH;
    uint8_t wrapVV = clampV ? 1 : wrapV;
    (void)wrapVV;
    if ((xmlFlags & 1) != 0) {
        this->EnsureExtension();
        this->m_extension->m_flags |= 1;
        this->m_extension->m_maskFile = static_cast<int32_t>(FileDataIdOf(xml->m_mask.m_text));
    }
    const char* name = this->GetName();
    if (name == nullptr) {
        name = this->GetObjectTypeName();
    }
    if ((xmlFlags & 2) != 0) {
        if ((xmlFlags & 4) == 0) {
            const char* file = xml->m_file.m_text;
            if (!this->SetTextureByName(file, clampU ? 1 : wrapU, clampV ? 1 : wrapV, 1, true, false)) {
                status->Add(1, "Texture %s: Unable to load texture file %s", name, file);
                uint32_t error = ERROR_COLOR;
                this->SetColorTexture(&error);
            } else if (color != 0) {
                uint32_t argb = static_cast<uint32_t>(color);
                this->CSimpleRegion::SetVertexColor(&argb);
            }
        } else {
            status->Add(1, "Texture %s: Has both atlas and file name. Using atlas.", name);
        }
    }
    if ((xmlFlags & 4) != 0) {
        const char* atlasName = xml->m_atlas.m_text;
        int32_t atlas = 0;
        TEXTUREATLASSCALE scale = { CSimpleTop::s_instance->GetRegionLayoutVersion(this), 1 };
        TextureAtlasFindByName(&atlas, atlasName, &scale);
        if (!this->SetAtlas(&atlas, 1, false, clampU ? 1 : wrapH, clampV ? 1 : wrapV)) {
            status->Add(1, "Texture %s: Unable to load atlas entry %s", name, atlasName);
            uint32_t error = ERROR_COLOR;
            this->SetColorTexture(&error);
        } else {
            if (color != 0) {
                uint32_t argb = static_cast<uint32_t>(color);
                this->CSimpleRegion::SetVertexColor(&argb);
            }
            if ((xmlFlags & 0x8000000) != 0) {
                float size[2] = {};
                TextureAtlasGetSize(&this->m_atlas, size);
                float toUi = 1.0f / TexelsPerScreen();
                this->SetSize(size[0] * toUi * UI_WIDTH, size[1] * toUi * UI_WIDTH);
            }
        }
    }
    if ((xmlFlags & 8) != 0) {
        this->SetBlendMode(xml->m_alphaMode);
    }
    if ((xml->m_regionFlags & 1) != 0) {
        float alpha = xml->m_alpha;
        if (alpha < 0.0f) {
            alpha = 0.0f;
        } else if (1.0f <= alpha) {
            alpha = 1.0f;
        }
        this->SetAlpha(static_cast<uint8_t>(static_cast<int32_t>(alpha * 255.0f)));
    }
    if ((xmlFlags >> 0x18) & 1) {
        this->SetDesaturation(((xmlFlags >> 0x19) & 1) != 0 ? 1.0f : 0.0f);
    }
    if ((xmlFlags >> 0x1d) & 1) {
        flags = ((xmlFlags >> 0xd) ^ flags) & 0x20000 ^ flags;
    }
    if ((xmlFlags & 0x4000000) != 0) {
        this->SetSnappingBias(xml->m_texelSnappingBias);
    }
}

// Unanchored, the texture fills its frame.
// ref: FUN_140634350
void CSimpleTexture::PostLoadXML(XMLOBJECT* node, CStatus* status) {
    if (this->m_parent != nullptr && !this->IsAnchored()) {
        this->SetAllPoints(this->m_parent, true);
    }
    this->CScriptRegion::PostLoadXML(node, status);
}

// The atlas entry for the new layout scale.
// ref: FUN_140634080
void CSimpleTexture::OnLayoutVersionChanged() {
    if (this->m_atlas == 0) {
        return;
    }
    int32_t element = TextureAtlasGetElement(&this->m_atlas);
    if (element == 0) {
        return;
    }
    uint32_t flags = this->m_textureFlags;
    TEXTUREATLASSCALE scale = { CSimpleTop::s_instance->GetRegionLayoutVersion(this), 1 };
    int32_t atlas = 0;
    TextureAtlasFindByElement(&atlas, element, &scale);
    this->SetAtlas(&atlas, static_cast<uint8_t>((flags >> 10) & 7), false, static_cast<uint8_t>((flags >> 4) & 7),
        static_cast<uint8_t>((flags >> 7) & 7));
}

// ---- CSimpleRegion ---------------------------------------------------------------------------------

// A blend mode that alpha changes into BLEND rebuilds the batch.
// ref: FUN_140634130
void CSimpleTexture::UpdateEffectiveAlpha() {
    uint8_t before = (this->m_alpha == 0xff || 1 < this->m_blendMode) ? this->m_blendMode : 2;
    this->CSimpleRegion::UpdateEffectiveAlpha();
    uint8_t after = (this->m_alpha == 0xff || 1 < this->m_blendMode) ? this->m_blendMode : 2;
    if (after != before) {
        this->MarkFrameBatchDirty();
    }
}

// ref: FUN_140632680
void CSimpleTexture::AddToBatch(CRenderBatch* batch) {
    if (this->m_texture != nullptr && !this->IsMaskTexture()) {
        RenderBatchAddTexture(batch, this, nullptr);
    }
}

// The corners recomputed, the animation's transform taken as their base.
// ref: FUN_140636cd0
void CSimpleTexture::Unknown2C0() {
    float positions[12] = {};
    this->ComputePositions(&this->m_rect, positions);
    this->SetAnimTransform(reinterpret_cast<const ANIMTRANSFORM*>(positions));
    std::memcpy(this->m_positions, positions, sizeof(this->m_positions));
}

// ---- the texture's slots -------------------------------------------------------------------------

bool CSimpleTexture::IsMaskTexture() {
    return false;
}

// The corners from the rect at the depth, each moved by its vertex offset, then turned by the
// rotation about the rotation centre.
// ref: FUN_140634ce0
void CSimpleTexture::ComputePositions(const CRect* rect, float* positions) {
    float scale = this->m_layoutScale;
    float depth = this->m_layoutDepth;
    // top left, bottom left, top right, bottom right: {x, y} from the rect's sides
    const float corners[4][2] = {
        { rect->minX, rect->maxY },
        { rect->minX, rect->minY },
        { rect->maxX, rect->maxY },
        { rect->maxX, rect->minY },
    };
    float offsets[4][3] = {};
    if (this->m_extension != nullptr) {
        std::memcpy(offsets, this->m_extension->m_vertexOffsets, sizeof(offsets));
    }
    if (this->m_rotation == 0.0f) {
        for (uint32_t i = 0; i < 4; i++) {
            positions[i * 3] = offsets[i][0] * scale + corners[i][0];
            positions[i * 3 + 1] = offsets[i][1] * scale + corners[i][1];
            positions[i * 3 + 2] = offsets[i][2] * scale + depth;
        }
        return;
    }
    float sine;
    float cosine;
    SinCos(this->m_rotation, &sine, &cosine);
    float center[2] = {
        (rect->maxX - rect->minX) * (this->m_rotationCenter[0] - 0.5f) + (rect->minX + rect->maxX) * 0.5f,
        (rect->maxY - rect->minY) * (this->m_rotationCenter[1] - 0.5f) + (rect->minY + rect->maxY) * 0.5f,
    };
    for (uint32_t i = 0; i < 4; i++) {
        float point[3] = { offsets[i][0] * scale + corners[i][0], offsets[i][1] * scale + corners[i][1],
            offsets[i][2] * scale + depth };
        RotatePoint(positions + i * 3, point, sine, cosine, center);
    }
}

// The quad's width in texels at the reference scale.
// ref: FUN_140632350
float CSimpleTexture::GetPixelWidth() {
    return (((this->m_positions[3][0] - this->m_positions[0][0]) * 1.0f * TexelsPerScreen()) / UI_WIDTH) / this->m_layoutScale;
}

// ref: FUN_140632310
float CSimpleTexture::GetPixelHeight() {
    return (((this->m_positions[0][1] - this->m_positions[1][1]) * 1.0f * TexelsPerScreen()) / UI_WIDTH) / this->m_layoutScale;
}

// ---- CLayoutFrame ------------------------------------------------------------------------------------

// The vertex offsets scale with the layout.
// ref: FUN_1406341a0
void CSimpleTexture::ScaleBy(float* scale) {
    this->CLayoutFrame::ScaleBy(scale);
    CTextureExtension* extension = this->m_extension;
    if (extension == nullptr) {
        return;
    }
    bool moved = false;
    for (auto& offset : extension->m_vertexOffsets) {
        float lengthSquared = offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2];
        if (2.3841858e-07f <= std::fabs(lengthSquared)) {
            moved = true;
            offset[0] *= *scale;
            offset[1] *= *scale;
        }
    }
    if (moved && (this->CLayoutFrame::m_flags & 2) == 0) {
        this->UpdatePositions(&this->m_rect);
    }
}

// Without a set width, the texture's own in UI units.
// ref: FUN_140632ff0
float CSimpleTexture::GetWidth() {
    float width = this->CLayoutFrame::GetWidth();
    if (width == 0.0f && this->m_texture != nullptr) {
        uint32_t texels = 0;
        if (TextureGetSize(this->m_texture, &texels, nullptr, false)) {
            return (static_cast<float>(texels) / TexelsPerScreen()) * UI_WIDTH;
        }
    }
    return width;
}

// ref: FUN_140632cb0
float CSimpleTexture::GetHeight() {
    float height = this->CLayoutFrame::GetHeight();
    if (height == 0.0f && this->m_texture != nullptr) {
        uint32_t texels = 0;
        if (TextureGetSize(this->m_texture, nullptr, &texels, false)) {
            return (static_cast<float>(texels) / TexelsPerScreen()) * UI_WIDTH;
        }
    }
    return height;
}

// Laid out once the texture has loaded.
// ref: FUN_140633280
bool CSimpleTexture::IsLayoutReady() {
    return this->m_texture == nullptr || !TextureIsLoading(this->m_texture);
}

// ref: FUN_140634300
void CSimpleTexture::OnLayoutReady() {
    if (this->CLayoutFrame::GetWidth() == 0.0f || this->CLayoutFrame::GetHeight() == 0.0f) {
        this->ClearRectFailed();
    }
    this->MarkFrameBatchDirty();
    this->Resize(false);
}

// Hidden, a texture that keeps its file only by id lets the handle go.
// ref: FUN_140633090
void CSimpleTexture::ApplyHide() {
    this->CSimpleRegion::ApplyHide();
    if (((this->m_textureFlags >> 0xd) & 1) == 0 && ReleasesHiddenTextures() && this->m_texture != nullptr
        && this->m_fileDataId != 0 && this->m_texture != nullptr) {
        UiReleaseTexture(&this->m_texture);
        CSimpleTop::s_instance->RemoveLayoutWaiting(this);
        this->MarkFrameBatchDirty();
    }
}

// Shown, a texture known only by its file is loaded.
// ref: FUN_140636c60
void CSimpleTexture::ApplyShow() {
    this->CSimpleRegion::ApplyShow();
    if ((this->m_layoutState & LAYOUT_SHOWN) != 0 && this->m_parent == nullptr && (this->m_layoutState & LAYOUT_VISIBLE) == 0) {
        this->m_layoutState |= LAYOUT_VISIBLE;
    }
    if ((this->m_layoutState & LAYOUT_VISIBLE) != 0 && this->m_texture == nullptr) {
        this->LoadFile(this->m_fileDataId);
    }
}

// ref: FUN_1406342d0
void CSimpleTexture::OnLayoutDepthChanged() {
    for (auto& position : this->m_positions) {
        position[2] = this->m_layoutDepth;
    }
}

// ref: FUN_140634270
void CSimpleTexture::OnFrameSizeChanged(const CRect* rect, const CRect* previous) {
    (void)previous;
    this->UpdatePositions(rect);
    if ((this->m_textureFlags & (TEXTURE_HORIZ_TILE | TEXTURE_VERT_TILE)) != 0) {
        this->m_textureFlags |= TEXTURE_TILE_DIRTY;
        this->MarkFrameBatchDirty();
    }
    if (this->m_parent != nullptr) {
        this->m_parent->MarkScrollRangeDirty();
    }
}

// ref: FUN_140632290
void CSimpleTexture::ClearAnimTransform() {
    this->CLayoutFrame::ClearAnimTransform();
    this->ComputePositions(&this->m_rect, &this->m_positions[0][0]);
}

// ref: FUN_1406322c0
void CSimpleTexture::ClearAnimRectOffset() {
    float texCoords[8] = {};
    const float* offset = this->GetAnimRectOffset();
    if (offset != nullptr) {
        std::memcpy(texCoords, offset, sizeof(texCoords));
    }
    this->CLayoutFrame::ClearAnimRectOffset();
    this->StoreTexCoords(texCoords);
}

// ---- CAnimatableObject ---------------------------------------------------------------------------------

// The animation's transform, alpha and coordinates applied.
// ref: FUN_1406343d0
void CSimpleTexture::Unknown08() {
    if (this->HasAnimTransform()) {
        const ANIMTRANSFORM* transform = &this->m_animData->m_transform;
        if (transform != nullptr) {
            std::memcpy(this->m_positions, transform->m_values, sizeof(this->m_positions));
        }
    }
    if (this->HasAnimAlpha()) {
        this->SetAlpha(this->GetAnimAlpha());
    }
    if (this->HasAnimRectOffset()) {
        const float* offset = this->GetAnimRectOffset();
        if (offset != nullptr) {
            std::memcpy(this->m_texCoords, offset, sizeof(this->m_texCoords));
        }
    }
}

// ref: FUN_140632250
void CSimpleTexture::OnAnimStarted(bool alpha, bool transform, bool rectOffset) {
    this->CSimpleRegion::OnAnimStarted(alpha, transform, rectOffset);
    if (rectOffset) {
        this->SetAnimRectOffset(&this->m_texCoords[0][0]);
    }
}

// A translation, in layout units.
// ref: FUN_140631ed0
void CSimpleTexture::Unknown40(void* value) {
    auto move = static_cast<const float*>(value);
    float scaled[2] = { this->m_layoutScale * move[0], this->m_layoutScale * move[1] };
    TranslatePositions(&this->m_positions[0][0], scaled);
    this->UpdateMasks();
}

// A texture coordinate translation.
// ref: FUN_140631ec0
void CSimpleTexture::Unknown50(void* value) {
    auto move = static_cast<const float*>(value);
    for (auto& texCoord : this->m_texCoords) {
        texCoord[0] += move[0];
        texCoord[1] += move[1];
    }
}

// ref: FUN_140631e60
void CSimpleTexture::Unknown58(int32_t index, void* value, float radians) {
    RotatePositions(&this->m_positions[0][0], index, static_cast<const float*>(value), radians);
    this->UpdateMasks();
}

// ref: FUN_140631e90
void CSimpleTexture::Unknown60(int32_t index, void* first, void* second) {
    ScalePositions(&this->m_positions[0][0], index, static_cast<const float*>(first), static_cast<const float*>(second));
    this->UpdateMasks();
}

// A flipbook frame: its cell of the texture.
// ref: FUN_1406349e0
void CSimpleTexture::Unknown78(float progress, uint32_t rows, uint32_t columns, uint32_t frames, uint32_t frameWidth,
    uint32_t frameHeight) {
    if (rows == 0 || columns == 0) {
        return;
    }
    if (frames == 0) {
        frames = rows * columns;
    }
    uint64_t frame = static_cast<uint64_t>(static_cast<int64_t>(static_cast<float>(frames) * progress)) & 0xffffffff;
    if (frames - 1 < static_cast<uint32_t>(static_cast<int64_t>(static_cast<float>(frames) * progress))) {
        frame = frames - 1;
    }
    uint32_t texWidth = 0;
    uint32_t texHeight = 0;
    float cellWidth;
    float extent;
    if (frameWidth == 0 || frameHeight == 0 || this->m_texture == nullptr
        || (TextureGetSize(this->m_texture, &texWidth, &texHeight, true), texWidth == 0 || texHeight == 0)) {
        cellWidth = 1.0f / static_cast<float>(columns);
        extent = 1.0f;
    } else {
        cellWidth = static_cast<float>(frameWidth) / static_cast<float>(texWidth);
        extent = static_cast<float>(frameWidth);
        rows = texHeight;
    }
    float rect[4];
    rect[1] = static_cast<float>(frame % columns) * cellWidth;
    rect[3] = rect[1] + cellWidth;
    rect[0] = static_cast<float>(frame / columns) * (extent / static_cast<float>(rows));
    rect[2] = rect[0] + extent / static_cast<float>(rows);
    this->SetTexCoordsRect(rect);
}

// ---- the texture ------------------------------------------------------------------------------------------

// ref: FUN_1406354e0
bool CSimpleTexture::SetColorTexture(const uint32_t* argb) {
    this->ClearAtlas();
    this->ReleaseExtensionIfUnused();
    CTexture* solid = TextureCreateSolid(argb, nullptr, "CSimpleTexture", 0);
    if (this->m_texture != nullptr) {
        UiReleaseTexture(&this->m_texture);
        CSimpleTop::s_instance->RemoveLayoutWaiting(this);
        this->MarkFrameBatchDirty();
    }
    this->m_fileDataId = 0;
    if (this->m_texture != solid) {
        if (this->m_texture != nullptr) {
            UiReleaseTexture(&this->m_texture);
            CSimpleTop::s_instance->RemoveLayoutWaiting(this);
            this->MarkFrameBatchDirty();
        }
        this->m_texture = solid;
        if (solid == nullptr) {
            CSimpleTop::s_instance->RemoveLayoutWaiting(this);
        } else {
            this->CheckLayoutReady();
            if (this->IsLayoutReady()) {
                this->OnLayoutReady();
                return true;
            }
        }
        this->MarkFrameBatchDirty();
    }
    return true;
}

// The texture handle replaced, the layout told (FUN_140635eb0).
// ref: FUN_140635eb0
static void ReplaceTexture(CTexture** slot, CSimpleTexture* owner, CTexture* texture) {
    if (*slot == texture) {
        return;
    }
    if (*slot != nullptr) {
        UiReleaseTexture(slot);
        CSimpleTop::s_instance->RemoveLayoutWaiting(owner);
        owner->MarkFrameBatchDirty();
    }
    *slot = texture;
    if (texture == nullptr) {
        CSimpleTop::s_instance->RemoveLayoutWaiting(owner);
    } else {
        owner->CheckLayoutReady();
        if (owner->IsLayoutReady()) {
            owner->OnLayoutReady();
            return;
        }
    }
    owner->MarkFrameBatchDirty();
}

namespace {

// What SetTextureById and SetTextureByName share once the file is known.
// ref: FUN_14166e680
bool ApplyFile(CSimpleTexture* texture, int32_t fileDataId, uint32_t wrapH, uint32_t wrapV, uint8_t filter) {
    if (texture->m_texture != nullptr) {
        UiReleaseTexture(&texture->m_texture);
        CSimpleTop::s_instance->RemoveLayoutWaiting(texture);
        texture->MarkFrameBatchDirty();
    }
    texture->m_fileDataId = 0;
    if (texture->m_extension != nullptr) {
        texture->m_extension->m_flags &= ~2u;
    }
    bool loaded = false;
    if (fileDataId != 0) {
        if (texture->m_extension == nullptr || (texture->m_extension->m_flags & 1) == 0) {
            uint32_t flags = texture->m_textureFlags;
            flags = (((wrapH & 0xff) << 4) ^ flags) & 0x70 ^ flags;
            flags = (((wrapV & 0xff) << 7) ^ flags) & 0x380 ^ flags;
            texture->m_textureFlags = ((static_cast<uint32_t>(filter) << 10) ^ flags) & 0x1c00 ^ flags;
            texture->m_fileDataId = fileDataId;
            texture->LoadFile(fileDataId);
            loaded = true;
        } else {
            CTextureExtension* extension = texture->m_extension;
            extension->m_fileDataId = fileDataId;
            extension->m_flags ^= (((wrapH & 0xff) << 2) ^ extension->m_flags) & 0x1c;
            extension->m_flags ^= (((wrapV & 0xff) << 5) ^ extension->m_flags) & 0xe0;
            extension->m_flags ^= ((static_cast<uint32_t>(filter) << 8) ^ extension->m_flags) & 0x700;
            if (extension->m_pixelTexture != nullptr) {
                TextureFree(&extension->m_pixelTexture);
            }
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t format = 2;
            MIPBITS* bits = TextureLoadImageById(extension->m_fileDataId, &width, &height, &format, nullptr, nullptr,
                nullptr, 1);
            if (bits != nullptr) {
                uint8_t own = 1;
                loaded = texture->BuildMaskedTexture(&own, bits[0], static_cast<int32_t>(width * 4));
                MipBitsFree(&bits);
            }
        }
    }
    texture->ReleaseExtensionIfUnused();
    return loaded;
}

}

// ---- texture sources -----------------------------------------------------------------------

// ref: FUN_140638820
TEXTURESOURCE TEXTURESOURCE::FromFileId(int32_t fileDataId) {
    TEXTURESOURCE source = {};
    source.m_type = SOURCE_FILE_ID;
    source.m_id = fileDataId;
    return source;
}

// ref: FUN_140638810
TEXTURESOURCE TEXTURESOURCE::FromAtlas(int32_t member) {
    TEXTURESOURCE source = {};
    source.m_type = SOURCE_ATLAS;
    source.m_id = member;
    return source;
}

// ref: FUN_140638850
TEXTURESOURCE TEXTURESOURCE::FromTexture(CSimpleTexture* texture) {
    TEXTURESOURCE source = {};
    source.m_type = SOURCE_TEXTURE;
    source.m_texture = texture;
    return source;
}

// ref: FUN_140638860
TEXTURESOURCE TEXTURESOURCE::FromString(const char* name) {
    int32_t id = 0;
    TextureFileId(&id, name);
    if (id != 0) {
        return FromFileId(id);
    }
    TEXTURESOURCE source = {};
    source.m_type = SOURCE_ATLAS;
    TEXTUREATLASSCALE scale = { 0, 0 };
    int32_t member = 0;
    source.m_id = *TextureAtlasFindByName(&member, name, &scale);
    return source;
}

// ref: FUN_1406389c0
CSimpleTexture* TEXTURESOURCE::GetTexture() const {
    return this->m_type == SOURCE_TEXTURE ? this->m_texture : nullptr;
}

// ref: FUN_140638960
int32_t TEXTURESOURCE::GetFileDataId() const {
    if (this->m_type == SOURCE_FILE || this->m_type == SOURCE_FILE_ID) {
        return this->m_id;
    }
    if (this->m_type == SOURCE_ATLAS) {
        if (this->m_id != 0) {
            return TextureAtlasGetFileDataId(&this->m_id);
        }
    } else if (this->m_type == SOURCE_TEXTURE && this->m_texture != nullptr) {
        if (this->m_texture->GetFileDataId() != 0) {
            return 1;
        }
    }
    return 0;
}

// ref: FUN_140636a20
bool CSimpleTexture::SetTextureSource(const TEXTURESOURCE* source) {
    int32_t type = source->m_type;
    uint32_t flags = this->m_textureFlags;
    if (type == TEXTURESOURCE::SOURCE_FILE || type == TEXTURESOURCE::SOURCE_FILE_ID) {
        return this->SetTextureById(source->GetFileDataId(), (flags >> 4) & 7, (flags >> 7) & 7,
            static_cast<uint8_t>((flags >> 10) & 7), true, false);
    }
    if (type == TEXTURESOURCE::SOURCE_ATLAS) {
        int32_t member = source->m_id;
        return this->SetAtlas(&member, static_cast<uint8_t>((flags >> 10) & 7), false, static_cast<uint8_t>((flags >> 4) & 7),
            static_cast<uint8_t>((flags >> 7) & 7));
    }
    if (type != TEXTURESOURCE::SOURCE_TEXTURE) {
        return false;
    }
    CSimpleTexture* other = source->GetTexture();
    if (other != nullptr) {
        if (other->m_atlas != 0) {
            TEXTURESOURCE atlas = TEXTURESOURCE::FromAtlas(other->m_atlas);
            return this->SetTextureSource(&atlas);
        }
        int32_t id = 0;
        if (other->m_texture != nullptr) {
            id = TextureGetFileDataId(reinterpret_cast<HTEXTURE>(other->m_texture));
        } else if ((other->m_layoutState & CLayoutFrame::LAYOUT_VISIBLE) == 0 && other->m_fileDataId != 0) {
            id = other->m_fileDataId;
        }
        TEXTURESOURCE file = TEXTURESOURCE::FromFileId(id);
        return this->SetTextureSource(&file);
    }
    this->ClearAtlas();
    if (this->m_texture != nullptr) {
        UiReleaseTexture(&this->m_texture);
        CSimpleTop::s_instance->RemoveLayoutWaiting(this);
        this->MarkFrameBatchDirty();
    }
    this->m_fileDataId = 0;
    if (this->m_extension != nullptr) {
        this->m_extension->m_flags &= ~2u;
    }
    this->ReleaseExtensionIfUnused();
    return false;
}

// ref: FUN_140635630
bool CSimpleTexture::SetTextureById(int32_t fileDataId, uint32_t wrapH, uint32_t wrapV, uint8_t filter,
    bool resetTexCoords, bool force) {
    if (resetTexCoords) {
        this->ClearAtlas();
    }
    if (!force && fileDataId != 0 && this->IsTextureFile(fileDataId)
        && ((static_cast<uint8_t>(wrapH) == 0) == ((this->m_textureFlags & 0x70) == 0))
        && ((static_cast<uint8_t>(wrapV) == 0) == ((this->m_textureFlags & 0x380) == 0))) {
        return true;
    }
    return ApplyFile(this, fileDataId, wrapH, wrapV, filter);
}

// ref: FUN_140635960
bool CSimpleTexture::SetTextureByName(const char* name, uint32_t wrapH, uint32_t wrapV, uint8_t filter,
    bool resetTexCoords, bool force) {
    int32_t requested = static_cast<int32_t>(FileDataIdOf(name));
    if (resetTexCoords) {
        this->ClearAtlas();
    }
    if (!force && name != nullptr && *name != '\0') {
        bool same = false;
        bool known = true;
        if (this->m_texture == nullptr) {
            if ((this->m_layoutState & LAYOUT_VISIBLE) != 0 || this->m_fileDataId == 0) {
                known = false;
            } else {
                same = TextureIsSameFileId(this->m_fileDataId, requested);
            }
        } else {
            same = TextureIsSameFile(this->m_texture, requested);
        }
        if (known && same && ((static_cast<uint8_t>(wrapH) == 0) == ((this->m_textureFlags & 0x70) == 0))
            && ((static_cast<uint8_t>(wrapV) == 0) == ((this->m_textureFlags & 0x380) == 0))) {
            return true;
        }
    }
    if (name == nullptr || *name == '\0') {
        return ApplyFile(this, 0, wrapH, wrapV, filter);
    }
    return ApplyFile(this, static_cast<int32_t>(FileDataIdOf(name)), wrapH, wrapV, filter);
}

// ref: FUN_1406358c0
bool CSimpleTexture::SetTextureHandle(CTexture* texture, bool keepMasks) {
    if (!keepMasks) {
        this->ReleaseExtensionIfUnused();
    }
    this->ClearAtlas();
    if (this->m_texture != texture) {
        if (texture != nullptr) {
            TextureAddRef(texture);
        }
        if (this->m_texture == nullptr) {
            this->m_fileDataId = 0;
        } else {
            this->ReleaseTexture();
        }
        ReplaceTexture(&this->m_texture, this, texture);
    }
    return true;
}

// ref: FUN_140634740
bool CSimpleTexture::SetAtlas(const int32_t* atlas, uint8_t filter, bool useAtlasSize, uint8_t wrapH, uint8_t wrapV) {
    if (*atlas == 0) {
        return false;
    }
    float texCoords[8] = {};
    if (!useAtlasSize) {
        if (this->m_atlas == 0) {
            std::memcpy(texCoords, this->m_texCoords, sizeof(texCoords));
        } else {
            TextureAtlasUnmapTexCoords(&this->m_atlas, texCoords, &this->m_texCoords[0][0]);
        }
    } else {
        const float full[8] = { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f };
        std::memcpy(texCoords, full, sizeof(texCoords));
    }
    this->ClearAtlas();
    this->m_atlas = *atlas;
    if (this->m_atlas == 0) {
        this->StoreTexCoords(texCoords);
    } else {
        float mapped[8] = {};
        TextureAtlasMapTexCoords(&this->m_atlas, mapped, texCoords);
        this->StoreTexCoords(mapped);
    }
    int32_t entry = *atlas;
    uint8_t tileH = TextureAtlasTilesHorizontally(&entry) ? 1 : wrapH;
    uint8_t tileV = TextureAtlasTilesVertically(&entry) ? 1 : wrapV;
    char path[0x110];
    if (!TextureAtlasGetPath(atlas, path, 0x104)) {
        int32_t fileDataId = TextureAtlasGetFileDataId(atlas);
        return this->SetTextureById(fileDataId, tileH, tileV, filter, false, false);
    }
    return this->SetTextureByName(path, tileH, tileV, filter, false, false);
}

// ref: FUN_140635c70
bool CSimpleTexture::SetPixels(const uint32_t* pixels, int32_t width, int32_t height) {
    if (this->m_extension != nullptr) {
        this->m_extension->m_flags &= ~1u;
        if (this->m_extension->m_pixelTexture != nullptr) {
            TextureFree(&this->m_extension->m_pixelTexture);
        }
    }
    this->EnsureExtension();
    uint32_t count = static_cast<uint32_t>(width * height);
    CTextureExtension* extension = this->m_extension;
    extension->m_flags |= 2;
    ArgbArrayReserve(&extension->m_pixels, count);
    std::memcpy(extension->m_pixels.m_data, pixels, static_cast<uint64_t>(count) * 4);
    extension->m_pixels.m_count = count;
    static uint32_t s_rawCount;     // DAT_142e99d9c
    s_rawCount++;
    char name[0x80];
    SStrPrintf(name, sizeof(name), "_RawPixelData%u", s_rawCount);
    blz::function_storage fill;
    TextureMakePixelFill(&fill, &extension->m_pixels);
    extension->m_pixelTexture = TextureCreateUnique(2, static_cast<uint32_t>(height), static_cast<uint32_t>(width), 1, 2, 2,
        0x82001, &fill, name, false);
    CTexture* texture = extension->m_pixelTexture;
    this->ClearAtlas();
    if (this->m_texture != texture) {
        if (texture != nullptr) {
            TextureAddRef(texture);
        }
        if (this->m_texture == nullptr) {
            this->m_fileDataId = 0;
        } else {
            this->ReleaseTexture();
        }
        ReplaceTexture(&this->m_texture, this, texture);
    }
    return true;
}

// ref: FUN_140632e90
int32_t CSimpleTexture::GetFileDataId() {
    if (this->m_texture != nullptr) {
        return TextureGetFileDataId(this->m_texture);
    }
    if ((this->m_layoutState & LAYOUT_VISIBLE) == 0 && this->m_fileDataId != 0) {
        return this->m_fileDataId;
    }
    return 0;
}

// ref: FUN_140632ef0
bool CSimpleTexture::GetTextureName(char* buffer, uint64_t size) {
    if (this->m_texture == nullptr) {
        if ((this->m_layoutState & LAYOUT_VISIBLE) == 0 && this->m_fileDataId != 0) {
            SStrPrintf(buffer, size, "%u", this->m_fileDataId);
            return true;
        }
        return false;
    }
    char name[0x110];
    TextureGetName(this->m_texture, name, 0x104);
    if (name[0] == '\0') {
        return false;
    }
    OsPathChangeExtension(buffer, size, name, "");
    return true;
}

// ref: FUN_140631a40
bool CSimpleTexture::IsTextureFile(int32_t fileDataId) {
    if (this->m_texture != nullptr) {
        return TextureIsSameFile(this->m_texture, fileDataId);
    }
    if ((this->m_layoutState & LAYOUT_VISIBLE) == 0 && this->m_fileDataId != 0) {
        return TextureIsSameFileId(this->m_fileDataId, fileDataId);
    }
    return false;
}

// Visible (or loading regardless), the file's texture is made with the texture's settings.
// ref: FUN_1406332a0
void CSimpleTexture::LoadFile(int32_t fileDataId) {
    if ((this->m_layoutState & LAYOUT_VISIBLE) == 0) {
        return;
    }
    if (((this->CLayoutFrame::m_flags & 2) == 0 || FrameXML_IsLoadingToc()) && ((this->m_textureFlags >> 0xe) & 1) == 0
        && ReleasesHiddenTextures()) {
        for (CSimpleFrame* frame = this->m_parent; frame != nullptr; frame = frame->m_parent) {
            if ((frame->m_layoutState & LAYOUT_VISIBLE) == 0) {
                return;
            }
        }
    }
    if (this->m_fileDataId == 0) {
        return;
    }
    uint32_t flags = this->m_textureFlags;
    CStatus status;
    CTexture* texture = TextureCreate(fileDataId, TextureGxFlags(flags), &status, TextureCreateFlags(flags), 1, 0);
    if (status.m_maxSeverity < 2) {
        ReplaceTexture(&this->m_texture, this, texture);
    } else {
        TextureFree(&texture);
        SysMessageReportStatus(&status);
    }
}

// ref: FUN_140632c30
void CSimpleTexture::ReleaseTexture() {
    if (this->m_texture != nullptr) {
        UiReleaseTexture(&this->m_texture);
        CSimpleTop::s_instance->RemoveLayoutWaiting(this);
        this->MarkFrameBatchDirty();
    }
    this->m_fileDataId = 0;
}

// ref: FUN_140632c90
void* CSimpleTexture::GetGxTex() {
    if (this->m_texture == nullptr) {
        return nullptr;
    }
    return TextureGetGxTexBlocking(this->m_texture);
}

// ref: FUN_140621360
bool CSimpleTexture::PrepareToDraw() {
    CTexture* texture = this->m_texture;
    if (texture == nullptr) {
        return false;
    }
    if (this->m_extension != nullptr && this->m_extension->m_masks.size() != 0) {
        this->UpdateMaskTexCoords();
    }
    if (!TextureIsReady(texture)) {
        TextureSetPriority(texture, static_cast<int32_t>(0x80000000));
        if (s_drawWhileLoading == 0) {
            if ((this->m_textureFlags & TEXTURE_BLOCKING) == 0) {
                return false;
            }
            TextureGetGxTexIfResident(texture);
        } else {
            TextureGetGxTexWait(texture);
        }
        if (!TextureIsReady(texture)) {
            return false;
        }
    }
    if ((this->m_textureFlags & TEXTURE_TILE_DIRTY) == 0) {
        return true;
    }
    float texCoords[8] = {};
    bool animated = false;
    const float* offset = nullptr;
    if (this->HasAnimRectOffset() && (offset = this->GetAnimRectOffset()) != nullptr) {
        std::memcpy(texCoords, offset, sizeof(texCoords));
        animated = true;
    } else {
        this->GetStoredTexCoords(texCoords);
    }
    auto gx = static_cast<const uint16_t*>(this->GetGxTex());
    if ((this->m_textureFlags & TEXTURE_HORIZ_TILE) != 0) {
        uint16_t texels = gx[0];
        float width = this->GetPixelWidth();
        if (texels != 0 && 0.0f < width) {
            float u = width / static_cast<float>(texels);
            if (texCoords[0] != 0.0f) {
                texCoords[0] = u;
            }
            if (texCoords[2] != 0.0f) {
                texCoords[2] = u;
            }
            if (texCoords[4] != 0.0f) {
                texCoords[4] = u;
            }
            if (texCoords[6] != 0.0f) {
                texCoords[6] = u;
            }
        }
    }
    if ((this->m_textureFlags & TEXTURE_VERT_TILE) != 0) {
        uint16_t texels = gx[1];
        float height = this->GetPixelHeight();
        if (texels != 0 && 0.0f < height) {
            float v = height / static_cast<float>(texels);
            if (texCoords[1] != 0.0f) {
                texCoords[1] = v;
            }
            if (texCoords[3] != 0.0f) {
                texCoords[3] = v;
            }
            if (texCoords[5] != 0.0f) {
                texCoords[5] = v;
            }
            if (texCoords[7] != 0.0f) {
                texCoords[7] = v;
            }
        }
    }
    if (animated) {
        this->SetAnimRectOffset(texCoords);
    }
    this->StoreTexCoords(texCoords);
    this->m_textureFlags &= ~TEXTURE_TILE_DIRTY;
    return true;
}

// ---- coordinates and corners ------------------------------------------------------------------------------

// ref: FUN_140635460
void CSimpleTexture::SetTexCoords(const float* texCoords) {
    if (this->m_atlas != 0) {
        float mapped[8] = {};
        TextureAtlasMapTexCoords(&this->m_atlas, mapped, texCoords);
        this->StoreTexCoords(mapped);
        return;
    }
    this->StoreTexCoords(texCoords);
}

// ref: FUN_1406353c0
void CSimpleTexture::SetTexCoordsRect(const float* rect) {
    // rect: top, left, bottom, right -> ul, ll, ur, lr
    const float texCoords[8] = { rect[1], rect[0], rect[1], rect[2], rect[3], rect[0], rect[3], rect[2] };
    this->SetTexCoords(texCoords);
}

// ref: FUN_140635190
void CSimpleTexture::StoreTexCoords(const float* texCoords) {
    if (!this->HasAnimRectOffset()) {
        std::memcpy(this->m_texCoords, texCoords, sizeof(this->m_texCoords));
        return;
    }
    const float* base = this->GetAnimRectOffset();
    if (base == nullptr) {
        return;
    }
    float previous[8];
    std::memcpy(previous, base, sizeof(previous));
    this->SetAnimRectOffset(texCoords);
    float* stored = &this->m_texCoords[0][0];
    for (uint32_t i = 0; i < 8; i++) {
        stored[i] = (texCoords[i] - previous[i]) + stored[i];
    }
}

// ref: FUN_140632e10
void CSimpleTexture::GetTexCoords(float* texCoords) {
    if (this->m_atlas != 0) {
        float stored[8];
        std::memcpy(stored, this->m_texCoords, sizeof(stored));
        TextureAtlasUnmapTexCoords(&this->m_atlas, texCoords, stored);
        return;
    }
    std::memcpy(texCoords, this->m_texCoords, sizeof(this->m_texCoords));
}

// ref: FUN_140632da0
void CSimpleTexture::GetStoredTexCoords(float* texCoords) {
    std::memcpy(texCoords, this->m_texCoords, sizeof(this->m_texCoords));
}

// ref: FUN_140634490
void CSimpleTexture::ClearAtlas() {
    float texCoords[8] = {};
    if (this->m_atlas == 0) {
        std::memcpy(texCoords, this->m_texCoords, sizeof(texCoords));
    } else {
        float stored[8];
        std::memcpy(stored, this->m_texCoords, sizeof(stored));
        TextureAtlasUnmapTexCoords(&this->m_atlas, texCoords, stored);
    }
    this->m_atlas = 0;
    this->StoreTexCoords(texCoords);
}

// ref: FUN_140636960
void CSimpleTexture::SetVertexOffset(uint32_t corner, const float* x, const float* y) {
    if (corner >= 4) {
        return;
    }
    float offsetX = *x;
    float offsetY = *y;
    if (this->m_extension == nullptr && std::fabs(offsetY * offsetY + offsetX * offsetX) < 2.3841858e-07f) {
        return;
    }
    this->EnsureExtension();
    this->m_extension->m_vertexOffsets[corner][0] = offsetX;
    this->m_extension->m_vertexOffsets[corner][1] = offsetY;
    this->m_extension->m_vertexOffsets[corner][2] = 0.0f;
    this->UpdatePositions(&this->m_rect);
}

void CSimpleTexture::GetVertexOffset(uint32_t corner, float* x, float* y) {
    if (this->m_extension != nullptr && corner < 4) {
        *x = this->m_extension->m_vertexOffsets[corner][0];
        *y = this->m_extension->m_vertexOffsets[corner][1];
        return;
    }
    *x = 0.0f;
    *y = 0.0f;
}

void CSimpleTexture::SetRotation(float radians, const float* center) {
    float centerX = 0.5f;
    float centerY = 0.5f;
    if (center != nullptr) {
        centerX = center[0];
        centerY = center[1];
    }
    if (radians != this->m_rotation || centerX != this->m_rotationCenter[0] || centerY != this->m_rotationCenter[1]) {
        this->m_rotation = radians;
        this->m_rotationCenter[0] = centerX;
        this->m_rotationCenter[1] = centerY;
        if ((this->CLayoutFrame::m_flags & 2) == 0) {
            this->UpdatePositions(&this->m_rect);
        }
    }
}

void CSimpleTexture::SetDesaturation(float desaturation) {
    float clamped = 0.0f;
    if (0.0f <= desaturation) {
        clamped = 1.0f <= desaturation ? 1.0f : desaturation;
    }
    if (clamped != this->m_desaturation) {
        this->m_desaturation = clamped;
        this->MarkFrameBatchDirty();
    }
}

void CSimpleTexture::SetSnappingBias(float bias) {
    float clamped = 0.0f;
    if (0.0f <= bias) {
        clamped = 1.0f <= bias ? 1.0f : bias;
    }
    if (clamped != this->m_snappingBias) {
        this->m_snappingBias = clamped;
        this->MarkFrameBatchDirty();
    }
}

void CSimpleTexture::SetBlendMode(uint8_t mode) {
    uint8_t before = this->m_blendMode;
    if (mode == before) {
        return;
    }
    uint8_t after = mode;
    if (this->m_alpha == 0xff || 1 < before) {
        this->m_blendMode = mode;
        if (this->m_alpha == 0xff) {
            if (after == before) {
                return;
            }
            goto changed;
        }
    } else {
        before = 2;
        this->m_blendMode = mode;
    }
    if (after < 2) {
        after = 2;
    }
    if (after == before) {
        return;
    }
changed:
    if ((this->m_layoutState & LAYOUT_VISIBLE) != 0 && this->m_parent != nullptr && this->IsLayoutReady()) {
        this->m_parent->MarkBatchDirty();
    }
}

// ref: FUN_140637160
void CSimpleTexture::UpdatePositions(const CRect* rect) {
    if (!this->HasAnimTransform()) {
        this->ComputePositions(rect, &this->m_positions[0][0]);
    } else if (this->m_animData != nullptr) {
        const float* base = this->m_animData->m_transform.m_values;
        float positions[12] = {};
        this->ComputePositions(rect, positions);
        float* stored = &this->m_positions[0][0];
        for (uint32_t i = 0; i < 12; i++) {
            stored[i] = (positions[i] - base[i]) + stored[i];
        }
        this->SetAnimTransform(reinterpret_cast<const ANIMTRANSFORM*>(positions));
    }
    this->UpdateMasks();
}

// ref: FUN_140637370
void CSimpleTexture::UpdatePositionsAndTiling() {
    this->UpdatePositions(&this->m_rect);
    if ((this->m_textureFlags & (TEXTURE_HORIZ_TILE | TEXTURE_VERT_TILE)) != 0) {
        this->m_textureFlags |= TEXTURE_TILE_DIRTY;
        this->MarkFrameBatchDirty();
    }
    if (this->m_parent != nullptr) {
        this->m_parent->MarkScrollRangeDirty();
    }
}

// ---- masks ---------------------------------------------------------------------------------------------------

// ref: FUN_140632150
void CSimpleTexture::EnsureExtension() {
    if (this->m_extension != nullptr) {
        return;
    }
    auto extension = static_cast<CTextureExtension*>(s_extensionHeap.GetData(false, s_extensionTag, -2, 0x1f));
    if (extension == nullptr) {
        this->m_extension = nullptr;
        return;
    }
    std::memset(static_cast<void*>(extension), 0, sizeof(CTextureExtension));
    extension->m_flags = (extension->m_flags & 0xfffff900) | 0x100;
    new (&extension->m_masked) blz::vector<CSimpleTexture*, s_maskedTextureTag>();
    new (&extension->m_masks) blz::vector<CSimpleMaskTexture*, s_maskTextureTag>();
    this->m_extension = extension;
}

// ref: FUN_140632390
void CSimpleTexture::ReleaseExtensionIfUnused() {
    CTextureExtension* extension = this->m_extension;
    if (extension == nullptr || (extension->m_flags & 3) != 0 || extension->m_masks.size() != 0
        || extension->m_masked.size() != 0) {
        return;
    }
    for (const auto& offset : extension->m_vertexOffsets) {
        if (2.3841858e-07f <= std::fabs(offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2])) {
            return;
        }
    }
    if (extension->m_pixelTexture != nullptr) {
        TextureFree(&extension->m_pixelTexture);
    }
    extension->m_masks.~vector();
    extension->m_masked.~vector();
    if (extension->m_pixels.m_data != nullptr) {
        SMemFree(extension->m_pixels.m_data, s_argbArrayTag, -2, 0);
    }
    s_extensionHeap.PutData(extension);
    this->m_extension = nullptr;
    this->MarkFrameBatchDirty();
}

// ref: FUN_140631f20
void CSimpleTexture::AddMaskTexture(CSimpleMaskTexture* mask) {
    this->EnsureExtension();
    CTextureExtension* extension = this->m_extension;
    if (2 < extension->m_masks.size()) {
        FrameScript_ActionError("Texture already has the maximum number of mask textures (%d)", 3);
        return;
    }
    for (CSimpleMaskTexture* existing : extension->m_masks) {
        if (existing == mask) {
            return;
        }
    }
    CSimpleMaskTexture_AddMasked(mask, this);
    extension->m_masks.push_back(mask);
    this->MarkFrameBatchDirty();
}

void CSimpleTexture::RemoveMaskTexture(CSimpleMaskTexture* mask) {
    CTextureExtension* extension = this->m_extension;
    if (extension == nullptr) {
        return;
    }
    bool found = false;
    uint64_t kept = 0;
    for (uint64_t i = 0; i < extension->m_masks.size(); i++) {
        CSimpleMaskTexture* entry = extension->m_masks[i];
        if (entry == mask) {
            CSimpleMaskTexture_RemoveMasked(mask, this);
            found = true;
        } else {
            extension->m_masks[kept++] = entry;
        }
    }
    if (found) {
        extension->m_masks.m_size = kept;
        this->MarkFrameBatchDirty();
    }
    this->ReleaseExtensionIfUnused();
}

CSimpleMaskTexture* CSimpleTexture::GetMaskTexture(uint32_t index) {
    auto& masks = this->m_extension != nullptr ? this->m_extension->m_masks : s_noMasks;
    if (index < masks.m_size) {
        return masks.m_data[index];
    }
    return nullptr;
}

uint64_t CSimpleTexture::GetNumMaskTextures() {
    return this->m_extension != nullptr ? this->m_extension->m_masks.size() : s_noMasks.size();
}

// Each mask's coordinates for this texture's corners: the corners relative to the mask's,
// turned into the mask's frame when the mask is rotated.
void CSimpleTexture::UpdateMaskTexCoords() {
    CTextureExtension* extension = this->m_extension;
    if (extension == nullptr || 3 < extension->m_masks.size()) {
        return;
    }
    for (uint32_t i = 0; i < extension->m_masks.size(); i++) {
        auto mask = reinterpret_cast<CSimpleTexture*>(extension->m_masks[i]);
        const float* own = &this->m_positions[0][0];
        const float* other = &mask->m_positions[0][0];
        float corners[8];
        float extent[2];
        if (own[1] == own[7] && other[1] == other[7] && own[0] <= own[9] && other[0] <= other[9]) {
            float originX = other[3];
            float originY = other[4];
            corners[0] = own[0] - originX;
            corners[1] = own[1] - originY;
            corners[2] = own[3] - originX;
            corners[3] = own[4] - originY;
            corners[4] = own[6] - originX;
            corners[5] = own[7] - originY;
            corners[6] = own[9] - originX;
            corners[7] = own[10] - originY;
            extent[0] = (other[9] - originX) - (other[0] - originX);
            extent[1] = other[1] - originY;
        } else {
            float angle = std::atan2(other[7] - other[1], other[6] - other[0]);
            float sine;
            float cosine;
            SinCos(-angle, &sine, &cosine);
            float originX = other[3];
            float originY = other[4];
            float rotatedOther[8];
            for (uint32_t k = 0; k < 4; k++) {
                float x = own[k * 3] - originX;
                float y = own[k * 3 + 1] - originY;
                corners[k * 2] = x * cosine - y * sine;
                corners[k * 2 + 1] = x * sine + y * cosine;
                float ox = other[k * 3] - originX;
                float oy = other[k * 3 + 1] - originY;
                rotatedOther[k * 2] = ox * cosine - oy * sine;
                rotatedOther[k * 2 + 1] = ox * sine + oy * cosine;
            }
            extent[0] = rotatedOther[4] - rotatedOther[0];
            extent[1] = rotatedOther[1] - rotatedOther[3];
        }
        MaskTexCoords(extension->m_maskTexCoords[i], corners, extent);
    }
}

void CSimpleTexture::UpdateMasks() {
    if (this->IsMaskTexture() && this->m_extension != nullptr) {
        for (CSimpleTexture* masked : this->m_extension->m_masked) {
            masked->UpdateMaskTexCoords();
        }
    }
    if (this->m_extension != nullptr && this->m_extension->m_masks.size() != 0) {
        this->UpdateMaskTexCoords();
    }
}

void CSimpleTexture::SetMaskFile(const char* name) {
    int32_t mask = 0;
    if (name != nullptr && *name != '\0') {
        mask = static_cast<int32_t>(FileDataIdOf(name));
    }
    int32_t fileDataId = 0;
    uint32_t wrapH = 0;
    uint32_t wrapV = 0;
    uint8_t filter = 1;
    CTextureExtension* extension = this->m_extension;
    if (extension == nullptr) {
        if (this->m_texture != nullptr) {
            fileDataId = TextureGetFileDataId(this->m_texture);
            uint32_t flags = this->m_textureFlags;
            wrapH = (flags >> 4) & 7;
            wrapV = (flags >> 7) & 7;
            filter = static_cast<uint8_t>((flags >> 10) & 7);
        }
    } else {
        fileDataId = extension->m_fileDataId;
        uint32_t flags = extension->m_flags;
        wrapH = (flags >> 2) & 7;
        wrapV = (flags >> 5) & 7;
        filter = static_cast<uint8_t>((flags >> 8) & 7);
    }
    if (mask == 0) {
        if (this->m_extension != nullptr) {
            this->m_extension->m_flags &= ~1u;
        }
        this->ReleaseExtensionIfUnused();
    } else {
        this->EnsureExtension();
        CTextureExtension* ext = this->m_extension;
        ext->m_flags |= 1;
        ext->m_flags &= ~2u;
        ext->m_maskFile = mask;
        ext->m_fileDataId = fileDataId;
        ext->m_flags = (ext->m_flags & 0xffffffe3) | (wrapH << 2);
        ext->m_flags = (ext->m_flags & 0xffffff1f) | (wrapV << 5);
        ext->m_flags = (static_cast<uint32_t>(filter) << 8) | (ext->m_flags & 0xfffff8ff);
    }
    if (fileDataId != 0) {
        this->SetTextureById(fileDataId, wrapH, wrapV, filter, false, true);
    }
}

bool CSimpleTexture::HasMaskFile() {
    return this->m_extension != nullptr && (this->m_extension->m_flags & 1) != 0;
}

// The masked texture: the image's colour with the mask file's alpha, sampled over the texture
// coordinates' rect and stored at power-of-two size.
// ref: FUN_140635f60
bool CSimpleTexture::BuildMaskedTexture(const uint8_t* flag, const uint8_t* pixels, int32_t pitch) {
    (void)flag;
    (void)pixels;
    (void)pitch;
    FROST_UNIMPLEMENTED();  // the mask-file compositing (needs the texture's masked-pixels writer)
    return false;
}


// A mask texture keeps the textures it masks.
// ref: FUN_1406381e0
void CSimpleMaskTexture_AddMasked(CSimpleMaskTexture* mask, CSimpleTexture* texture) {
    auto owner = reinterpret_cast<CSimpleTexture*>(mask);
    owner->EnsureExtension();
    for (CSimpleTexture* masked : owner->m_extension->m_masked) {
        if (masked == texture) {
            return;
        }
    }
    owner->m_extension->m_masked.push_back(texture);
}

// ref: FUN_140638730
void CSimpleMaskTexture_RemoveMasked(CSimpleMaskTexture* mask, CSimpleTexture* texture) {
    auto owner = reinterpret_cast<CSimpleTexture*>(mask);
    if (owner->m_extension == nullptr) {
        return;
    }
    auto& masked = owner->m_extension->m_masked;
    for (uint64_t i = 0; i < masked.size(); i++) {
        if (masked[i] == texture) {
            masked.erase(i, i + 1);
            break;
        }
    }
    owner->ReleaseExtensionIfUnused();
}

// The quad and its visible masks (up to three) into a batch.
// ref: FUN_140620b00
void RenderBatchAddTexture(CRenderBatch* batch, CSimpleTexture* texture, CSimpleTexture* owner) {
    bool ready = texture->PrepareToDraw();
    auto& masks = texture->m_extension != nullptr ? texture->m_extension->m_masks : s_noMasks;
    for (CSimpleMaskTexture* entry : masks) {
        auto mask = reinterpret_cast<CSimpleTexture*>(entry);
        if ((mask->m_layoutState & CLayoutFrame::LAYOUT_SHOWN) != 0) {
            ready = mask->m_texture != nullptr && mask->PrepareToDraw() && ready;
        }
    }
    if (!ready) {
        return;
    }
    uint8_t mode = (texture->m_alpha == 0xff || 1 < texture->m_blendMode) ? texture->m_blendMode : 2;
    uint16_t blend = g_renderBatchBlends[mode];
    CTexture* extraTextures[3] = {};
    const float* extraTexCoords[3] = {};
    uint32_t extraCount = 0;
    uint32_t considered = static_cast<uint32_t>(masks.size() < 3 ? masks.size() : 3);
    for (uint32_t i = 0; i < considered; i++) {
        auto mask = reinterpret_cast<CSimpleTexture*>(masks.m_data[i]);
        if ((mask->m_layoutState & CLayoutFrame::LAYOUT_SHOWN) != 0) {
            extraTextures[extraCount] = mask->m_texture;
            extraTexCoords[extraCount] = (i < 3 && texture->m_extension != nullptr) ? texture->m_extension->m_maskTexCoords[i] : nullptr;
            extraCount++;
        }
    }
    bool onTop = texture->m_parent != nullptr
        && (texture->m_parent->m_flags2 & CSimpleFrame::FRAME2_STRATA) == 0x480;
    bool gradient = (texture->m_regionFlags & CSimpleRegion::REGIONFLAG_GRADIENT) != 0;
    const uint8_t* colors = gradient ? reinterpret_cast<const uint8_t*>(texture->m_gradient->m_colors)
                                     : reinterpret_cast<const uint8_t*>(&texture->m_color);
    uint64_t drawOrder;
    CSimpleFrame* frame;
    if (owner == nullptr) {
        drawOrder = texture->GetOwnDrawOrderKey();
        frame = texture->m_parent;
    } else {
        drawOrder = owner->GetOwnDrawOrderKey();
        frame = owner->m_parent;
    }
    batch->AddGeometry(frame, drawOrder, texture->m_texture, blend, 4, &texture->m_positions[0][0],
        &texture->m_texCoords[0][0], static_cast<uint16_t>(gradient ? 4 : 0), colors, 6, s_quadIndices,
        texture->m_desaturation, onTop, texture->m_snappingBias, ((texture->m_textureFlags >> 0x11) & 1) != 0, extraCount,
        extraTextures, extraTexCoords);
}

// ---- CSimpleMaskTexture ------------------------------------------------------------------------------

namespace {

CDataAllocator s_maskHeap(0x2a0, 100);

}

int32_t CSimpleMaskTexture::s_metaTable = -2;
int32_t CSimpleMaskTexture::s_restrictedMetaTable = -2;

// ref: FUN_140631b10
CSimpleMaskTexture::CSimpleMaskTexture(CSimpleFrame* frame, uint8_t drawLayer, int8_t subLevel, bool show)
    : CSimpleTexture(frame, drawLayer, subLevel, show) {
}

CSimpleMaskTexture::~CSimpleMaskTexture() {
}

void* CSimpleMaskTexture::operator new(size_t size) {
    (void)size;
    return s_maskHeap.GetData(false, ".?AVCSimpleMaskTexture@@", -2, 0x1f);
}

void CSimpleMaskTexture::operator delete(void* mask) {
    if (mask != nullptr) {
        s_maskHeap.PutData(mask);
    }
}

// ref: FUN_1406383a0
void CSimpleMaskTexture::CreateScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    s_metaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleMaskTexture_FillScriptMethods, false);
    s_restrictedMetaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleMaskTexture_FillScriptMethods, true);
}

// ref: FUN_1424c4260
void CSimpleMaskTexture::DestroyScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    luaL_unref(L, LUA_REGISTRYINDEX, s_metaTable);
    luaL_unref(L, LUA_REGISTRYINDEX, s_restrictedMetaTable);
    s_metaTable = -2;
    s_restrictedMetaTable = -2;
}

// The reference's type id and its magic-static guard (CSimpleMaskTexture), held here so translated code
// and the port number the class once, alike.
int32_t g_cSimpleMaskTextureType;  // DAT_142e99dc8
int32_t g_cSimpleMaskTextureTypeGuard;  // DAT_142e99dcc

int32_t CSimpleMaskTexture::GetObjectType() {
    return FrameScript_TypeId(&g_cSimpleMaskTextureType, &g_cSimpleMaskTextureTypeGuard);
}

// ref: FUN_140667880
int32_t CSimpleMaskTexture::GetScriptMetaTable() {
    if ((this->FrameScript_Object::m_flags & 1) == 0 && (this->FrameScript_Object::m_flags & 4) == 0) {
        return s_metaTable;
    }
    return s_restrictedMetaTable;
}

// ref: FUN_1406679b0
bool CSimpleMaskTexture::IsObjectTypeName(const char* name) {
    return _stricmp(name, "MaskTexture") == 0 || this->CSimpleTexture::IsObjectTypeName(name);
}

// ref: FUN_1406678b0
bool CSimpleMaskTexture::IsObjectType(int32_t type) {
    return type == CSimpleMaskTexture::GetObjectType() || type == CSimpleTexture::GetObjectType()
        || this->CScriptRegion::IsObjectType(type);
}

// ref: FUN_140667870
const char* CSimpleMaskTexture::GetObjectTypeName() {
    return "MaskTexture";
}

// The texture's settings, then the textures it masks: by global name (target) or by child key
// of its frame (childKey).
// ref: FUN_1406384b0
void CSimpleMaskTexture::LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) {
    this->CSimpleTexture::LoadXML(node, status, intrinsic);
    if ((node->m_flags & 0x200000) == 0) {
        return;
    }
    for (XMLOBJECT* list = node->m_child; list != nullptr; list = list->m_next) {
        if (list->m_type != XMLTYPE_MASKEDTEXTURES) {
            continue;
        }
        for (XMLOBJECT* child = list->m_child; child != nullptr; child = child->m_next) {
            if (child->m_type != XMLTYPE_MASKEDTEXTURE) {
                continue;
            }
            auto masked = static_cast<XMLMASKEDTEXTURE*>(child);
            const char* name = this->GetName();
            if (name == nullptr) {
                name = this->GetObjectTypeName();
            }
            if ((masked->m_maskFlags & 1) != 0) {
                lua_State* L = FrameScript_GetContext();
                bool restricted = !((this->FrameScript_Object::m_flags & 8) == 0 && (this->FrameScript_Object::m_flags & 0x20) != 0);
                lua_pushstring(L, masked->m_target.m_text);
                lua_rawget(L, LUA_GLOBALSINDEX);
                auto target = static_cast<CSimpleTexture*>(
                    FrameScript_Object::GetObjectFromLua(nullptr, L, CSimpleMaskTexture::GetObjectType(), -1, false, restricted));
                lua_settop(L, -2);
                if (target == nullptr) {
                    status->Add(2, "Texture %s: Can't find texture to be masked %s", name, masked->m_target.m_text);
                } else {
                    target->AddMaskTexture(this);
                }
            } else if ((masked->m_maskFlags & 2) != 0) {
                auto frame = static_cast<CScriptObject*>(this->GetParent());
                CScriptObject* target = frame != nullptr ? frame->GetObjectByPath(masked->m_childKey.m_text) : nullptr;
                if (target != nullptr && target->IsObjectTypeName("Texture")) {
                    static_cast<CSimpleTexture*>(target)->AddMaskTexture(this);
                    continue;
                }
                status->Add(2, "Texture %s: Can't find child texture to be masked %s", name, masked->m_childKey.m_text);
            }
        }
    }
}

bool CSimpleMaskTexture::IsMaskTexture() {
    return true;
}

// The masked textures' coordinates follow the mask.
// ref: FUN_1406386e0
void CSimpleMaskTexture::OnFrameSizeChanged(const CRect* rect, const CRect* previous) {
    this->CSimpleTexture::OnFrameSizeChanged(rect, previous);
    if (this->m_extension != nullptr) {
        for (CSimpleTexture* masked : this->m_extension->m_masked) {
            masked->UpdateMaskTexCoords();
        }
    }
}
