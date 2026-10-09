#ifndef UI_TEXTURE_ATLAS_HPP
#define UI_TEXTURE_ATLAS_HPP

// The UI texture atlases (Engine/Source/Frame/TextureAtlas*.cpp, 0x140602880..): named
// elements of the UiTextureAtlasElement, UiTextureAtlasMember and UiTextureAtlas tables, each a
// rect of an atlas file, picked by element and layout scale.

#include <cstdint>

// The layout version an atlas entry is picked for, when set.
struct TEXTUREATLASSCALE {
    int32_t m_version;
    uint8_t m_set;
};

// The member of an element name (FUN_140602c00) or element id (FUN_140602b70) for a scale; 0
// when the tables have none.
int32_t* TextureAtlasFindByName(int32_t* member, const char* name, TEXTUREATLASSCALE* scale);
int32_t* TextureAtlasFindByElement(int32_t* member, int32_t element, TEXTUREATLASSCALE* scale);
int32_t* TextureAtlasFindByMember(int32_t* member, int32_t id, TEXTUREATLASSCALE* scale);
// The member's element (FUN_140602aa0), its atlas file (FUN_140602b20), its size in texels
// (FUN_140602880), its path when it has one (FUN_140603040) and whether it tiles
// (FUN_1406035a0, FUN_1406035e0).
int32_t TextureAtlasGetElement(const int32_t* member);
// The member's element's name, empty without one (FUN_140602fe0).
const char* TextureAtlasGetName(const int32_t* member);
int32_t TextureAtlasGetFileDataId(const int32_t* member);
void TextureAtlasGetSize(const int32_t* member, float* size);
bool TextureAtlasGetPath(const int32_t* member, char* path, uint64_t size);
bool TextureAtlasTilesHorizontally(const int32_t* member);
bool TextureAtlasTilesVertically(const int32_t* member);
// Texture coordinates into the member's rect of its atlas (FUN_1406031f0) and back
// (FUN_1406033a0).
void TextureAtlasMapTexCoords(const int32_t* member, float* out, const float* texCoords);
void TextureAtlasUnmapTexCoords(const int32_t* member, float* out, const float* texCoords);

#endif
