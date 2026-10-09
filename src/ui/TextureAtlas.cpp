#include "ui/TextureAtlas.hpp"

#include <cmath>
#include <cstring>

#include "db/DB2Tables.hpp"
#include "db/WowDB2Lookup.hpp"
#include "ui/CSimpleTop.hpp"
#include "ui/UITypes.hpp"
#include "util/Unimplemented.hpp"

namespace {

// UiTextureAtlasMember's record (UiTextureAtlasMember's meta: 26 bytes, unpacked).
struct UITEXTUREATLASMEMBERREC {
    int32_t m_committedName;        // 0x00 a string offset
    int32_t m_id;                   // 0x04
    uint16_t m_atlas;               // 0x08
    int16_t m_left;                 // 0x0a
    int16_t m_right;                // 0x0c
    int16_t m_top;                  // 0x0e
    int16_t m_bottom;               // 0x10
    uint16_t m_element;             // 0x12
    int16_t m_width;                // 0x14 overrides the rect's when positive
    int16_t m_height;               // 0x16
    uint8_t m_flags;                // 0x18 2 tiles vertically, 4 horizontally
    uint8_t m_canvas;               // 0x19 0: the atlas's
};

// UiTextureAtlas's record.
struct UITEXTUREATLASREC {
    int32_t m_fileDataId;           // 0x00
    uint16_t m_width;               // 0x04
    uint16_t m_height;              // 0x06
    uint8_t m_canvas;               // 0x08
};

// UiTextureAtlasElement's record: its name (a string offset) and id.
struct UITEXTUREATLASELEMENTREC {
    int32_t m_name;                 // 0x00
    int32_t m_id;                   // 0x04
};

const UITEXTUREATLASMEMBERREC* MemberRecord(int32_t id) {
    bool local;
    return static_cast<const UITEXTUREATLASMEMBERREC*>(g_uiTextureAtlasMemberDB.GetRecord(static_cast<uint32_t>(id), false, &local));
}

const UITEXTUREATLASREC* AtlasRecord(uint16_t id) {
    bool local;
    return static_cast<const UITEXTUREATLASREC*>(g_uiTextureAtlasDB.GetRecord(id, false, &local));
}

// A UiCanvas record's dimension (FUN_14022b390 on the table FUN_14051c440 returns).
int32_t CanvasDimension(int32_t canvas, uint32_t field) {
    bool local;
    auto record = static_cast<const uint8_t*>(g_uiCanvasDB.GetRecord(static_cast<uint32_t>(canvas), false, &local));
    if (record == nullptr) {
        return 0;
    }
    int64_t offset = g_uiCanvasDB.m_meta->m_fieldOffsets[field];
    return DB2FieldI16(record + offset, field, offset, &g_uiCanvasDB);
}

// The canvas a member is drawn for: its own, else its atlas's, else 1.
int32_t MemberCanvas(int32_t id, const UITEXTUREATLASMEMBERREC* member) {
    if (member->m_canvas != 0) {
        return member->m_canvas;
    }
    const UITEXTUREATLASMEMBERREC* again = MemberRecord(id);
    if (again != nullptr) {
        const UITEXTUREATLASREC* atlas = AtlasRecord(again->m_atlas);
        if (atlas != nullptr) {
            return atlas->m_canvas;
        }
    }
    return 1;
}

// ref: FUN_140602c80: the element's member for the canvas: an exact match, else the one whose
// canvas height is closest from below (taller ones rank by their negated height).
int32_t* PickMember(int32_t* member, const UITEXTUREATLASELEMENTREC* element, TEXTUREATLASSCALE* scale) {
    int32_t elementId = element->m_id;
    int32_t canvas = scale->m_set != 0 ? scale->m_version : CSimpleTop::s_instance->m_layoutVersion;
    bool local;
    int32_t wantedHeight = g_uiCanvasDB.GetRecord(static_cast<uint32_t>(canvas), false, &local) != nullptr
        ? CanvasDimension(canvas, 2)
        : 1;
    *member = 0;
    WowDB2ParentLookup* parents = g_uiTextureAtlasMemberDB.m_parentLookup;
    if (parents == nullptr) {
        scale->m_set = 0;
        return member;
    }
    int32_t best = static_cast<int32_t>(0x80000000);
    DB2PARENTITER it;
    DB2PARENTITER end;
    parents->Begin(&it, elementId);
    parents->End(&end);
    while (it.m_entry != end.m_entry || it.m_overflow != end.m_overflow || it.m_parent != end.m_parent || it.m_source != end.m_source) {
        auto candidate = static_cast<const UITEXTUREATLASMEMBERREC*>(parents->Record(&it));
        int32_t id = candidate->m_id;
        const UITEXTUREATLASMEMBERREC* record = MemberRecord(id);
        if (record != nullptr) {
            int32_t memberCanvas = MemberCanvas(id, record);
            if (memberCanvas == canvas) {
                *member = id;
                scale->m_set = 0;
                return member;
            }
            int32_t height = g_uiCanvasDB.GetRecord(static_cast<uint32_t>(memberCanvas), false, &local) != nullptr
                ? CanvasDimension(memberCanvas, 2)
                : 1;
            int32_t rank = height <= wantedHeight ? height : -height;
            if (best < rank) {
                *member = id;
                best = rank;
            }
        }
        parents->Next(&it);
    }
    scale->m_set = 0;
    return member;
}

}

