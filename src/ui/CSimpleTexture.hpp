#ifndef UI_C_SIMPLE_TEXTURE_HPP
#define UI_C_SIMPLE_TEXTURE_HPP

// CSimpleTexture (Engine/Source/Frame/CSimpleTexture.cpp): the textured quad regions are drawn
// with ("Texture", 0x2a0 bytes; vtable 0x1426cbf58 with the CLayoutFrame vtable 0x1426cc250 at
// +0x18 and the CAnimatableObject vtable 0x1426cc348 at +0x1d8). It holds the texture, the
// four corners and their texture coordinates, the blend mode, the rotation and desaturation,
// an atlas entry, and (in an extension made on demand) vertex offsets, mask textures and raw
// pixel data.

#include <cstdint>

#include "blz/Vector.hpp"
#include "ui/CSimpleRegion.hpp"

class CSimpleMaskTexture;
class CSimpleTexture;
class CTexture;
struct XMLOBJECT;
class CStatus;

extern const char s_maskedTextureTag[];
extern const char s_maskTextureTag[];

// TSGrowableArray<CArgb> (0x20 bytes).
struct ARGBARRAY {
    uint32_t m_count;               // 0x00
    uint32_t m_pad04;
    uint32_t* m_data;               // 0x08
    uint32_t m_capacity;            // 0x10
    uint32_t m_pad14;
    uint32_t m_chunk;               // 0x18
    uint32_t m_pad1c;
};
static_assert(sizeof(ARGBARRAY) == 0x20, "TSGrowableArray<CArgb> is 0x20 bytes");

// CTextureExtension (0xf8, CDataAllocator 0x142e99d30): what few textures need.
struct CTextureExtension {
    // 1 a file to be masked (m_maskFile), 2 raw pixels, bits 2-4 horizontal wrap, 5-7 vertical
    // wrap, 8-10 filter (the settings the masked texture is made with).
    uint32_t m_flags;               // 0x00
    int32_t m_maskFile;             // 0x04
    int32_t m_fileDataId;           // 0x08
    float m_vertexOffsets[4][3];    // 0x0c per corner
    uint32_t m_pad3c;
    ARGBARRAY m_pixels;             // 0x40
    CTexture* m_pixelTexture;       // 0x60
    blz::vector<CSimpleTexture*, s_maskedTextureTag> m_masked;      // 0x68 what this mask masks
    blz::vector<CSimpleMaskTexture*, s_maskTextureTag> m_masks;     // 0x80 the masks on this one
    float m_maskTexCoords[3][8];    // 0x98
};

class CSimpleTexture : public CSimpleRegion {
    public:
    // m_textureFlags.
    enum : uint32_t {
        TEXTURE_BLOCKING = 0x1,             // drawn once resident, before it is loaded
        TEXTURE_TILE_DIRTY = 0x2,           // the tiled coordinates need the texture's size
        TEXTURE_HORIZ_TILE = 0x4,
        TEXTURE_VERT_TILE = 0x8,
        TEXTURE_WRAP_H = 0x70,
        TEXTURE_WRAP_V = 0x380,
        TEXTURE_FILTER = 0x1c00,
        TEXTURE_2000 = 0x2000,
        TEXTURE_4000 = 0x4000,
        TEXTURE_8000 = 0x8000,
        TEXTURE_10000 = 0x10000,
        TEXTURE_SNAP_TO_PIXEL_GRID = 0x20000,
    };

    CTextureExtension* m_extension; // 0x218
    CTexture* m_texture;            // 0x220
    uint8_t m_blendMode;            // 0x228
    uint8_t m_pad229[3];
    int32_t m_atlas;                // 0x22c the atlas entry, 0 for none
    int32_t m_fileDataId;           // 0x230 the file asked for
    float m_positions[4][3];        // 0x234 top left, bottom left, top right, bottom right
    float m_desaturation;           // 0x264
    float m_snappingBias;           // 0x268
    float m_rotation;               // 0x26c radians
    float m_rotationCenter[2];      // 0x270 0..1 across the rect
    float m_texCoords[4][2];        // 0x278
    uint32_t m_textureFlags;        // 0x298
    uint32_t m_pad29c;

    // The textures alive (DAT_142e99d28).
    static int64_t s_count;
    // The registry references of the metatables (DAT_142cf54cc, DAT_142cf54d0).
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;
    // Textures are drawn only once loaded unless this is set (DAT_142e99d2c).
    static uint8_t s_drawWhileLoading;

