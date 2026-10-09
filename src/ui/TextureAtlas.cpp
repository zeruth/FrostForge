#include "ui/TextureAtlas.hpp"

#include <cmath>
#include <cstring>

#include "util/Unimplemented.hpp"

namespace {

// UiTextureAtlasMember's record as the client keeps it.
struct UITEXTUREATLASMEMBERREC {
    uint8_t m_pad00[8];
    uint16_t m_atlas;               // 0x08
    int16_t m_left;                 // 0x0a
    int16_t m_right;                // 0x0c
    int16_t m_top;                  // 0x0e
    int16_t m_bottom;               // 0x10
    uint16_t m_element;             // 0x12
    int16_t m_width;                // 0x14 overrides the rect's when positive
    int16_t m_height;               // 0x16
    uint8_t m_flags;                // 0x18 2 tiles vertically, 4 horizontally
    uint8_t m_scale;                // 0x19
};

// UiTextureAtlas's record.
struct UITEXTUREATLASREC {
    int32_t m_fileDataId;           // 0x00
    uint16_t m_width;               // 0x04
    uint16_t m_height;              // 0x06
    uint8_t m_scale;                // 0x08
};

// The rows of UiTextureAtlasMember (DAT_142e88c30) and UiTextureAtlas (DAT_142e88e40), through
// WowClientDB's record lookup (FUN_14057b640).
const UITEXTUREATLASMEMBERREC* MemberRecord(int32_t id) {
    (void)id;
    FROST_UNIMPLEMENTED();  // the client database (WowClientDB2) is not ported yet
    return nullptr;
}

const UITEXTUREATLASREC* AtlasRecord(uint16_t id) {
    (void)id;
    FROST_UNIMPLEMENTED();  // the client database (WowClientDB2) is not ported yet
    return nullptr;
}

// Whether the atlas lookups are enabled (DAT_142e97a90).
uint8_t s_atlasesLoaded;

}

// ref: FUN_140602c00
int32_t* TextureAtlasFindByName(int32_t* member, const char* name, TEXTUREATLASSCALE* scale) {
    (void)name;
    if (s_atlasesLoaded != 0) {
        FROST_UNIMPLEMENTED();  // the element by name (FUN_14051e820) and the member pick (FUN_140602c80)
    }
    *member = 0;
    scale->m_set = 0;
    return member;
}

// ref: FUN_140602b70
int32_t* TextureAtlasFindByElement(int32_t* member, int32_t element, TEXTUREATLASSCALE* scale) {
    (void)element;
    if (s_atlasesLoaded != 0) {
        FROST_UNIMPLEMENTED();  // the element record and the member pick (FUN_140602c80)
    }
    *member = 0;
    scale->m_set = 0;
    return member;
}

// ref: FUN_140602aa0
int32_t TextureAtlasGetElement(const int32_t* member) {
    const UITEXTUREATLASMEMBERREC* record = MemberRecord(*member);
    return record != nullptr ? record->m_element : 0;
}

// ref: FUN_140602b20
int32_t TextureAtlasGetFileDataId(const int32_t* member) {
    const UITEXTUREATLASMEMBERREC* record = MemberRecord(*member);
    if (record != nullptr) {
        const UITEXTUREATLASREC* atlas = AtlasRecord(record->m_atlas);
        if (atlas != nullptr) {
            return atlas->m_fileDataId;
        }
    }
    return 0;
}

// The size in texels: the record's override or its rect, against the atlas's scale.
// ref: FUN_140602880
void TextureAtlasGetSize(const int32_t* member, float* size) {
    const UITEXTUREATLASMEMBERREC* record = MemberRecord(*member);
    if (record == nullptr) {
        size[0] = 1.0f;
        size[1] = 1.0f;
        return;
    }
    int32_t width = record->m_width > 0 ? record->m_width : record->m_right - record->m_left;
    int32_t height = record->m_height > 0 ? record->m_height : record->m_bottom - record->m_top;
    size[0] = static_cast<float>(width);
    size[1] = static_cast<float>(height);
    FROST_UNIMPLEMENTED();  // the scale record (DAT_142e86920) and its divisors (FUN_14022b390)
}

// ref: FUN_140603040
bool TextureAtlasGetPath(const int32_t* member, char* path, uint64_t size) {
    (void)member;
    (void)size;
    *path = '\0';
    return false;
}

// ref: FUN_1406035a0
bool TextureAtlasTilesHorizontally(const int32_t* member) {
    const UITEXTUREATLASMEMBERREC* record = MemberRecord(*member);
    return record != nullptr && (record->m_flags & 4) != 0;
}

// ref: FUN_1406035e0
bool TextureAtlasTilesVertically(const int32_t* member) {
    const UITEXTUREATLASMEMBERREC* record = MemberRecord(*member);
    return record != nullptr && (record->m_flags & 2) != 0;
}

// ref: FUN_1406031f0
void TextureAtlasMapTexCoords(const int32_t* member, float* out, const float* texCoords) {
    const UITEXTUREATLASMEMBERREC* record = MemberRecord(*member);
    const UITEXTUREATLASREC* atlas = nullptr;
    const UITEXTUREATLASMEMBERREC* other = MemberRecord(*member);
    if (other != nullptr) {
        atlas = AtlasRecord(other->m_atlas);
    }
    if (record != nullptr && atlas != nullptr) {
        float scaleU = 1.0f / static_cast<float>(atlas->m_width);
        float spanU = static_cast<float>(record->m_right - record->m_left) * scaleU;
        float scaleV = 1.0f / static_cast<float>(atlas->m_height);
        float originU = static_cast<float>(record->m_left) * scaleU;
        float originV = static_cast<float>(record->m_top) * scaleV;
        float spanV = static_cast<float>(record->m_bottom - record->m_top) * scaleV;
        for (uint32_t i = 0; i < 4; i++) {
            out[i * 2] = spanU * texCoords[i * 2] + originU;
            out[i * 2 + 1] = spanV * texCoords[i * 2 + 1] + originV;
        }
        return;
    }
    const float full[8] = { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f };
    std::memcpy(out, full, sizeof(full));
}

// ref: FUN_1406033a0
void TextureAtlasUnmapTexCoords(const int32_t* member, float* out, const float* texCoords) {
    const UITEXTUREATLASMEMBERREC* record = MemberRecord(*member);
    const UITEXTUREATLASREC* atlas = nullptr;
    const UITEXTUREATLASMEMBERREC* other = MemberRecord(*member);
    if (other != nullptr) {
        atlas = AtlasRecord(other->m_atlas);
    }
    const float full[8] = { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f };
    std::memcpy(out, full, sizeof(full));
    if (record == nullptr || atlas == nullptr) {
        return;
    }
    float scaleU = 1.0f / static_cast<float>(atlas->m_width);
    float scaleV = 1.0f / static_cast<float>(atlas->m_height);
    float spanU = static_cast<float>(record->m_right - record->m_left) * scaleU;
    float originU = static_cast<float>(record->m_left) * scaleU;
    float originV = static_cast<float>(record->m_top) * scaleV;
    if (6.0e-36f < std::fabs(spanU)) {
        float spanV = static_cast<float>(record->m_bottom - record->m_top) * scaleV;
        if (6.0e-36f < std::fabs(spanV)) {
            float inverseV = 1.0f / spanV;
            float inverseU = 1.0f / spanU;
            for (uint32_t i = 0; i < 4; i++) {
                out[i * 2] = (texCoords[i * 2] - originU) * inverseU;
                out[i * 2 + 1] = (texCoords[i * 2 + 1] - originV) * inverseV;
            }
        }
    }
}