// ref: FUN_140602c00
int32_t* TextureAtlasFindByName(int32_t* member, const char* name, TEXTUREATLASSCALE* scale) {
    if (g_uiDatabaseReady) {
        // FUN_14051e820
        auto element = static_cast<const UITEXTUREATLASELEMENTREC*>(g_uiTextureAtlasElementDB.GetRecordByStringKey(name, 0));
        if (element != nullptr) {
            TEXTUREATLASSCALE pick = { 0, scale->m_set };
            if (scale->m_set != 0) {
                pick.m_version = scale->m_version;
            }
            PickMember(member, element, &pick);
            scale->m_set = 0;
            return member;
        }
    }
    *member = 0;
    scale->m_set = 0;
    return member;
}

// ref: FUN_140602b70
int32_t* TextureAtlasFindByElement(int32_t* member, int32_t element, TEXTUREATLASSCALE* scale) {
    if (g_uiDatabaseReady) {
        bool local;
        auto record = static_cast<const UITEXTUREATLASELEMENTREC*>(g_uiTextureAtlasElementDB.GetRecord(static_cast<uint32_t>(element), false, &local));
        if (record != nullptr) {
            TEXTUREATLASSCALE pick = { 0, scale->m_set };
            if (scale->m_set != 0) {
                pick.m_version = scale->m_version;
            }
            PickMember(member, record, &pick);
            scale->m_set = 0;
            return member;
        }
    }
    *member = 0;
    scale->m_set = 0;
    return member;
}

// The member of an atlas member id, through its element.
// ref: FUN_140602ee0
int32_t* TextureAtlasFindByMember(int32_t* member, int32_t id, TEXTUREATLASSCALE* scale) {
    const UITEXTUREATLASMEMBERREC* record = g_uiDatabaseReady ? MemberRecord(id) : nullptr;
    if (record == nullptr) {
        *member = 0;
        scale->m_set = 0;
        return member;
    }
    if (g_uiDatabaseReady) {
        bool local;
        auto element = static_cast<const UITEXTUREATLASELEMENTREC*>(g_uiTextureAtlasElementDB.GetRecord(record->m_element, false, &local));
        if (element != nullptr) {
            TEXTUREATLASSCALE pick = { 0, scale->m_set };
            if (scale->m_set != 0) {
                pick.m_version = scale->m_version;
            }
            PickMember(member, element, &pick);
            scale->m_set = 0;
            return member;
        }
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

// The size in UI units: the record's override or its rect, scaled from its canvas to
// 1024x768.
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
    float x = static_cast<float>(width);
    float y = static_cast<float>(height);
    int32_t canvas = MemberCanvas(*member, record);
    bool local;
    if (g_uiCanvasDB.GetRecord(static_cast<uint32_t>(canvas), false, &local) != nullptr) {
        x = x * 1024.0f / static_cast<float>(CanvasDimension(canvas, 1));
        y = y * 768.0f / static_cast<float>(CanvasDimension(canvas, 2));
    }
    size[0] = x;
    size[1] = y;
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

// ref: FUN_140602fe0
const char* TextureAtlasGetName(const int32_t* member) {
    const UITEXTUREATLASMEMBERREC* record = MemberRecord(*member);
    if (record != nullptr) {
        bool local;
        auto element = static_cast<const int32_t*>(g_uiTextureAtlasElementDB.GetRecord(record->m_element, false, &local));
        if (element != nullptr && *element != 0) {
            return reinterpret_cast<const char*>(element) + *element;
        }
    }
    return "";
}
