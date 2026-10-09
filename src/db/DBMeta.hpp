#ifndef DB_DB_META_HPP
#define DB_DB_META_HPP

// The generated description of a client table (one per table, 0xb0 bytes, in .rdata next to the
// table's name). The first field arrays describe the record as the client lays it out in
// memory; the second describe the file's columns.

#include <cstdint>

// A unique integer key: the field, and a value that is exempt from the check.
struct DBMETAINTKEY {
    int32_t m_field;                // 0x00
    uint8_t m_hasIgnore;            // 0x04
    uint8_t m_pad05[3];
    int32_t m_ignore;               // 0x08
    int32_t m_pad0c;
};

// A unique string key.
struct DBMETASTRINGKEY {
    uint32_t m_field;               // 0x00
    uint8_t m_hasIgnore;            // 0x04
    uint8_t m_pad05[3];
    const char* m_ignore;           // 0x08
};

using DBMETAORDER = bool (*)(const void* left, const void* right);
using DBMETAORDERPTR = bool (*)(const void* const* left, const void* const* right);

// Field types (both arrays): 0 int32, 1 int64, 2 string, 3 float, 4 int8, 5 int16.
// Field flags: 1 indexed (a foreign key), 2 packed in the file, 4 unsigned.
struct DBMeta {
    const char* m_name;             // 0x00
    int32_t m_fileDataId;           // 0x08
    int32_t m_fileFieldCount;       // 0x0c
    int32_t m_recordSize;           // 0x10
    int32_t m_fieldCount;           // 0x14
    int32_t m_indexField;           // 0x18 -1: the ids come from the file's id list
    uint8_t m_sparse;               // 0x1c
    uint8_t m_pad1d[3];
    const int32_t* m_fieldOffsets;  // 0x20
    const int32_t* m_fieldSizes;    // 0x28
    const int32_t* m_fieldTypes;    // 0x30
    const int32_t* m_fieldFlags;    // 0x38
    const int32_t* m_fileFieldSizes;  // 0x40
    const int32_t* m_fileFieldTypes;  // 0x48
    const int32_t* m_fileFieldFlags;  // 0x50
    int32_t m_indexedFieldCount;    // 0x58
    uint32_t m_tableHash;           // 0x5c
    int32_t m_60;
    uint32_t m_layoutHash;          // 0x64
    int32_t m_stringFieldCount;     // 0x68
    int32_t m_intKeyCount;          // 0x6c
    int32_t m_stringKeyCount;       // 0x70
    int32_t m_pad74;
    const DBMETAINTKEY* m_intKeys;  // 0x78
    const DBMETASTRINGKEY* m_stringKeys;  // 0x80
    uint8_t m_hasParent;            // 0x88
    uint8_t m_pad89[3];
    int32_t m_parentFileField;      // 0x8c
    int32_t m_parentField;          // 0x90
    int32_t m_pad94;
    DBMETAORDER m_order;            // 0x98 orders a parent's or a key's records
    DBMETAORDERPTR m_orderPtr;      // 0xa0 orders the table's iteration
    uint8_t m_zeroIdValid;          // 0xa8 an id of 0 is a record (else ~0 is the invalid id)
    uint8_t m_a9;
    uint8_t m_padaa[6];
};
static_assert(sizeof(DBMeta) == 0xb0, "a table meta is 0xb0 bytes");

#endif