    // ref: FUN_140631b10
    CSimpleTexture(CSimpleFrame* frame, uint8_t drawLayer, int8_t subLevel, bool show);
    // ref: FUN_140631c70
    ~CSimpleTexture() override;
    // Textures come from their own pool (0x142e99d70).
    static void* operator new(size_t size);
    static void operator delete(void* texture);

    static void CreateScriptMetaTables();
    static void DestroyScriptMetaTables();
    // This class's type id (FUN_140616420, 0x142e97b60).
    static int32_t GetObjectType();

    // FrameScript_Object, CScriptObject and CScriptRegion.
    int32_t GetScriptMetaTable() override;
    bool IsObjectTypeName(const char* name) override;
    bool IsObjectType(int32_t type) override;
    const char* GetObjectTypeName() override;
    uint32_t GetObjectSize() override;
    void DumpHierarchy(uint32_t indent, HIERARCHYDUMPFUNC dump, void* context) override;
    void Unknown98(void* argument) override;
    void LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) override;
    void PostLoadXML(XMLOBJECT* node, CStatus* status) override;
    void OnLayoutVersionChanged() override;

    // CSimpleRegion.
    void UpdateEffectiveAlpha() override;
    void AddToBatch(CRenderBatch* batch) override;
    void Unknown2C0() override;

    // The texture's own slots (0x2d8..0x2f0).
    virtual bool IsMaskTexture();
    // The corners of `rect` with the vertex offsets and the rotation.
    virtual void ComputePositions(const CRect* rect, float* positions);
    virtual float GetPixelWidth();
    virtual float GetPixelHeight();

    // CLayoutFrame.
    void ScaleBy(float* scale) override;
    float GetWidth() override;
    float GetHeight() override;
    bool IsLayoutReady() override;
    void OnLayoutReady() override;
    void ApplyHide() override;
    void ApplyShow() override;
    void OnLayoutDepthChanged() override;
    void OnFrameSizeChanged(const CRect* rect, const CRect* previous) override;
    void ClearAnimTransform() override;
    void ClearAnimRectOffset() override;

    // CAnimatableObject.
    void Unknown08() override;
    void OnAnimStarted(bool alpha, bool transform, bool rectOffset) override;
    void Unknown40(void* value) override;
    void Unknown50(void* value) override;
    void Unknown58(int32_t index, void* value, float radians) override;
    void Unknown60(int32_t index, void* first, void* second) override;
    void Unknown78(float progress, uint32_t rows, uint32_t columns, uint32_t frames, uint32_t frameWidth,
        uint32_t frameHeight) override;

    // ---- the texture -----------------------------------------------------------------------
    // A solid colour instead of a file (FUN_1406354e0).
    bool SetColorTexture(const uint32_t* argb);
    // The file of a FileDataID, with its wrap and filter (FUN_140635630); false when it is not
    // a texture.
    bool SetTextureById(int32_t fileDataId, uint32_t wrapH, uint32_t wrapV, uint8_t filter, bool resetTexCoords,
        bool force);
    // The same by path (FUN_140635960).
    bool SetTextureByName(const char* name, uint32_t wrapH, uint32_t wrapV, uint8_t filter, bool resetTexCoords,
        bool force);
    // A texture the caller made (FUN_1406358c0).
    bool SetTextureHandle(CTexture* texture, bool keepMasks);
    // An atlas entry (FUN_140634740).
    bool SetAtlas(const int32_t* atlas, uint8_t filter, bool useAtlasSize, uint8_t wrapH, uint8_t wrapV);
    // Pixels the caller has, width by height ARGB (FUN_140635c70).
    bool SetPixels(const uint32_t* pixels, int32_t width, int32_t height);
    // The texture's FileDataID, the one asked for while it loads (FUN_140632e90).
    int32_t GetFileDataId();
    // Its path, false when it has none (FUN_140632ef0).
    bool GetTextureName(char* buffer, uint64_t size);
    // Whether the texture is the file (FUN_140631a40).
    bool IsTextureFile(int32_t fileDataId);
    // The file's texture is made (FUN_1406332a0).
    void LoadFile(int32_t fileDataId);
    // Lets the texture go (FUN_140632c30).
    void ReleaseTexture();
    // The texture's Gx texture, waiting when it loads (FUN_140632c90).
    void* GetGxTex();
    // Whether the texture is ready to draw, the tiled coordinates brought up to date
    // (FUN_140621360).
    bool PrepareToDraw();

    // ---- coordinates and corners --------------------------------------------------------------
    // Sets the coordinates through the atlas when there is one (FUN_140635460).
    void SetTexCoords(const float* texCoords);
    // As left, right, top, bottom (FUN_1406353c0).
    void SetTexCoordsRect(const float* rect);
    // The stored coordinates, with the running animation (FUN_140635190).
    void StoreTexCoords(const float* texCoords);
    // The coordinates as the caller set them (FUN_140632e10) and as stored (FUN_140632da0).
    void GetTexCoords(float* texCoords);
    void GetStoredTexCoords(float* texCoords);
    // Leaves the atlas, keeping the coordinates it gave (FUN_140634490).
    void ClearAtlas();
    // ref: FUN_140636960
    void SetVertexOffset(uint32_t corner, const float* x, const float* y);
    // ref: FUN_140632fa0
    void GetVertexOffset(uint32_t corner, float* x, float* y);
    // ref: FUN_140635340
    void SetRotation(float radians, const float* center);
    // ref: FUN_1406349b0
    void SetDesaturation(float desaturation);
    // ref: FUN_1406354b0
    void SetSnappingBias(float bias);
    // ref: FUN_140634920
    void SetBlendMode(uint8_t mode);
    // The corners recomputed for `rect` (FUN_140637160), and after a size change (FUN_140637370).
    void UpdatePositions(const CRect* rect);
    void UpdatePositionsAndTiling();

    // ---- masks -------------------------------------------------------------------------------
    void EnsureExtension();
    void ReleaseExtensionIfUnused();
    // ref: FUN_140631f20
    void AddMaskTexture(CSimpleMaskTexture* mask);
    // ref: FUN_140634520
    void RemoveMaskTexture(CSimpleMaskTexture* mask);
    // ref: FUN_140632d30
    CSimpleMaskTexture* GetMaskTexture(uint32_t index);
    // ref: FUN_140632d70
    uint64_t GetNumMaskTextures();
    // The mask coordinates for each mask on the texture (FUN_140636d40), and for the textures
    // this one masks (FUN_1406370d0).
    void UpdateMaskTexCoords();
    void UpdateMasks();
    // ref: FUN_140634b30
    void SetMaskFile(const char* name);
    // ref: FUN_140633070
    bool HasMaskFile();
    // The masked texture from the mask file's alpha and the texture's pixels (FUN_140635f60).
    bool BuildMaskedTexture(const uint8_t* flag, const uint8_t* pixels, int32_t pitch);
};
static_assert(sizeof(CSimpleTexture) == 0x2a0, "CSimpleTexture is 0x2a0 bytes");

