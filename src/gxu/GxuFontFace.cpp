// Gxu font faces (Engine/Source/Gxu/GxuFontFace.cpp): a font file's bytes and its FreeType
// face, shared by file data id through a hash table and handed out as HFACE handles.

#include <new>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "cas/CAS.hpp"
#include "gxu/GxuFontPrivate.hpp"
#include "storm/Memory.hpp"

template <>
struct TSHashTableTags<FACEDATA>;

namespace {

const char s_faceFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Gxu\\GxuFontFace.cpp";
const char s_faceTableTag[] = ".?AV?$TSHashTable@UFACEDATA@@VHASHKEY_DWORD@@$07@@";
const char s_faceListTag[] = ".?AV?$TSList@UFACEDATA@@V?$TSGetExplicitLink@UFACEDATA@@@@$07@@";
const char s_faceArrayTag[] = ".?AV?$TSGrowableArray@PEAUHFACE__@@$07@@";
const char s_faceFixedArrayTag[] = ".?AV?$TSFixedArray@PEAUHFACE__@@$07@@";

// TSHashTable<FACEDATA, HASHKEY_DWORD, 8> (vtable 0x1426d2b78; the static at 0x142cf5eb0).
class FACETABLE : public TSHashTable<FACEDATA, 0x10> {
    public:
    // ref: FUN_140691d70
    void Delete(FACEDATA* node) override {
        node->~FACEDATA();
        SMemFree(node, s_faceTableTag, -2, 0);
    }

    // ref: FUN_140691da0
    FACEDATA* NewNode(TSExplicitList<FACEDATA>* slot, uint32_t extra, uint32_t flags) override {
        void* memory = SMemAlloc(extra + sizeof(FACEDATA), 8, s_faceListTag, 0, -2, flags | 8);
        auto node = new (memory) FACEDATA();
        slot->LinkToTail(node);
        return node;
    }
};

FACETABLE s_faceTable;

// TSGrowableArray<HFACE, 8> (0x142e9cfb8): a reference to every face made, closed at shutdown.
struct FACEARRAY {
    uint32_t m_count = 0;
    CHandleObject** m_data = nullptr;
    uint32_t m_capacity = 0;
    uint32_t m_granularity = 0;
};
FACEARRAY s_faces;

// The face table's hash of a file data id: FNV-1a over its four bytes.
uint32_t FaceHash(uint32_t key) {
    uint32_t hash = 0x811c9dc5;
    for (int32_t i = 0; i < 4; i++) {
        hash = (hash ^ ((key >> (i * 8)) & 0xff)) * 0x1000193;
    }
    return hash;
}

FACEDATA* FindFace(uint32_t fileDataId) {
    uint32_t hash = FaceHash(fileDataId);
    return s_faceTable.Find(hash, [fileDataId](FACEDATA* face) {
        return face->m_fileDataId == fileDataId;
    });
}

// TSGrowableArray<HFACE, 8>::Add (FUN_1406931b0).
// ref: FUN_1406931b0
void AddFace(CHandleObject* face) {
    uint32_t count = s_faces.m_count + 1;
    if (s_faces.m_capacity < count) {
        uint32_t granularity = s_faces.m_granularity;
        if (granularity == 0) {
            if (count < 0x20) {
                granularity = count;
                for (uint32_t low = s_faces.m_count & count; low != 0; low = (low - 1) & low) {
                    granularity = low;
                }
                if (granularity == 0) {
                    granularity = 1;
                }
            } else {
                s_faces.m_granularity = 0x20;
                granularity = 0x20;
            }
        }
        if (count % granularity != 0) {
            count += granularity - count % granularity;
        }
        CHandleObject** old = s_faces.m_data;
        s_faces.m_capacity = count;
        s_faces.m_data = static_cast<CHandleObject**>(SMemAlloc(static_cast<uint64_t>(count) * 8, 8, s_faceArrayTag, 0, -7, 0));
        if (old != nullptr) {
            uint32_t keep = s_faces.m_count <= count ? s_faces.m_count : count;
            for (uint32_t i = 0; i < keep; i++) {
                s_faces.m_data[i] = old[i];
            }
            SMemFree(old, s_faceArrayTag, -2, 0);
        }
    }
    s_faces.m_data[s_faces.m_count++] = face;
}

// A face over a file's bytes, with its Unicode charmap, entered in the table (FUN_1406929f0).
// ref: FUN_1406929f0
FACEDATA* FaceCreate(void* library, void* data, uint32_t size, uint32_t fileDataId) {
    FT_Face face = nullptr;
    if (FT_New_Memory_Face(static_cast<FT_Library>(library), static_cast<const FT_Byte*>(data),
            static_cast<FT_Long>(size), 0, &face) != 0 || face == nullptr) {
        return nullptr;
    }
    if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) != 0) {
        return nullptr;
    }
    void* memory = SMemAlloc(sizeof(FACEDATA), 1, "HFACE", 0, -2, 0);
    auto node = new (memory) FACEDATA();
    node->m_data = data;
    node->m_face = face;
    uint32_t hash = FaceHash(fileDataId);
    s_faceTable.Insert(node, hash);
    node->m_hashval = hash;
    node->m_fileDataId = fileDataId;
    return node;
}