// CSimpleMaskTexture ("MaskTexture", 0x2a0; vtable 0x1426cefd8, CLayoutFrame 0x1426cf2d0,
// CAnimatableObject 0x1426cf3c8): a texture whose alpha masks the textures it is added to.
class CSimpleMaskTexture : public CSimpleTexture {
    public:
    // The metatables (DAT_142cf54d4, DAT_142cf54d8).
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;

    CSimpleMaskTexture(CSimpleFrame* frame, uint8_t drawLayer, int8_t subLevel, bool show);
    // ref: FUN_140667800
    ~CSimpleMaskTexture() override;
    // Masks come from their own pool (0x142e99da0).
    static void* operator new(size_t size);
    static void operator delete(void* mask);

    // ref: FUN_1406383a0
    static void CreateScriptMetaTables();
    // ref: FUN_1406383f0
    static void DestroyScriptMetaTables();
    // ref: FUN_140638440
    static int32_t GetObjectType();

    int32_t GetScriptMetaTable() override;
    bool IsObjectTypeName(const char* name) override;
    bool IsObjectType(int32_t type) override;
    const char* GetObjectTypeName() override;
    void LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) override;
    bool IsMaskTexture() override;
    void OnFrameSizeChanged(const CRect* rect, const CRect* previous) override;
};

// The vertex-rotation helpers the texture animations use (FUN_140637eb0 ...).
void TextureAnimOrigin(float* origin, int32_t point, const float* offset, const float* positions);

#endif