// A face from bytes the caller loaded, which it takes; an existing face keeps its own.
// ref: FUN_140692b20
CHandleObject* FaceCreateFromBuffer(uint32_t fileDataId, void* data, void* library, uint32_t size) {
    FACEDATA* face = FindFace(fileDataId);
    if (face != nullptr) {
        SMemFree(data, s_faceFile, 0x48, 0);
    } else {
        face = FaceCreate(library, data, size, fileDataId);
        if (face == nullptr) {
            SMemFree(data, s_faceFile, 0x4c, 0);
            return nullptr;
        }
        AddFace(HandleDuplicate(face));
    }
    return HandleDuplicate(face);
}

}

// ref: FUN_140692230
FACEDATA::~FACEDATA() {
    if (this->m_face != nullptr) {
        FT_Done_Face(this->m_face);
    }
    if (this->m_data != nullptr) {
        cas::CASFileFree(this->m_data);
    }
    // FUN_140692000: out of the table and its lists.
    if (this->m_table != nullptr && this->m_linktoslot.IsLinked()) {
        static_cast<FACETABLE*>(this->m_table)->Unlink(this);
    }
    this->m_linktofull.Unlink();
    this->m_linktoslot.Unlink();
}

// The face of a font file, loading it the first time.
// ref: FUN_140692320
CHandleObject* GxuFontFaceCreateById(uint32_t fileDataId, void* library) {
    if (library == nullptr || fileDataId == 0) {
        return nullptr;
    }
    FACEDATA* face = FindFace(fileDataId);
    if (face == nullptr) {
        uint8_t* data = nullptr;
        uint32_t size = 0;
        if (!cas::CASFileLoadById(static_cast<int32_t>(fileDataId), &data, &size, 1, 0x1b)) {
            return nullptr;
        }
        face = FaceCreate(library, data, size, fileDataId);
        if (face == nullptr) {
            cas::CASFileFree(data);
            return nullptr;
        }
        AddFace(HandleDuplicate(face));
    }
    return HandleDuplicate(face);
}

// ref: FUN_140692480
CHandleObject* GxuFontFaceCreateByName(const char* path, void* library) {
    if (path == nullptr || *path == '\0') {
        return nullptr;
    }
    uint32_t fileDataId = static_cast<uint32_t>(cas::CASFileGetIdByName(path, 1));
    if (library == nullptr || fileDataId == 0) {
        return nullptr;
    }
    return GxuFontFaceCreateById(fileDataId, library);
}

// A face over bytes the caller has, by the file's name.
// ref: FUN_140692620
CHandleObject* GxuFontFaceCreateFromBuffer(const char* path, void* library, void* data, uint32_t size) {
    if (path == nullptr || *path == '\0') {
        return nullptr;
    }
    uint32_t fileDataId = static_cast<uint32_t>(cas::CASFileGetIdByName(path, 1));
    return FaceCreateFromBuffer(fileDataId, data, library, size);
}

// ref: FUN_140692310
FT_FaceRec_* GxuFontFaceGetFace(CHandleObject* face) {
    return static_cast<FACEDATA*>(face)->m_face;
}

// ref: FUN_140692680
FT_FaceRec_* GxuFontFaceFindById(uint32_t fileDataId) {
    if (fileDataId == 0) {
        return nullptr;
    }
    FACEDATA* face = FindFace(fileDataId);
    return face != nullptr ? face->m_face : nullptr;
}

// ref: FUN_140692740
FT_FaceRec_* GxuFontFaceFindByName(const char* path) {
    if (path == nullptr || *path == '\0') {
        return nullptr;
    }
    uint32_t fileDataId = static_cast<uint32_t>(cas::CASFileGetIdByName(path, 1));
    if (fileDataId == 0) {
        return nullptr;
    }
    FACEDATA* face = FindFace(fileDataId);
    return face != nullptr ? face->m_face : nullptr;
}

// ref: FUN_140692840
bool GxuFontFaceExistsById(uint32_t fileDataId) {
    return fileDataId != 0 && FindFace(fileDataId) != nullptr;
}

// ref: FUN_140692900
bool GxuFontFaceExistsByName(const char* path) {
    if (path == nullptr || *path == '\0') {
        return false;
    }
    uint32_t fileDataId = static_cast<uint32_t>(cas::CASFileGetIdByName(path, 1));
    return fileDataId != 0 && FindFace(fileDataId) != nullptr;
}

// Closes the module's reference to every face (TSFixedArray<HFACE, 8> cleared).
// ref: FUN_1406922a0
void GxuFontFaceCloseAll() {
    for (uint32_t i = 0; i < s_faces.m_count; i++) {
        HandleClose(s_faces.m_data[i]);
    }
    if (s_faces.m_data != nullptr) {
        SMemFree(s_faces.m_data, s_faceFixedArrayTag, -2, 0);
    }
    s_faces.m_capacity = 0;
    s_faces.m_count = 0;
    s_faces.m_data = nullptr;
}
